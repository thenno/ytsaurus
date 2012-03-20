#include "stdafx.h"
#include "job.h"
#include "environment_manager.h"
#include "slot.h"
#include "environment.h"
#include "private.h"

#include <ytlib/actions/action_util.h>
#include <ytlib/transaction_client/transaction.h>
#include <ytlib/file_server/file_ypath_proxy.h>
#include <ytlib/chunk_holder/chunk_cache.h>

namespace NYT {
namespace NExecAgent {

using namespace NScheduler::NProto;
using namespace NChunkHolder;
using namespace NRpc;
using namespace NScheduler::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

//// ToDo: make error.proto in misc.
//NScheduler::NProto::TJobResult MakeJobResult(const TError& error)
//{
//    NScheduler::NProto::TJobResult result;
//    result.set_is_ok(error.IsOK());
//    result.set_error_message(error.GetMessage());
//
//    return result;
//}

////////////////////////////////////////////////////////////////////////////////

TJob::TJob(
    const TJobId& jobId,
    const TJobSpec& jobSpec,
    TChunkCachePtr chunkCache,
    IChannel::TPtr masterChannel,
    TSlotPtr slot,
    IProxyController* proxyController)
    : JobId(jobId)
    , JobSpec(jobSpec)
    , ChunkCache(chunkCache)
    , MasterChannel(masterChannel)
    , Slot(slot)
    , ProxyController(proxyController)
    , JobResult(New< TFuture<NScheduler::NProto::TJobResult> >())
    , Started(New< TFuture<TVoid> >())
    , Finished(New< TFuture<NScheduler::NProto::TJobResult> >())
{
    //Slot->GetInvoker()->Invoke(FromMethod(
    //    &TJob::PrepareFiles,
    //    MakeStrong(this)));
}

const TJobId& TJob::GetId() const
{
    return JobId;
}

// TODO(babenko): get rid of this when new futures are ready
void TJob::SubscribeStarted(const TClosure& callback)
{
    Started->Subscribe(FromFunctor([=] (TVoid) { callback.Run(); }));
}

void TJob::UnsubscribeStarted(const TClosure& callback)
{
    YUNREACHABLE();
}

void TJob::SubscribeFinished(const TCallback<void(TJobResult)>& callback)
{
    Finished->Subscribe(FromFunctor([=] (TJobResult result) { callback.Run(result); }));
}

void TJob::UnsubscribeFinished(const TCallback<void(TJobResult)>& callback)
{
    YUNREACHABLE();
}

TJobResult TJob::GetResult()
{
    TJobResult result;
    YVERIFY(JobResult->TryGet(&result));
    return result;
}

//
//void TJob::SubscribeOnStarted(IAction::TPtr callback)
//{
//    OnStarted->Subscribe(callback->ToParamAction<TVoid>());
//}
//
//void TJob::SubscribeOnFinished(
//    IParamAction<NScheduler::NProto::TJobResult>::TPtr callback)
//{
//    OnFinished->Subscribe(callback);
//}
//
//const NScheduler::NProto::TJobSpec& TJob::GetSpec()
//{
//    // ToDo(psushin): make special supervisor call "Ready to start"
//    // to identify moment of start completion more precisely.
//    Slot->GetInvoker()->Invoke(FromMethod(
//        &TJob::StartComplete,
//        MakeStrong(this));
//    return JobSpec;
//}
//
//void TJob::SetResult(const NScheduler::NProto::TJobResult& jobResult)
//{
//    JobResult->Set(jobResult);
//}
//
//void TJob::Cancel(const TError& error)
//{
//    Slot->GetInvoker()->Invoke(FromMethod(
//        &TJob::DoCancel,
//        MakeStrong(this),
//        error));
//}
//
//void TJob::DoCancel(const TError& error)
//{
//    VERIFY_THREAD_AFFINITY(JobThread);
//
//    YASSERT(Error.IsOK());
//    YASSERT(!OnFinished->IsSet());
//
//    Error = error;
//
//    try {
//        LOG_TRACE("Trying to kill job (JobId: %s, error: %s)", 
//            ~JobId.ToString(),
//            ~error.GetMessage());
//
//        ProxyController->Kill(error);
//    } catch (yexception& e) {
//        Error = TError(Sprintf(
//            "Failed to kill job (JobId: %s, error: %s)", 
//            ~JobId.ToString(),
//            e.what()));
//
//        LOG_DEBUG("%s", ~Error.GetMessage());
//    }
//
//    if (!OnFinished->IsSet()) {
//        OnFinished->Set(MakeJobResult(Error));
//    }
//
//    Slot->Clean();
//}
//
//void TJob::StartComplete()
//{
//    VERIFY_THREAD_AFFINITY(JobThread);
//
//    if (!Error.IsOK())
//        return;
//
//    OnStarted->Set(TVoid());
//}
//
//void TJob::PrepareFiles()
//{
//    VERIFY_THREAD_AFFINITY(JobThread);
//
//    if (!Error.IsOK())
//        return;
//
//    auto transactionId = TTransactionId::FromProto(JobSpec.transaction_id());
//    auto batchReq = CypressProxy.ExecuteBatch();
//
//    for (int i = 0; i < JobSpec.operation_spec().files_size(); ++i) {
//        auto fetchReq = TFileYPathProxy::Fetch(WithTransaction(
//            JobSpec.operation_spec().files(i),
//            transactionId));
//        batchReq->AddRequest(~fetchReq);
//    }
//
//    batchReq->Invoke()->Subscribe(FromMethod(
//        &TJob::OnFilesFetched,
//        TPtr(this))->Via(Slot->GetInvoker()));
//}
//
//void TJob::OnFilesFetched(
//    TCypressServiceProxy::TRspExecuteBatch::TPtr batchRsp)
//{
//    VERIFY_THREAD_AFFINITY(JobThread);
//
//    if (!Error.IsOK())
//        return;
//
//    if (!batchRsp->IsOK()) {
//        DoCancel(batchRsp->GetError());
//        return;
//    }
//
//    auto awaiter = New<TParallelAwaiter>(~Slot->GetInvoker());
//
//    for (int fileIndex = 0; fileIndex < batchRsp->GetSize(); ++fileIndex) {
//        auto fetchRsp = batchRsp->GetResponse<TFileYPathProxy::TRspFetch>(fileIndex);
//        if (!fetchRsp->IsOK()) {
//            DoCancel(fetchRsp->GetError());
//            return;
//        }
//
//        auto chunkId = TChunkId::FromProto(fetchRsp->chunk_id());
//        auto fileName = fetchRsp->file_name();
//        auto executable = fetchRsp->executable();
//
//        awaiter->Await(ChunkCache->DownloadChunk(chunkId), FromMethod(
//            &TJob::OnChunkDownloaded,
//            TPtr(this),
//            fileIndex,
//            fileName,
//            executable));
//    }
//
//    awaiter->Complete(FromMethod(
//        &TJob::RunJobProxy,
//        TPtr(this)));
//}
//
//void TJob::OnChunkDownloaded(
//    NChunkHolder::TChunkCache::TDownloadResult result,
//    int fileIndex,
//    const Stroka& fileName,
//    bool executable)
//{
//    VERIFY_THREAD_AFFINITY(JobThread);
//
//    if (!Error.IsOK())
//        return;
//
//    if (!result.IsOK()) {
//        LOG_INFO("Failed to download file (JobId: %s, file name: %s, error: %s)", 
//            ~JobId.ToString(),
//            ~fileName,
//            ~result.GetMessage());
//        DoCancel(result);
//        return;
//    }
//
//    CachedChunks.push_back(result.Value());
//
//    try {
//        Slot->MakeLink(
//            fileName, 
//            CachedChunks.back()->GetFileName(), 
//            executable);
//    } catch (yexception& ex) {
//        LOG_INFO("Failed to make symlink (JobId: %s, file name: %s, error: %s)", 
//            ~JobId.ToString(),
//            ~fileName,
//            ex.what());
//        DoCancel(TError(ex.what()));
//        return;
//    }
//
//    LOG_INFO("Successfully downloaded file (JobId: %s, file name: %s)", 
//        ~JobId.ToString(),
//        ~fileName);
//}
//
//void TJob::RunJobProxy()
//{
//    VERIFY_THREAD_AFFINITY(JobThread);
//
//    if (!Error.IsOK())
//        return;
//
//    try {
//        ProxyController->Run();
//        ProxyController->SubscribeOnExit(~FromMethod(
//            &TJob::OnJobExit,
//            TPtr(this))->Via(Slot->GetInvoker()));
//
//    } catch (yexception& ex) {
//        DoCancel(TError(ex.what()));
//    }
//}
//
//void TJob::OnJobExit(TError error)
//{
//    VERIFY_THREAD_AFFINITY(JobThread);
//
//    if (!Error.IsOK())
//        return;
//
//    if (!error.IsOK()) {
//        DoCancel(error);
//        return;
//    }
//
//    if (!JobResult->IsSet()) {
//        DoCancel(TError(
//            "Job proxy successfully exited but job result has not been set."));
//    } else {
//        OnFinished->Set(JobResult->Get());
//    }
//}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT

