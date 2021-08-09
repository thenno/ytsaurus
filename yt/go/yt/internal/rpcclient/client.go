package rpcclient

import (
	"context"
	"crypto/tls"
	"net/http"
	"time"

	"github.com/cenkalti/backoff/v4"
	"github.com/golang/protobuf/proto"

	"a.yandex-team.ru/library/go/certifi"
	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/library/go/core/xerrors"
	"a.yandex-team.ru/yt/go/bus"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/internal"
)

var _ yt.Client = (*client)(nil)

type client struct {
	Encoder

	conf           *yt.Config
	httpClusterURL yt.ClusterURL
	rpcClusterURL  yt.ClusterURL
	token          string

	log log.Structured

	// httpClient is used to retrieve available proxies.
	httpClient *http.Client
	proxySet   *internal.ProxySet

	connPool ConnPool
	stop     *internal.StopGroup
}

func NewClient(conf *yt.Config) (*client, error) {
	c := &client{
		conf:           conf,
		httpClusterURL: yt.NormalizeProxyURL(conf.Proxy),
		rpcClusterURL:  yt.NormalizeProxyURL(conf.RPCProxy),
		log:            conf.GetLogger(),
		stop:           internal.NewStopGroup(),
	}

	certPool, err := certifi.NewCertPool()
	if err != nil {
		return nil, err
	}

	c.httpClient = &http.Client{
		Transport: &http.Transport{
			MaxIdleConns:        0,
			MaxIdleConnsPerHost: 100,
			IdleConnTimeout:     30 * time.Second,

			TLSHandshakeTimeout: 10 * time.Second,
			TLSClientConfig: &tls.Config{
				RootCAs: certPool,
			},
		},
		Timeout: 60 * time.Second,
	}

	if token := conf.GetToken(); token != "" {
		c.token = token
	}

	c.proxySet = &internal.ProxySet{UpdateFn: c.listRPCProxies}

	c.connPool = NewLRUConnPool(func(ctx context.Context, addr string) (*bus.ClientConn, error) {
		clientOpts := []bus.ClientOption{
			bus.WithLogger(c.log.Logger()),
			bus.WithDefaultProtocolVersionMajor(1),
		}
		return bus.NewClient(ctx, addr, clientOpts...)
	}, connPoolSize)

	c.Encoder.StartCall = c.startCall
	c.Encoder.Invoke = c.do
	c.Encoder.InvokeReadRow = c.doReadRow

	proxyBouncer := &ProxyBouncer{Log: c.log, ProxySet: c.proxySet, ConnPool: c.connPool}
	requestLogger := &LoggingInterceptor{Structured: c.log}
	mutationRetrier := &MutationRetrier{Log: c.log}
	readRetrier := &Retrier{Config: c.conf, Log: c.log}
	errorWrapper := &ErrorWrapper{}

	c.Encoder.Invoke = c.Encoder.Invoke.
		Wrap(proxyBouncer.Intercept).
		Wrap(requestLogger.Intercept).
		Wrap(mutationRetrier.Intercept).
		Wrap(readRetrier.Intercept).
		Wrap(errorWrapper.Intercept)

	return c, nil
}

func (c *client) schema() string {
	schema := "http"
	if c.conf.UseTLS {
		schema = "https"
	}
	return schema
}

func (c *client) do(
	ctx context.Context,
	call *Call,
	rsp proto.Message,
) error {
	return c.invoke(ctx, call, rsp)
}

func (c *client) doReadRow(
	ctx context.Context,
	call *Call,
	rsp ProtoRowset,
) (yt.TableReader, error) {
	var rspAttachments [][]byte

	err := c.invoke(ctx, call, rsp, bus.WithResponseAttachments(&rspAttachments))
	if err != nil {
		return nil, err
	}

	rows, err := decodeFromWire(rspAttachments)
	if err != nil {
		err := xerrors.Errorf("unable to decode response from wire format: %w", err)
		return nil, err
	}

	return newTableReader(rows, rsp.GetRowsetDescriptor())
}

