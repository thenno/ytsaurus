#pragma once

#include "public.h"

#include <ytlib/misc/error.h>

#include <ytlib/ytree/yson_serializable.h>

#include <ytlib/compression/public.h>
#include <ytlib/erasure/public.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

class TRemoteReaderConfig
    : public virtual TYsonSerializable
{
public:
    //! Timeout for a block request.
    TDuration NodeRpcTimeout;

    //! Time to wait before asking the master for seeds.
    TDuration RetryBackoffTime;

    //! Maximum number of attempts to fetch new seeds.
    int RetryCount;

    //! Time to wait before making another pass with same seeds.
    TDuration PassBackoffTime;

    //! Maximum number of passes with same seeds.
    int PassCount;

    //! Enable fetching blocks from peers suggested by seeds.
    bool FetchFromPeers;

    //! Timeout after which a node forgets about the peer.
    //! Only makes sense if the reader is equipped with peer descriptor.
    TDuration PeerExpirationTimeout;

    //! If True then fetched blocks are cached by the node.
    bool EnableNodeCaching;

    //! If True then the master may be asked for seeds.
    bool AllowFetchingSeedsFromMaster;

    TRemoteReaderConfig()
    {
        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("retry_backoff_time", RetryBackoffTime)
            .Default(TDuration::Seconds(3));
        RegisterParameter("retry_count", RetryCount)
            .Default(20);
        RegisterParameter("pass_backoff_time", PassBackoffTime)
            .Default(TDuration::Seconds(3));
        RegisterParameter("pass_count", PassCount)
            .Default(500);
        RegisterParameter("fetch_from_peers", FetchFromPeers)
            .Default(true);
        RegisterParameter("peer_expiration_timeout", PeerExpirationTimeout)
            .Default(TDuration::Seconds(300));
        RegisterParameter("enable_node_caching", EnableNodeCaching)
            .Default(true);
        RegisterParameter("allow_fetching_seeds_from_master", AllowFetchingSeedsFromMaster)
            .Default(true);
    }
};

///////////////////////////////////////////////////////////////////////////////

class TClientBlockCacheConfig
    : public virtual TYsonSerializable
{
public:
    //! The maximum number of bytes that block are allowed to occupy.
    //! Zero means that no blocks are cached.
    i64 MaxSize;

    TClientBlockCacheConfig()
    {
        RegisterParameter("max_size", MaxSize)
            .Default(0)
            .GreaterThanOrEqual(0);
    }
};

///////////////////////////////////////////////////////////////////////////////

