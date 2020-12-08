package yt

import (
	"fmt"
	"io/ioutil"
	"os"
	"os/user"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/xerrors"

	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/library/go/core/log/nop"
	zaplog "a.yandex-team.ru/library/go/core/log/zap"
)

const (
	DefaultLightRequestTimeout = 5 * time.Minute
	DefaultTxTimeout           = 15 * time.Second
	DefaultTxPingPeriod        = 3 * time.Second
)

type Config struct {
	// Proxy configures address of YT HTTP proxy.
	//
	// If Proxy is not set, value of YT_PROXY environment variable is used instead.
	//
	// May be equal to cluster name. E.g. hahn or markov.
	//
	// May be equal to hostname with optional port. E.g. localhost:12345 or sas5-1547-proxy-hahn.sas.yp-c.yandex.net.
	// In that case, provided host is used for all requests and proxy discovery is disabled.
	Proxy string

	// Token configures OAuth token used by the client.
	//
	// If Token is not set, value of YT_TOKEN environment variable is used instead.
	Token string

	// ReadTokenFromFile
	//
	// When this variable is set, client tries reading token from ~/.yt/token file.
	ReadTokenFromFile bool

	// Logger overrides default logger, used by the client.
	//
	// When Logger is not set, logging behaviour is configured by YT_LOG_LEVEL environment variable.
	//
	// If YT_LOG_LEVEL is not set, no logging is performed. Otherwise logs are written to stderr,
	// with log level derived from value of YT_LOG_LEVEL variable.
	//
	// WARNING: Running YT client in production without debug logs is highly discouraged.
	Logger log.Structured

	// LightRequestTimeout specifies default timeout for light requests. Timeout includes all retries and backoffs.
	// Timeout for single request is not configurable right now.
	//
	// A Timeout of zero means no timeout. Client can still specify timeout on per-request basis using context.
	//
	// nil value means default timeout of 5 minutes.
	LightRequestTimeout *time.Duration

	// TxTimeout specifies timeout of YT transaction (both master and tablet).
	//
	// YT transaction is aborted by server after not receiving pings from client for TxTimeout seconds.
	//
	// TxTimeout of zero means default timeout of 15 seconds.
	TxTimeout time.Duration

	// TxPingPeriod specifies period of pings for YT transactions.
	//
	// TxPingPeriod of zero means default value of 3 seconds.
	TxPingPeriod time.Duration
}

func (c *Config) GetProxy() (string, error) {
	if c.Proxy != "" {
		return c.Proxy, nil
	}

	if proxy := os.Getenv("YT_PROXY"); proxy != "" {
		return proxy, nil
	}

	return "", xerrors.New("YT proxy is not set (either Config.Proxy or YT_PROXY must be set)")
}

func (c *Config) GetToken() string {
	if c.Token != "" {
		return c.Token
	}

	if token := os.Getenv("YT_TOKEN"); token != "" {
		return token
	}

	if c.ReadTokenFromFile {
		u, err := user.Current()
		if err != nil {
			return ""
		}

		token, err := ioutil.ReadFile(filepath.Join(u.HomeDir, ".yt", "token"))
		if err != nil {
			return ""
		}

		return strings.Trim(string(token), "\n")
	}

	return ""
}

func (c *Config) GetLogger() log.Structured {
	if c.Logger != nil {
		return c.Logger
	}

	logLevel := os.Getenv("YT_LOG_LEVEL")
	if logLevel == "" {
		return (&nop.Logger{}).Structured()
	}

	lvl, err := log.ParseLevel(logLevel)
	if err != nil {
		lvl = log.DebugLevel
	}

	config := zaplog.CLIConfig(lvl)
	config.OutputPaths = []string{"stderr"}

	l, err := zaplog.New(config)
	if err != nil {
		panic(fmt.Sprintf("failed to configure default logger: %+v", err))
	}
	return l.Structured()
}

func (c *Config) GetLightRequestTimeout() time.Duration {
	if c.LightRequestTimeout != nil {
		return *c.LightRequestTimeout
	}

	return DefaultLightRequestTimeout
}

func (c *Config) GetTxTimeout() time.Duration {
	if c.TxTimeout == 0 {
		return DefaultTxTimeout
	}

	return c.TxTimeout
}

func (c *Config) GetTxPingPeriod() time.Duration {
	if c.TxPingPeriod == 0 {
		return DefaultTxPingPeriod
	}

	return c.TxPingPeriod
}

type ClusterURL struct {
	Address          string
	DisableDiscovery bool
}

func NormalizeProxyURL(proxy string) ClusterURL {
	const prefix = "http://"
	const suffix = ".yt.yandex.net"

	var url ClusterURL
	if !strings.Contains(proxy, ".") && !strings.Contains(proxy, ":") && !strings.Contains(proxy, "localhost") {
		proxy += suffix
	}

	if strings.ContainsAny(proxy, "0123456789") {
		url.DisableDiscovery = true
	}

	proxy = strings.TrimPrefix(proxy, prefix)
	url.Address = proxy
	return url
}
