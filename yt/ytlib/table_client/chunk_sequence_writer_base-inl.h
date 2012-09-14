#ifndef CHUNK_SEQUENCE_WRITER_BASE_INL_H_
#error "Direct inclusion of this file is not allowed, include chunk_sequence_writer_base.h"
#endif
#undef CHUNK_SEQUENCE_WRITER_BASE_INL_H_

#include "private.h"
#include "schema.h"

#include <ytlib/misc/string.h>
#include <ytlib/misc/address.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>

#include <ytlib/chunk_client/chunk_list_ypath_proxy.h>
#include <ytlib/chunk_client/chunk_ypath_proxy.h>
#include <ytlib/chunk_client/dispatcher.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

template <class TChunkWriter>
TChunkSequenceWriterBase<TChunkWriter>::TChunkSequenceWriterBase(
    TTableWriterConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    const NObjectClient::TTransactionId& transactionId,
    const NChunkClient::TChunkListId& parentChunkList,
    const TNullable<TKeyColumns>& keyColumns)
    : Config(config)
    , MasterChannel(masterChannel)
    , TransactionId(transactionId)
    , ParentChunkList(parentChunkList)
    , KeyColumns(keyColumns)
    , RowCount(0)
    , Progress(0)
    , CompleteChunkSize(0)
    , NextSession(Null)
    , CloseChunksAwaiter(New<TParallelAwaiter>(NChunkClient::TDispatcher::Get()->GetWriterInvoker()))
    , Logger(TableWriterLogger)
{
    YASSERT(config);
    YASSERT(masterChannel);

    // TODO(babenko): use TTaggedLogger here, tag with parentChunkListId.
}

template <class TChunkWriter>
TChunkSequenceWriterBase<TChunkWriter>::~TChunkSequenceWriterBase()
{ }