class TSequentialReaderConfig
    : public virtual TYsonSerializable
{
public:
    //! Prefetch window size (in bytes).
    i64 WindowSize;

    //! Maximum amount of data to be transfered via a single RPC request.
    i64 GroupSize;

    TSequentialReaderConfig()
    {
        RegisterParameter("window_size", WindowSize)
            .Default(64 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("group_size", GroupSize)
            .Default(8 * 1024 * 1024)
            .GreaterThan(0);

        RegisterValidator([&] () {
            if (GroupSize > WindowSize) {
                THROW_ERROR_EXCEPTION("\"group_size\" cannot be larger than \"window_size\"");
            }
        });
    }
};

///////////////////////////////////////////////////////////////////////////////

class TReplicationWriterConfig
    : public virtual TYsonSerializable
{
public:
    //! Maximum window size (in bytes).
    i64 SendWindowSize;

    //! Maximum group size (in bytes).
    i64 GroupSize;

    //! RPC requests timeout.
    /*!
     *  This timeout is especially useful for PutBlocks calls to ensure that
     *  uploading is not stalled.
     */
    TDuration NodeRpcTimeout;

    //! Maximum allowed period of time without RPC requests to nodes.
    /*!
     *  If the writer remains inactive for the given period, it sends #TChunkHolderProxy::PingSession.
     */
    TDuration NodePingInterval;

    //! If True then written blocks are cached by the node.
    bool EnableNodeCaching;

    TReplicationWriterConfig()
    {
        RegisterParameter("send_window_size", SendWindowSize)
            .Default(4 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("group_size", GroupSize)
            .Default(1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("node_ping_interval", NodePingInterval)
            .Default(TDuration::Seconds(10));
        RegisterParameter("enable_node_caching", EnableNodeCaching)
            .Default(false);

        RegisterValidator([&] () {
            if (SendWindowSize < GroupSize) {
                THROW_ERROR_EXCEPTION("\"window_size\" cannot be less than \"group_size\"");
            }
        });
    }
};

///////////////////////////////////////////////////////////////////////////////

class TErasureWriterConfig
    : public virtual TYsonSerializable
{
public:
    i64 ErasureWindowSize;

    TErasureWriterConfig()
    {
        RegisterParameter("erasure_window_size", ErasureWindowSize)
            .Default(1024 * 1024)
            .GreaterThan(0);
    }
};

///////////////////////////////////////////////////////////////////////////////

class TEncodingWriterConfig
    : public virtual TYsonSerializable
{
public:
    i64 EncodeWindowSize;
    double DefaultCompressionRatio;
    bool VerifyCompression;

    TEncodingWriterConfig()
    {
        RegisterParameter("encode_window_size", EncodeWindowSize)
            .Default(4 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("default_compression_ratio", DefaultCompressionRatio)
            .Default(0.2);
        RegisterParameter("verify_compression", VerifyCompression)
            .Default(true);
    }
};

///////////////////////////////////////////////////////////////////////////////

struct TEncodingWriterOptions
    : public virtual TYsonSerializable
{
    NCompression::ECodec Codec;

    TEncodingWriterOptions()
    {
        RegisterParameter("compression_codec", Codec)
            .Default(NCompression::ECodec::None);
    }
};

///////////////////////////////////////////////////////////////////////////////

class TDispatcherConfig
    : public virtual TYsonSerializable
{
public:
    int CompressionPoolSize;
    int ErasurePoolSize;

    TDispatcherConfig()
    {
        RegisterParameter("compression_pool_size", CompressionPoolSize)
            .Default(4)
            .GreaterThan(0);
        RegisterParameter("erasure_pool_size", ErasurePoolSize)
            .Default(4)
            .GreaterThan(0);
    }
};

///////////////////////////////////////////////////////////////////////////////

class TMultiChunkWriterConfig
    : public TReplicationWriterConfig
    , public TErasureWriterConfig
{
public:
    i64 DesiredChunkSize;
    i64 MaxMetaSize;

    int UploadReplicationFactor;

    bool ChunksMovable;
    bool ChunksVital;

    bool PreferLocalHost;

    NErasure::ECodec ErasureCodec;

    TMultiChunkWriterConfig()
    {
        RegisterParameter("desired_chunk_size", DesiredChunkSize)
            .GreaterThan(0)
            .Default(1024 * 1024 * 1024);
        RegisterParameter("max_meta_size", MaxMetaSize)
            .GreaterThan(0)
            .LessThanOrEqual(64 * 1024 * 1024)
            .Default(30 * 1024 * 1024);
        RegisterParameter("upload_replication_factor", UploadReplicationFactor)
            .GreaterThanOrEqual(1)
            .Default(2);
        RegisterParameter("chunks_movable", ChunksMovable)
            .Default(true);
        RegisterParameter("chunks_vital", ChunksVital)
            .Default(true);
        RegisterParameter("prefer_local_host", PreferLocalHost)
            .Default(true);
        RegisterParameter("erasure_codec", ErasureCodec)
            .Default(NErasure::ECodec::None);
    }
};

///////////////////////////////////////////////////////////////////////////////

struct TMultiChunkWriterOptions
    : public virtual TEncodingWriterOptions
{
    int ReplicationFactor;
    Stroka Account;

    TMultiChunkWriterOptions()
    {
        RegisterParameter("replication_factor", ReplicationFactor)
            .GreaterThanOrEqual(1)
            .Default(3);
        RegisterParameter("account", Account)
            .NonEmpty();
    }
};

///////////////////////////////////////////////////////////////////////////////

struct TMultiChunkReaderConfig
    : public virtual TRemoteReaderConfig
    , public virtual TSequentialReaderConfig
{
    i64 MaxBufferSize;

    TMultiChunkReaderConfig()
    {
        RegisterParameter("max_buffer_size", MaxBufferSize)
            .GreaterThan(0L)
            .LessThanOrEqual(10L * 1024 * 1024 * 1024)
            .Default(256L * 1024 * 1024);

        RegisterValidator([&] () {
            if (MaxBufferSize < 2 * WindowSize) {
                THROW_ERROR_EXCEPTION("\"max_buffer_size\" cannot be less than twice \"window_size\"");
            }
        });
    }
};

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
