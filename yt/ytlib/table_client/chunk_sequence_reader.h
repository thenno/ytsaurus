﻿#pragma once

#include "common.h"
#include "reader.h"
#include "chunk_reader.h"

#include "../chunk_client/retriable_reader.h"
#include "../transaction_client/transaction.h"
#include "../misc/async_stream_state.h"

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TChunkSequenceReader
    : public IAsyncReader
{
public:
    typedef TIntrusivePtr<TChunkSequenceReader> TPtr;

    struct TConfig
        : public TConfigBase
    {
        typedef TIntrusivePtr<TConfig> TPtr;

        NChunkClient::TRetriableReader::TConfig::TPtr RetriableReader;
        NChunkClient::TSequentialReader::TConfig::TPtr SequentialReader;

        TConfig()
        {
            Register("retriable_reader", RetriableReader).Default(New<NChunkClient::TRetriableReader::TConfig>());
            Register("sequential_reader", SequentialReader).Default(New<NChunkClient::TSequentialReader::TConfig>());
        }
    };

    TChunkSequenceReader(
        TConfig* config,
        const TChannel& channel,
        const NTransactionClient::TTransactionId transactionId,
        NRpc::IChannel::TPtr masterChannel,
        // ToDo: use rvalue reference.
        yvector<NChunkClient::TChunkId>& chunks,
        int startRow,
        int endRow);

    TAsyncError::TPtr AsyncOpen();

    bool HasNextRow() const;

    TAsyncError::TPtr AsyncNextRow();

    bool NextColumn();

    TValue GetValue();

    TColumn GetColumn() const;

    void Cancel(const TError& error);

private:
    void PrepareNextChunk();
    void OnNextReaderOpened(
        TError error, 
        TChunkReader::TPtr reader);
    void SetCurrentChunk(TChunkReader::TPtr nextReader);
    void OnNextRow(TError error);


    const TConfig::TPtr Config;
    const TChannel Channel;
    const NTransactionClient::TTransactionId TransactionId;
    const yvector<NChunkClient::TChunkId> Chunks;
    const int StartRow;
    const int EndRow;

    NRpc::IChannel::TPtr MasterChannel;

    TAsyncStreamState State;

    int NextChunkIndex;
    TFuture<TChunkReader::TPtr>::TPtr NextReader;
    TChunkReader::TPtr CurrentReader;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