template <class TChunkWriter>
bool TChunkSequenceWriterBase<TChunkWriter>::TryWriteRow(const TRow& row)
{
    if (!CurrentSession.ChunkWriter) {
        return false;
    }

    if (!CurrentSession.ChunkWriter->TryWriteRow(row)) {
        return false;
    }

    OnRowWritten();

    return true;
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::CreateNextSession()
{
    YASSERT(NextSession.IsNull());

    NextSession = NewPromise<TSession>();

    LOG_DEBUG("Creating chunk (TransactionId: %s, ReplicationFactor: %d, UploadReplicationFactor: %d)",
        ~TransactionId.ToString(),
        Config->ReplicationFactor,
        Config->UploadReplicationFactor);

    NObjectClient::TObjectServiceProxy objectProxy(MasterChannel);

    auto req = NTransactionClient::TTransactionYPathProxy::CreateObject(
        NCypressClient::FromObjectId(TransactionId));
    req->set_type(NObjectClient::EObjectType::Chunk);

    auto* reqExt = req->MutableExtension(NChunkClient::NProto::TReqCreateChunkExt::create_chunk);
    if (Config->PreferLocalHost) {
        reqExt->set_preferred_host_name(Stroka(GetLocalHostName()));
    }
    reqExt->set_replication_factor(Config->ReplicationFactor);
    reqExt->set_upload_replication_factor(Config->UploadReplicationFactor);
    reqExt->set_movable(Config->ChunksMovable);

    objectProxy.Execute(req).Subscribe(
        BIND(&TChunkSequenceWriterBase::OnChunkCreated, MakeWeak(this))
        .Via(NChunkClient::TDispatcher::Get()->GetWriterInvoker()));
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::OnChunkCreated(
    NTransactionClient::TTransactionYPathProxy::TRspCreateObjectPtr rsp)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(!NextSession.IsNull());

    if (!State.IsActive()) {
        return;
    }

    if (!rsp->IsOK()) {
        State.Fail(rsp->GetError());
        return;
    }

    auto chunkId = NChunkClient::TChunkId::FromProto(rsp->object_id());
    const auto& rspExt = rsp->GetExtension(NChunkClient::NProto::TRspCreateChunkExt::create_chunk);
    auto addresses = FromProto<Stroka>(rspExt.node_addresses());

    if (addresses.size() < Config->UploadReplicationFactor) {
        State.Fail(TError("Not enough data nodes available"));
        return;
    }

    LOG_DEBUG("Chunk created (Addresses: [%s], ChunkId: %s)",
        ~JoinToString(addresses),
        ~chunkId.ToString());

    TSession session;
    session.RemoteWriter = New<NChunkClient::TRemoteWriter>(
        Config,
        chunkId,
        addresses);
    session.RemoteWriter->Open();

    PrepareChunkWriter(&session);

    NextSession.Set(session);
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::SetProgress(double progress)
{
    Progress = progress;
}

template <class TChunkWriter>
TAsyncError TChunkSequenceWriterBase<TChunkWriter>::AsyncOpen()
{
    YASSERT(!State.HasRunningOperation());

    CreateNextSession();

    State.StartOperation();
    NextSession.Subscribe(BIND(
        &TChunkSequenceWriterBase::InitCurrentSession,
        MakeWeak(this)));

    return State.GetOperationError();
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::InitCurrentSession(TSession nextSession)
{
    VERIFY_THREAD_AFFINITY_ANY();

    CurrentSession = nextSession;

    NextSession.Reset();
    CreateNextSession();

    State.FinishOperation();
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::OnRowWritten()
{
    VERIFY_THREAD_AFFINITY_ANY();

    ++RowCount;

    if (CurrentSession.ChunkWriter->GetMetaSize() > Config->MaxMetaSize) {
        LOG_DEBUG("Switching to next chunk: meta is too large (TransactionId: %s, ChunkMetaSize: %" PRId64,
            ~TransactionId.ToString(),
            CurrentSession.ChunkWriter->GetMetaSize());

        SwitchSession();
        return;
    }


    if (CurrentSession.ChunkWriter->GetCurrentSize() > Config->DesiredChunkSize) 
    {
        i64 currentDataSize = CompleteChunkSize + CurrentSession.ChunkWriter->GetCurrentSize();
        auto expectedInputSize = currentDataSize * std::max(.0, 1. - Progress);

        if (expectedInputSize > Config->DesiredChunkSize || 
            CurrentSession.ChunkWriter->GetCurrentSize() > 2 * Config->DesiredChunkSize) 
        {
            LOG_DEBUG("Switching to next chunk: too much data (TransactionId: %s, currentSessionSize: %" PRId64 ", ExpectedInputSize: %lf)",
                ~TransactionId.ToString(),
                CurrentSession.ChunkWriter->GetCurrentSize(),
                expectedInputSize);

            SwitchSession();
            return;
        }
    }
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::SwitchSession()
{
    State.StartOperation();
    YASSERT(!NextSession.IsNull());
    // We're not waiting for chunk to be closed.
    FinishCurrentSession();
    NextSession.Subscribe(BIND(
        &TChunkSequenceWriterBase::InitCurrentSession,
        MakeWeak(this)));
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::FinishCurrentSession()
{
    if (CurrentSession.IsNull())
        return;

    if (CurrentSession.ChunkWriter->GetCurrentSize() > 0) {
        LOG_DEBUG("Finishing chunk (ChunkId: %s)",
            ~CurrentSession.RemoteWriter->GetChunkId().ToString());

        auto finishResult = NewPromise<TError>();
        CloseChunksAwaiter->Await(finishResult.ToFuture(), BIND(
            &TChunkSequenceWriterBase::OnChunkFinished, 
            MakeWeak(this),
            CurrentSession.RemoteWriter->GetChunkId()));

        CurrentSession.ChunkWriter->AsyncClose().Subscribe(BIND(
            &TChunkSequenceWriterBase::OnChunkClosed,
            MakeWeak(this),
            CurrentSession,
            finishResult));

    } else {
        LOG_DEBUG("Canceling empty chunk (ChunkId: %s)",
            ~CurrentSession.RemoteWriter->GetChunkId().ToString());
    }

    CurrentSession.Reset();
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::OnChunkClosed(
    TSession currentSession,
    TAsyncErrorPromise finishResult,
    TError error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!error.IsOK()) {
        finishResult.Set(error);
        return;
    }

    auto remoteWriter = currentSession.RemoteWriter;
    auto chunkWriter = currentSession.ChunkWriter;

    CompleteChunkSize += chunkWriter->GetCurrentSize();

    LOG_DEBUG("Chunk successfully closed (ChunkId: %s)",
        ~remoteWriter->GetChunkId().ToString());

    NObjectClient::TObjectServiceProxy objectProxy(MasterChannel);
    auto batchReq = objectProxy.ExecuteBatch();
    {
        auto req = NChunkClient::TChunkYPathProxy::Confirm(
            NCypressClient::FromObjectId(remoteWriter->GetChunkId()));
        *req->mutable_chunk_info() = remoteWriter->GetChunkInfo();
        ToProto(req->mutable_node_addresses(), remoteWriter->GetNodeAddresses());
        *req->mutable_chunk_meta() = chunkWriter->GetMasterMeta();

        batchReq->AddRequest(req);
    }
    {
        auto req = NChunkClient::TChunkListYPathProxy::Attach(
            NCypressClient::FromObjectId(ParentChunkList));
        *req->add_children_ids() = remoteWriter->GetChunkId().ToProto();

        batchReq->AddRequest(req);
    }
    {
        auto req = NTransactionClient::TTransactionYPathProxy::ReleaseObject(
            NCypressClient::FromObjectId(TransactionId));
        *req->mutable_object_id() = remoteWriter->GetChunkId().ToProto();

        batchReq->AddRequest(req);
    }

    {
        NProto::TInputChunk inputChunk;

        auto* slice = inputChunk.mutable_slice();
        slice->mutable_start_limit();
        slice->mutable_end_limit();
        *slice->mutable_chunk_id() = remoteWriter->GetChunkId().ToProto();

        auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(chunkWriter->GetMasterMeta().extensions());
        inputChunk.set_uncompressed_data_size(miscExt.uncompressed_data_size());
        inputChunk.set_row_count(miscExt.row_count());

        ToProto(inputChunk.mutable_node_addresses(), remoteWriter->GetNodeAddresses());
        *inputChunk.mutable_channel() = TChannel::CreateUniversal().ToProto();
        *inputChunk.mutable_extensions() = chunkWriter->GetSchedulerMeta().extensions();

        TGuard<TSpinLock> guard(WrittenChunksGuard);
        WrittenChunks.push_back(inputChunk);
    }

    batchReq->Invoke().Subscribe(BIND(
        &TChunkSequenceWriterBase::OnChunkRegistered,
        MakeWeak(this),
        remoteWriter->GetChunkId(),
        finishResult));
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::OnChunkRegistered(
    NChunkClient::TChunkId chunkId,
    TAsyncErrorPromise finishResult,
    NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!batchRsp->IsOK()) {
        finishResult.Set(batchRsp->GetError());
        return;
    }

    LOG_DEBUG("Chunk registered successfully (ChunkId: %s)",
        ~chunkId.ToString());

    for (int i = 0; i < batchRsp->GetSize(); ++i) {
        auto rsp = batchRsp->GetResponse(i);
        if (!rsp->IsOK()) {
            finishResult.Set(rsp->GetError());
            return;
        }
    }

    finishResult.Set(TError());
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::OnChunkFinished(
    NChunkClient::TChunkId chunkId,
    TError error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!error.IsOK()) {
        State.Fail(error);
        return;
    }

    LOG_DEBUG("Chunk successfully closed and registered (ChunkId: %s)",
        ~chunkId.ToString());
}

template <class TChunkWriter>
TAsyncError TChunkSequenceWriterBase<TChunkWriter>::AsyncClose()
{
    YASSERT(!State.HasRunningOperation());

    State.StartOperation();
    FinishCurrentSession();

    CloseChunksAwaiter->Complete(BIND(
        &TChunkSequenceWriterBase::OnClose,
        MakeWeak(this)));

    return State.GetOperationError();
}

template <class TChunkWriter>
void TChunkSequenceWriterBase<TChunkWriter>::OnClose()
{
    if (State.IsActive()) {
        State.Close();
    }

    LOG_DEBUG("Sequence writer closed (TransactionId: %s)",
        ~TransactionId.ToString());

    State.FinishOperation();
}

template <class TChunkWriter> 
const std::vector<NProto::TInputChunk>& TChunkSequenceWriterBase<TChunkWriter>::GetWrittenChunks() const
{
    return WrittenChunks;
}

template <class TChunkWriter>
i64 TChunkSequenceWriterBase<TChunkWriter>::GetRowCount() const
{
    YASSERT(!State.HasRunningOperation());
    return RowCount;
}

template <class TChunkWriter> 
const TNullable<TKeyColumns>& TChunkSequenceWriterBase<TChunkWriter>::GetKeyColumns() const
{
    return KeyColumns;
}

template <class TChunkWriter> 
TAsyncError TChunkSequenceWriterBase<TChunkWriter>::GetReadyEvent()
{
    if (State.HasRunningOperation()) {
        return State.GetOperationError();
    }

    YCHECK(CurrentSession.ChunkWriter);
    return CurrentSession.ChunkWriter->GetReadyEvent();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT 
} // namespace NTableClient