func (c *client) invoke(
	ctx context.Context,
	call *Call,
	rsp proto.Message,
	opts ...bus.SendOption,
) error {
	addr := call.RequestedProxy
	if addr == "" {
		var err error
		addr, err = c.pickRPCProxy(ctx)
		if err != nil {
			return err
		}
	}
	call.SelectedProxy = addr

	conn, err := c.connPool.Conn(ctx, addr)
	if err != nil {
		return err
	}
	c.log.Debug("got bus conn", log.String("fqdn", addr))

	opts = append(opts,
		bus.WithRequestID(call.CallID),
		bus.WithToken(c.token),
	)
	if call.Attachments != nil {
		opts = append(opts, bus.WithAttachments(call.Attachments...))
	}

	return conn.Send(ctx, "ApiService", string(call.Method), call.Req, rsp, opts...)
}

func (c *client) Stop() {
	_ = c.connPool.Close()
	c.stop.Stop()
}

func (c *client) startCall() *Call {
	bf := backoff.NewExponentialBackOff()
	bf.MaxElapsedTime = c.conf.GetLightRequestTimeout()
	return &Call{
		Backoff: bf,
	}
}

// LockRows wraps encoder's implementation with transaction.
func (c *client) LockRows(
	ctx context.Context,
	path ypath.Path,
	locks []string,
	lockType yt.LockType,
	keys []interface{},
	opts *yt.LockRowsOptions,
) (err error) {
	if opts == nil {
		opts = &yt.LockRowsOptions{}
	}
	if opts.TransactionOptions == nil {
		opts.TransactionOptions = &yt.TransactionOptions{}
	}

	var zero yt.TxID
	if opts.TransactionID != zero {
		return c.Encoder.LockRows(ctx, path, locks, lockType, keys, opts)
	}

	txID, err := c.StartTabletTx(ctx, nil)
	if err != nil {
		return err
	}
	defer c.AbortTx(ctx, txID, nil)

	opts.TransactionID = txID

	err = c.Encoder.LockRows(ctx, path, locks, lockType, keys, opts)
	if err != nil {
		return err
	}

	return c.CommitTx(ctx, txID, nil)
}

// InsertRows wraps encoder's implementation with transaction.
func (c *client) InsertRows(
	ctx context.Context,
	path ypath.Path,
	rows []interface{},
	opts *yt.InsertRowsOptions,
) (err error) {
	if opts == nil {
		opts = &yt.InsertRowsOptions{}
	}
	if opts.TransactionOptions == nil {
		opts.TransactionOptions = &yt.TransactionOptions{}
	}

	var zero yt.TxID
	if opts.TransactionID != zero {
		return c.Encoder.InsertRows(ctx, path, rows, opts)
	}

	txID, err := c.StartTabletTx(ctx, nil)
	if err != nil {
		return err
	}
	defer c.AbortTx(ctx, txID, nil)

	opts.TransactionID = txID

	err = c.Encoder.InsertRows(ctx, path, rows, opts)
	if err != nil {
		return err
	}

	return c.CommitTx(ctx, txID, nil)
}

// DeleteRows wraps encoder's implementation with transaction.
func (c *client) DeleteRows(
	ctx context.Context,
	path ypath.Path,
	keys []interface{},
	opts *yt.DeleteRowsOptions,
) (err error) {
	if opts == nil {
		opts = &yt.DeleteRowsOptions{}
	}
	if opts.TransactionOptions == nil {
		opts.TransactionOptions = &yt.TransactionOptions{}
	}

	var zero yt.TxID
	if opts.TransactionID != zero {
		return c.Encoder.DeleteRows(ctx, path, keys, opts)
	}

	txID, err := c.StartTabletTx(ctx, nil)
	if err != nil {
		return err
	}
	defer c.AbortTx(ctx, txID, nil)

	opts.TransactionID = txID

	err = c.Encoder.DeleteRows(ctx, path, keys, opts)
	if err != nil {
		return err
	}

	return c.CommitTx(ctx, txID, nil)
}
