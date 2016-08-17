#include "operation_controller_detail.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "helpers.h"
#include "master_connector.h"

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/chunk_teleporter.h>
#include <yt/ytlib/chunk_client/data_statistics.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/input_slice.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/node_tracker_client/node_directory_builder.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/query_client/plan_fragment.h>
#include <yt/ytlib/query_client/query_preparer.h>
#include <yt/ytlib/query_client/functions_cache.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/table_consumer.h>
#include <yt/ytlib/table_client/helpers.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_ypath.pb.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/fs.h>

#include <yt/core/profiling/scoped_timer.h>

#include <functional>

namespace NYT {
namespace NScheduler {

using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NFormats;
using namespace NJobProxy;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;
using namespace NApi;
using namespace NRpc;
using namespace NTableClient;
using namespace NQueryClient;

using NNodeTrackerClient::TNodeId;
using NTableClient::NProto::TBoundaryKeysExt;
using NTableClient::NProto::TOldBoundaryKeysExt;
using NTableClient::TTableReaderOptions;

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TLivePreviewTableBase::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, LivePreviewTableId);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TInputTable::Persist(const TPersistenceContext& context)
{
    TUserObject::Persist(context);

    using NYT::Persist;
    Persist(context, ChunkCount);
    Persist(context, Chunks);
    Persist(context, Schema);
    Persist(context, SchemaMode);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TJobBoundaryKeys::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinKey);
    Persist(context, MaxKey);
    Persist(context, ChunkTreeId);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TOutputTable::Persist(const TPersistenceContext& context)
{
    TUserObject::Persist(context);
    TLivePreviewTableBase::Persist(context);

    using NYT::Persist;
    Persist(context, TableUploadOptions);
    Persist(context, Options);
    Persist(context, ChunkPropertiesUpdateNeeded);
    Persist(context, UploadTransactionId);
    Persist(context, OutputChunkListId);
    Persist(context, DataStatistics);
    // NB: Scheduler snapshots need not be stable.
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, OutputChunkTreeIds);
    Persist(context, BoundaryKeys);
    Persist(context, EffectiveAcl);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TIntermediateTable::Persist(const TPersistenceContext& context)
{
    TLivePreviewTableBase::Persist(context);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TUserFile::Persist(const TPersistenceContext& context)
{
    TUserObject::Persist(context);

    using NYT::Persist;
    Persist<TAttributeDictionaryRefSerializer>(context, Attributes);
    Persist(context, Stage);
    Persist(context, FileName);
    Persist(context, ChunkSpecs);
    Persist(context, Type);
    Persist(context, Executable);
    Persist(context, Format);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TCompletedJob::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Lost);
    Persist(context, JobId);
    Persist(context, SourceTask);
    Persist(context, OutputCookie);
    Persist(context, DataSize);
    Persist(context, DestinationPool);
    Persist(context, InputCookie);
    Persist(context, NodeDescriptor);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TJoblet::Persist(const TPersistenceContext& context)
{
    // NB: Every joblet is aborted after snapshot is loaded.
    // Here we only serialize a subset of members required for ReinstallJob to work
    // properly.
    using NYT::Persist;
    Persist(context, Task);
    Persist(context, NodeDescriptor);
    Persist(context, InputStripeList);
    Persist(context, OutputCookie);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TTaskGroup::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinNeededResources);
    // NB: Scheduler snapshots need not be stable.
    Persist<
        TSetSerializer<
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, NonLocalTasks);
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, CandidateTasks);
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, DelayedTasks);
    Persist<
        TMapSerializer<
            TDefaultSerializer,
            TSetSerializer<
                TDefaultSerializer,
                TUnsortedTag
            >,
            TUnsortedTag
        >
    >(context, NodeIdToTasks);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TStripeDescriptor::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Stripe);
    Persist(context, Cookie);
    Persist(context, Task);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TInputChunkDescriptor::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, InputStripes);
    Persist(context, InputChunks);
    Persist(context, State);
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TTask::TTask()
    : CachedPendingJobCount(-1)
    , CachedTotalJobCount(-1)
    , LastDemandSanityCheckTime(TInstant::Zero())
    , CompletedFired(false)
    , Logger(OperationLogger)
{ }

TOperationControllerBase::TTask::TTask(TOperationControllerBase* controller)
    : Controller(controller)
    , CachedPendingJobCount(0)
    , CachedTotalJobCount(0)
    , LastDemandSanityCheckTime(TInstant::Zero())
    , CompletedFired(false)
    , Logger(OperationLogger)
{ }

void TOperationControllerBase::TTask::Initialize()
{
    Logger = Controller->Logger;
    Logger.AddTag("Task: %v", GetId());
}

int TOperationControllerBase::TTask::GetPendingJobCount() const
{
    return GetChunkPoolOutput()->GetPendingJobCount();
}

int TOperationControllerBase::TTask::GetPendingJobCountDelta()
{
    int oldValue = CachedPendingJobCount;
    int newValue = GetPendingJobCount();
    CachedPendingJobCount = newValue;
    return newValue - oldValue;
}

int TOperationControllerBase::TTask::GetTotalJobCount() const
{
    return GetChunkPoolOutput()->GetTotalJobCount();
}

int TOperationControllerBase::TTask::GetTotalJobCountDelta()
{
    int oldValue = CachedTotalJobCount;
    int newValue = GetTotalJobCount();
    CachedTotalJobCount = newValue;
    return newValue - oldValue;
}

const TProgressCounter& TOperationControllerBase::TTask::GetJobCounter() const
{
    return GetChunkPoolOutput()->GetJobCounter();
}

TJobResources TOperationControllerBase::TTask::GetTotalNeededResourcesDelta()
{
    auto oldValue = CachedTotalNeededResources;
    auto newValue = GetTotalNeededResources();
    CachedTotalNeededResources = newValue;
    newValue -= oldValue;
    return newValue;
}

TJobResources TOperationControllerBase::TTask::GetTotalNeededResources() const
{
    i64 count = GetPendingJobCount();
    // NB: Don't call GetMinNeededResources if there are no pending jobs.
    return count == 0 ? ZeroJobResources() : GetMinNeededResources() * count;
}

bool TOperationControllerBase::TTask::IsIntermediateOutput() const
{
    return false;
}

i64 TOperationControllerBase::TTask::GetLocality(TNodeId nodeId) const
{
    return HasInputLocality()
        ? GetChunkPoolOutput()->GetLocality(nodeId)
        : 0;
}

bool TOperationControllerBase::TTask::HasInputLocality() const
{
    return true;
}

void TOperationControllerBase::TTask::AddInput(TChunkStripePtr stripe)
{
    Controller->RegisterInputStripe(stripe, this);
    if (HasInputLocality()) {
        Controller->AddTaskLocalityHint(this, stripe);
    }
    AddPendingHint();
}

void TOperationControllerBase::TTask::AddInput(const std::vector<TChunkStripePtr>& stripes)
{
    for (auto stripe : stripes) {
        if (stripe) {
            AddInput(stripe);
        }
    }
}

void TOperationControllerBase::TTask::FinishInput()
{
    LOG_DEBUG("Task input finished");

    GetChunkPoolInput()->Finish();
    AddPendingHint();
    CheckCompleted();
}

void TOperationControllerBase::TTask::CheckCompleted()
{
    if (!CompletedFired && IsCompleted()) {
        CompletedFired = true;
        OnTaskCompleted();
    }
}

TUserJobSpecPtr TOperationControllerBase::TTask::GetUserJobSpec() const
{
    return nullptr;
}

void TOperationControllerBase::TTask::ScheduleJob(
    ISchedulingContext* context,
    const TJobResources& jobLimits,
    TScheduleJobResult* scheduleJobResult)
{
    if (!CanScheduleJob(context, jobLimits)) {
        scheduleJobResult->RecordFail(EScheduleJobFailReason::TaskRefusal);
        return;
    }

    bool intermediateOutput = IsIntermediateOutput();
    int jobIndex = Controller->JobIndexGenerator.Next();
    auto joblet = New<TJoblet>(this, jobIndex);

    const auto& nodeResourceLimits = context->ResourceLimits();
    auto nodeId = context->GetNodeDescriptor().Id;
    const auto& address = context->GetNodeDescriptor().Address;

    auto* chunkPoolOutput = GetChunkPoolOutput();
    auto localityNodeId = HasInputLocality() ? nodeId : InvalidNodeId;
    joblet->OutputCookie = chunkPoolOutput->Extract(localityNodeId);
    if (joblet->OutputCookie == IChunkPoolOutput::NullCookie) {
        LOG_DEBUG("Job input is empty");
        scheduleJobResult->RecordFail(EScheduleJobFailReason::EmptyInput);
        return;
    }

    joblet->InputStripeList = chunkPoolOutput->GetStripeList(joblet->OutputCookie);

    auto estimatedResourceUsage = GetNeededResources(joblet);
    auto neededResources = ApplyMemoryReserve(estimatedResourceUsage);

    joblet->EstimatedResourceUsage = estimatedResourceUsage;
    joblet->ResourceLimits = neededResources;

    // Check the usage against the limits. This is the last chance to give up.
    if (!Dominates(jobLimits, neededResources)) {
        LOG_DEBUG("Job actual resource demand is not met (Limits: %v, Demand: %v)",
            FormatResources(jobLimits),
            FormatResources(neededResources));
        CheckResourceDemandSanity(nodeResourceLimits, neededResources);
        chunkPoolOutput->Aborted(joblet->OutputCookie);
        // Seems like cached min needed resources are too optimistic.
        ResetCachedMinNeededResources();
        scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
        return;
    }

    // Async part.
    auto controller = MakeStrong(Controller); // hold the controller
    auto jobSpecBuilder = BIND([=, this_ = MakeStrong(this)] (TJobSpec* jobSpec) {
        BuildJobSpec(joblet, jobSpec);
        controller->CustomizeJobSpec(joblet, jobSpec);

        auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        if (controller->Spec->JobProxyMemoryOvercommitLimit) {
            schedulerJobSpecExt->set_job_proxy_memory_overcommit_limit(*controller->Spec->JobProxyMemoryOvercommitLimit);
        }
        schedulerJobSpecExt->set_job_proxy_ref_counted_tracker_log_period(ToProto(controller->Spec->JobProxyRefCountedTrackerLogPeriod));

        schedulerJobSpecExt->set_enable_sort_verification(controller->Spec->EnableSortVerification);

        // Adjust sizes if approximation flag is set.
        if (joblet->InputStripeList->IsApproximate) {
            schedulerJobSpecExt->set_input_uncompressed_data_size(static_cast<i64>(
                schedulerJobSpecExt->input_uncompressed_data_size() *
                ApproximateSizesBoostFactor));
            schedulerJobSpecExt->set_input_row_count(static_cast<i64>(
                schedulerJobSpecExt->input_row_count() *
                ApproximateSizesBoostFactor));
        }

        if (schedulerJobSpecExt->input_uncompressed_data_size() > controller->Spec->MaxDataSizePerJob) {
            controller->GetCancelableInvoker()->Invoke(BIND(
                &TOperationControllerBase::OnOperationFailed,
                controller,
                TError(
                    "Maximum allowed data size per job violated: %v > %v",
                    schedulerJobSpecExt->input_uncompressed_data_size(),
                    controller->Spec->MaxDataSizePerJob)));
        }
    });

    auto jobType = GetJobType();
    joblet->JobId = context->GenerateJobId();
    auto restarted = LostJobCookieMap.find(joblet->OutputCookie) != LostJobCookieMap.end();
    scheduleJobResult->JobStartRequest.Emplace(
        joblet->JobId,
        jobType,
        neededResources,
        restarted,
        jobSpecBuilder,
        Controller->Spec->JobNodeAccount);

    joblet->JobType = jobType;
    joblet->NodeDescriptor = context->GetNodeDescriptor();
    joblet->JobProxyMemoryReserveFactor = Controller->GetJobProxyMemoryDigest(jobType)->GetQuantile(Controller->Config->JobProxyMemoryReserveQuantile);
    auto userJobSpec = GetUserJobSpec();
    if (userJobSpec) {
        joblet->UserJobMemoryReserveFactor = Controller->GetUserJobMemoryDigest(GetJobType())->GetQuantile(Controller->Config->UserJobMemoryReserveQuantile);
    }

    LOG_DEBUG(
        "Job scheduled (JobId: %v, OperationId: %v, JobType: %v, Address: %v, JobIndex: %v, ChunkCount: %v (%v local), "
        "Approximate: %v, DataSize: %v (%v local), RowCount: %v, Restarted: %v, EstimatedResourceUsage: %v, JobProxyMemoryReserveFactor: %v, "
        "UserJobMemoryReserveFactor: %v, ResourceLimits: %v)",
        joblet->JobId,
        Controller->OperationId,
        jobType,
        address,
        jobIndex,
        joblet->InputStripeList->TotalChunkCount,
        joblet->InputStripeList->LocalChunkCount,
        joblet->InputStripeList->IsApproximate,
        joblet->InputStripeList->TotalDataSize,
        joblet->InputStripeList->LocalDataSize,
        joblet->InputStripeList->TotalRowCount,
        restarted,
        FormatResources(estimatedResourceUsage),
        joblet->JobProxyMemoryReserveFactor,
        joblet->UserJobMemoryReserveFactor,
        FormatResources(neededResources));

    // Prepare chunk lists.
    if (intermediateOutput) {
        joblet->ChunkListIds.push_back(Controller->ExtractChunkList(Controller->IntermediateOutputCellTag));
    } else {
        for (const auto& table : Controller->OutputTables) {
            joblet->ChunkListIds.push_back(Controller->ExtractChunkList(table.CellTag));
        }
    }

    // Sync part.
    PrepareJoblet(joblet);
    Controller->CustomizeJoblet(joblet);

    Controller->RegisterJoblet(joblet);

    OnJobStarted(joblet);
}

bool TOperationControllerBase::TTask::IsPending() const
{
    return GetChunkPoolOutput()->GetPendingJobCount() > 0;
}

bool TOperationControllerBase::TTask::IsCompleted() const
{
    return IsActive() && GetChunkPoolOutput()->IsCompleted();
}

bool TOperationControllerBase::TTask::IsActive() const
{
    return true;
}

i64 TOperationControllerBase::TTask::GetTotalDataSize() const
{
    return GetChunkPoolOutput()->GetTotalDataSize();
}

i64 TOperationControllerBase::TTask::GetCompletedDataSize() const
{
    return GetChunkPoolOutput()->GetCompletedDataSize();
}

i64 TOperationControllerBase::TTask::GetPendingDataSize() const
{
    return GetChunkPoolOutput()->GetPendingDataSize();
}

void TOperationControllerBase::TTask::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, DelayedTime_);

    Persist(context, Controller);

    Persist(context, CachedPendingJobCount);
    Persist(context, CachedTotalJobCount);

    Persist(context, CachedTotalNeededResources);
    Persist(context, CachedMinNeededResources);

    Persist(context, LastDemandSanityCheckTime);

    Persist(context, CompletedFired);

    Persist(context, LostJobCookieMap);
}

void TOperationControllerBase::TTask::PrepareJoblet(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::TTask::OnJobStarted(TJobletPtr joblet)
{ }

void TOperationControllerBase::TTask::OnJobCompleted(TJobletPtr joblet, const TCompletedJobSummary& jobSummary)
{
    if (!jobSummary.Abandoned) {
        const auto& statistics = jobSummary.Statistics;
        auto outputStatisticsMap = GetOutputDataStatistics(statistics);
        for (int index = 0; index < static_cast<int>(joblet->ChunkListIds.size()); ++index) {
            YCHECK(outputStatisticsMap.find(index) != outputStatisticsMap.end());
            auto outputStatistics = outputStatisticsMap[index];
            if (outputStatistics.chunk_count() == 0) {
                Controller->ChunkListPool->Reinstall(joblet->ChunkListIds[index]);
                joblet->ChunkListIds[index] = NullChunkListId;
            }
        }

        auto inputStatistics = GetTotalInputDataStatistics(statistics);
        auto outputStatistics = GetTotalOutputDataStatistics(statistics);
        if (Controller->IsRowCountPreserved()) {
            if (inputStatistics.row_count() != outputStatistics.row_count()) {
                Controller->OnOperationFailed(TError(
                    "Input/output row count mismatch in completed job: %v != %v",
                    inputStatistics.row_count(),
                    outputStatistics.row_count())
                    << TErrorAttribute("task", GetId()));
            }
        }
    } else {
        auto& chunkListIds = joblet->ChunkListIds;
        Controller->ChunkListPool->Release(chunkListIds);
        std::fill(chunkListIds.begin(), chunkListIds.end(), NullChunkListId);
    }
    GetChunkPoolOutput()->Completed(joblet->OutputCookie);
}

void TOperationControllerBase::TTask::ReinstallJob(TJobletPtr joblet, EJobReinstallReason reason)
{
    Controller->ReleaseChunkLists(joblet->ChunkListIds);

    auto* chunkPoolOutput = GetChunkPoolOutput();

    auto list = HasInputLocality()
        ? chunkPoolOutput->GetStripeList(joblet->OutputCookie)
        : nullptr;

    switch (reason) {
        case EJobReinstallReason::Failed:
            chunkPoolOutput->Failed(joblet->OutputCookie);
            break;
        case EJobReinstallReason::Aborted:
            chunkPoolOutput->Aborted(joblet->OutputCookie);
            break;
        default:
            YUNREACHABLE();
    }

    if (HasInputLocality()) {
        for (const auto& stripe : list->Stripes) {
            Controller->AddTaskLocalityHint(this, stripe);
        }
    }

    AddPendingHint();
}

void TOperationControllerBase::TTask::OnJobFailed(TJobletPtr joblet, const TFailedJobSummary& /* jobSummary */)
{
    ReinstallJob(joblet, EJobReinstallReason::Failed);
}

void TOperationControllerBase::TTask::OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& /* jobSummary */)
{
    ReinstallJob(joblet, EJobReinstallReason::Aborted);
}

void TOperationControllerBase::TTask::OnJobLost(TCompletedJobPtr completedJob)
{
    YCHECK(LostJobCookieMap.insert(std::make_pair(
        completedJob->OutputCookie,
        completedJob->InputCookie)).second);
}

void TOperationControllerBase::TTask::OnTaskCompleted()
{
    LOG_DEBUG("Task completed");
}

bool TOperationControllerBase::TTask::CanScheduleJob(
    ISchedulingContext* /*context*/,
    const TJobResources& /*jobLimits*/)
{
    return true;
}

void TOperationControllerBase::TTask::DoCheckResourceDemandSanity(
    const TJobResources& neededResources)
{
    const auto& nodeDescriptors = Controller->GetExecNodeDescriptors();
    if (nodeDescriptors.size() < Controller->Config->SafeOnlineNodeCount) {
        return;
    }

    for (const auto& descriptor : nodeDescriptors) {
        if (Dominates(descriptor.ResourceLimits, neededResources)) {
            return;
        }
    }

    // It seems nobody can satisfy the demand.
    Controller->OnOperationFailed(
        TError("No online node can satisfy the resource demand")
            << TErrorAttribute("task", GetId())
            << TErrorAttribute("needed_resources", neededResources));
}

void TOperationControllerBase::TTask::CheckResourceDemandSanity(
    const TJobResources& neededResources)
{
    // Run sanity check to see if any node can provide enough resources.
    // Don't run these checks too often to avoid jeopardizing performance.
    auto now = TInstant::Now();
    if (now < LastDemandSanityCheckTime + Controller->Config->ResourceDemandSanityCheckPeriod)
        return;
    LastDemandSanityCheckTime = now;

    // Schedule check in controller thread.
    Controller->GetCancelableInvoker()->Invoke(BIND(
        &TTask::DoCheckResourceDemandSanity,
        MakeWeak(this),
        neededResources));
}

void TOperationControllerBase::TTask::CheckResourceDemandSanity(
    const TJobResources& nodeResourceLimits,
    const TJobResources& neededResources)
{
    // The task is requesting more than some node is willing to provide it.
    // Maybe it's OK and we should wait for some time.
    // Or maybe it's not and the task is requesting something no one is able to provide.

    // First check if this very node has enough resources (including those currently
    // allocated by other jobs).
    if (Dominates(nodeResourceLimits, neededResources))
        return;

    CheckResourceDemandSanity(neededResources);
}

void TOperationControllerBase::TTask::AddPendingHint()
{
    Controller->AddTaskPendingHint(this);
}

void TOperationControllerBase::TTask::AddLocalityHint(TNodeId nodeId)
{
    Controller->AddTaskLocalityHint(this, nodeId);
}

void TOperationControllerBase::TTask::AddSequentialInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(
        Controller->InputNodeDirectory,
        schedulerJobSpecExt->mutable_input_node_directory());
    auto* inputSpec = schedulerJobSpecExt->add_input_specs();
    inputSpec->set_table_reader_options(ConvertToYsonString(GetTableReaderOptions()).Data());
    const auto& list = joblet->InputStripeList;
    for (const auto& stripe : list->Stripes) {
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddParallelInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(
        Controller->InputNodeDirectory,
        schedulerJobSpecExt->mutable_input_node_directory());
    const auto& list = joblet->InputStripeList;
    for (const auto& stripe : list->Stripes) {
        auto* inputSpec = stripe->Foreign ? schedulerJobSpecExt->add_foreign_input_specs() :
            schedulerJobSpecExt->add_input_specs();
        inputSpec->set_table_reader_options(ConvertToYsonString(GetTableReaderOptions()).Data());
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddChunksToInputSpec(
    TNodeDirectoryBuilder* directoryBuilder,
    TTableInputSpec* inputSpec,
    TChunkStripePtr stripe)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        auto* chunkSpec = inputSpec->add_chunks();
        ToProto(chunkSpec, chunkSlice);
        auto replicas = chunkSlice->GetInputChunk()->GetReplicaList();
        directoryBuilder->Add(replicas);
    }
}

void TOperationControllerBase::TTask::UpdateInputSpecTotals(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    const auto& list = joblet->InputStripeList;
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    schedulerJobSpecExt->set_input_uncompressed_data_size(
        schedulerJobSpecExt->input_uncompressed_data_size() +
        list->TotalDataSize);
    schedulerJobSpecExt->set_input_row_count(
        schedulerJobSpecExt->input_row_count() +
        list->TotalRowCount);
}

void TOperationControllerBase::TTask::AddFinalOutputSpecs(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    YCHECK(joblet->ChunkListIds.size() == Controller->OutputTables.size());
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    for (int index = 0; index < Controller->OutputTables.size(); ++index) {
        const auto& table = Controller->OutputTables[index];
        auto* outputSpec = schedulerJobSpecExt->add_output_specs();
        outputSpec->set_table_writer_options(ConvertToYsonString(table.Options).Data());
        ToProto(outputSpec->mutable_table_schema(), table.TableUploadOptions.TableSchema);
        ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[index]);
    }
}

void TOperationControllerBase::TTask::AddIntermediateOutputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet,
    const TKeyColumns& keyColumns)
{
    YCHECK(joblet->ChunkListIds.size() == 1);
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    auto* outputSpec = schedulerJobSpecExt->add_output_specs();
    auto options = New<TTableWriterOptions>();
    options->Account = Controller->Spec->IntermediateDataAccount;
    options->ChunksVital = false;
    options->ChunksMovable = false;
    options->ReplicationFactor = 1;
    options->CompressionCodec = Controller->Spec->IntermediateCompressionCodec;
    outputSpec->set_table_writer_options(ConvertToYsonString(options).Data());

    ToProto(outputSpec->mutable_table_schema(), TTableSchema::FromKeyColumns(keyColumns));
    ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[0]);
}

void TOperationControllerBase::TTask::ResetCachedMinNeededResources()
{
    CachedMinNeededResources.Reset();
}

TJobResources TOperationControllerBase::TTask::ApplyMemoryReserve(const TExtendedJobResources& jobResources) const
{
    TJobResources result;
    result.SetCpu(jobResources.GetCpu());
    result.SetUserSlots(jobResources.GetUserSlots());
    i64 memory = jobResources.GetFootprintMemory();
    memory += jobResources.GetJobProxyMemory() * Controller->GetJobProxyMemoryDigest(GetJobType())->GetQuantile(Controller->Config->JobProxyMemoryReserveQuantile);
    if (GetUserJobSpec()) {
        memory += jobResources.GetUserJobMemory() * Controller->GetUserJobMemoryDigest(GetJobType())->GetQuantile(Controller->Config->UserJobMemoryReserveQuantile);
    } else {
        YCHECK(jobResources.GetUserJobMemory() == 0);
    }
    result.SetMemory(memory);
    result.SetNetwork(jobResources.GetNetwork());
    return result;
}

void TOperationControllerBase::TTask::AddFootprintAndUserJobResources(TExtendedJobResources& jobResources) const
{
    jobResources.SetFootprintMemory(GetFootprintMemorySize());
    auto userJobSpec = GetUserJobSpec();
    if (userJobSpec) {
        jobResources.SetUserJobMemory(userJobSpec->MemoryLimit);
    }
}

TJobResources TOperationControllerBase::TTask::GetMinNeededResources() const
{
    if (!CachedMinNeededResources) {
        YCHECK(GetPendingJobCount() > 0);
        CachedMinNeededResources = GetMinNeededResourcesHeavy();
    }
    auto result = ApplyMemoryReserve(*CachedMinNeededResources);
    if (result.GetUserSlots() > 0 && result.GetMemory() == 0) {
        LOG_WARNING("Found min needed resources of task with non-zero user slots and zero memory");
    }
    return result;
}

void TOperationControllerBase::TTask::RegisterIntermediate(
    TJobletPtr joblet,
    TChunkStripePtr stripe,
    TTaskPtr destinationTask,
    bool attachToLivePreview)
{
    RegisterIntermediate(
        joblet,
        stripe,
        destinationTask->GetChunkPoolInput(),
        attachToLivePreview);

    if (destinationTask->HasInputLocality()) {
        Controller->AddTaskLocalityHint(destinationTask, stripe);
    }
    destinationTask->AddPendingHint();
}

void TOperationControllerBase::TTask::RegisterIntermediate(
    TJobletPtr joblet,
    TChunkStripePtr stripe,
    IChunkPoolInput* destinationPool,
    bool attachToLivePreview)
{
    IChunkPoolInput::TCookie inputCookie;

    auto lostIt = LostJobCookieMap.find(joblet->OutputCookie);
    if (lostIt == LostJobCookieMap.end()) {
        inputCookie = destinationPool->Add(stripe);
    } else {
        inputCookie = lostIt->second;
        destinationPool->Resume(inputCookie, stripe);
        LostJobCookieMap.erase(lostIt);
    }

    // Store recovery info.
    auto completedJob = New<TCompletedJob>(
        joblet->JobId,
        this,
        joblet->OutputCookie,
        joblet->InputStripeList->TotalDataSize,
        destinationPool,
        inputCookie,
        joblet->NodeDescriptor);

    Controller->RegisterIntermediate(
        joblet,
        completedJob,
        stripe,
        attachToLivePreview);
}

TChunkStripePtr TOperationControllerBase::TTask::BuildIntermediateChunkStripe(
    google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs)
{
    auto stripe = New<TChunkStripe>();
    for (auto& chunkSpec : *chunkSpecs) {
        auto chunkSlice = CreateInputSlice(New<TInputChunk>(std::move(chunkSpec)));
        stripe->ChunkSlices.push_back(chunkSlice);
    }
    return stripe;
}

void TOperationControllerBase::TTask::RegisterOutput(
    TJobletPtr joblet,
    int key,
    const TCompletedJobSummary& jobSummary)
{
    Controller->RegisterOutput(joblet, key, jobSummary);
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TOperationControllerBase(
    TSchedulerConfigPtr config,
    TOperationSpecBasePtr spec,
    TOperationOptionsPtr options,
    IOperationHost* host,
    TOperation* operation)
    : Config(config)
    , Host(host)
    , Operation(operation)
    , OperationId(Operation->GetId())
    , StartTime(Operation->GetStartTime())
    , AuthenticatedUser(Operation->GetAuthenticatedUser())
    , AuthenticatedMasterClient(CreateClient())
    , AuthenticatedInputMasterClient(AuthenticatedMasterClient)
    , AuthenticatedOutputMasterClient(AuthenticatedMasterClient)
    , Logger(OperationLogger)
    , CancelableContext(New<TCancelableContext>())
    , CancelableControlInvoker(CancelableContext->CreateInvoker(Host->GetControlInvoker()))
    , Invoker(Host->CreateOperationControllerInvoker())
    , SuspendableInvoker(CreateSuspendableInvoker(Invoker))
    , CancelableInvoker(CancelableContext->CreateInvoker(SuspendableInvoker))
    , JobCounter(0)
    , UserTransactionId(Operation->GetUserTransaction() ? Operation->GetUserTransaction()->GetId() : NullTransactionId)
    , Spec(spec)
    , Options(options)
    , CachedNeededResources(ZeroJobResources())
    , CheckTimeLimitExecutor(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::CheckTimeLimit, MakeWeak(this)),
        Config->OperationTimeLimitCheckPeriod))
    , EventLogValueConsumer_(Host->CreateLogConsumer())
    , EventLogTableConsumer_(new TTableConsumer(EventLogValueConsumer_.get()))
    , CodicilData_(MakeOperationCodicilString(OperationId))
{
    Logger.AddTag("OperationId: %v", OperationId);
}

void TOperationControllerBase::InitializeConnections()
{ }

void TOperationControllerBase::InitializeReviving(TControllerTransactionsPtr controllerTransactions)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto codicilGuard = MakeCodicilGuard();

    LOG_INFO("Initializing operation for revive");

    InitializeConnections();

    std::atomic<bool> cleanStart = {false};


    // Check transactions.
    {
        auto checkTransaction = [&] (ITransactionPtr transaction) {
            if (cleanStart) {
                return;
            }
            if (!transaction) {
                cleanStart = true;
                LOG_INFO("Operation transaction is missing, will use clean start");
                return;
            }
        };

        // NB: Async transaction is not checked.
        checkTransaction(controllerTransactions->Sync);
        checkTransaction(controllerTransactions->Input);
        checkTransaction(controllerTransactions->Output);
    }

    // Downloading snapshot.
    if (!cleanStart)
    {
        auto snapshotOrError = WaitFor(Host->GetMasterConnector()->DownloadSnapshot(OperationId));
        if (!snapshotOrError.IsOK()) {
            LOG_INFO(snapshotOrError, "Failed to download snapshot, will use clean start");
            cleanStart = true;
        } else {
            LOG_INFO("Snapshot succesfully downloaded");
            Snapshot = snapshotOrError.Value();
        }
    }

    // Abort transactions if needed.
    {
        std::vector<TFuture<void>> asyncResults;

        auto scheduleAbort = [&] (ITransactionPtr transaction) {
            if (transaction) {
                asyncResults.push_back(transaction->Abort());
            }
        };

        // NB: Async transaction is always aborted.
        scheduleAbort(controllerTransactions->Async);

        if (cleanStart) {
            LOG_INFO("Aborting operation transactions");
            // NB: Don't touch user transaction.
            scheduleAbort(controllerTransactions->Sync);
            scheduleAbort(controllerTransactions->Input);
            scheduleAbort(controllerTransactions->Output);
        } else {
            LOG_INFO("Reusing operation transactions");
            SyncSchedulerTransaction = controllerTransactions->Sync;
            InputTransaction = controllerTransactions->Input;
            OutputTransaction = controllerTransactions->Output;

            StartAsyncSchedulerTransaction();

            AreTransactionsActive = true;
        }

        WaitFor(Combine(asyncResults))
            .ThrowOnError();
    }


    if (cleanStart) {
        LOG_INFO("Using clean start instead of revive");

        Snapshot = TSharedRef();
        auto error = WaitFor(Host->GetMasterConnector()->RemoveSnapshot(OperationId));
        if (!error.IsOK()) {
            LOG_WARNING(error, "Failed to remove snapshot");
        }

        InitializeTransactions();
        InitializeStructures();
    }

    LOG_INFO("Operation initialized");
}


void TOperationControllerBase::Initialize()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto codicilGuard = MakeCodicilGuard();

    LOG_INFO("Initializing operation (Title: %v)",
        Spec->Title);

    InitializeConnections();
    InitializeTransactions();
    InitializeStructures();

    LOG_INFO("Operation initialized");
}

void TOperationControllerBase::InitializeStructures()
{
    InputNodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
    AuxNodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();

    for (const auto& path : GetInputTablePaths()) {
        TInputTable table;
        table.Path = path;
        InputTables.push_back(table);
    }

    for (const auto& path : GetOutputTablePaths()) {
        TOutputTable table;
        table.Path = path;

        auto rowCountLimit = path.GetRowCountLimit();
        if (rowCountLimit) {
            if (RowCountLimitTableIndex) {
                THROW_ERROR_EXCEPTION("Only one output table with row_count_limit is supported");
            }
            RowCountLimitTableIndex = OutputTables.size();
            RowCountLimit = rowCountLimit.Get();
        }

        OutputTables.push_back(table);
    }

    for (const auto& pair : GetFilePaths()) {
        TUserFile file;
        file.Path = pair.first;
        file.Stage = pair.second;
        Files.push_back(file);
    }

    if (InputTables.size() > Config->MaxInputTableCount) {
        THROW_ERROR_EXCEPTION(
            "Too many input tables: maximum allowed %v, actual %v",
            Config->MaxInputTableCount,
            InputTables.size());
    }

    DoInitialize();
}

void TOperationControllerBase::DoInitialize()
{ }

void TOperationControllerBase::Prepare()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    GetUserObjectBasicAttributes<TInputTable>(
        AuthenticatedInputMasterClient,
        InputTables,
        InputTransaction->GetId(),
        Logger,
        EPermission::Read);

    for (const auto& table : InputTables) {
        if (table.Type != EObjectType::Table) {
            THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                table.Path.GetPath(),
                EObjectType::Table,
                table.Type);
        }
    }

    GetUserObjectBasicAttributes<TOutputTable>(
        AuthenticatedOutputMasterClient,
        OutputTables,
        OutputTransaction->GetId(),
        Logger,
        EPermission::Write);

    yhash_set<TObjectId> outputTableIds;
    for (const auto& table : OutputTables) {
        const auto& path = table.Path.GetPath();
        if (table.Type != EObjectType::Table) {
            THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                path,
                EObjectType::Table,
                table.Type);
        }
        if (outputTableIds.find(table.ObjectId) != outputTableIds.end()) {
            THROW_ERROR_EXCEPTION("Output table %v is specified multiple times",
                path);
        }
        outputTableIds.insert(table.ObjectId);
    }

    GetUserObjectBasicAttributes<TUserFile>(
        AuthenticatedMasterClient,
        Files,
        InputTransaction->GetId(),
        Logger,
        EPermission::Read);

    for (const auto& file : Files) {
        const auto& path = file.Path.GetPath();
        if (file.Type != EObjectType::Table && file.Type != EObjectType::File) {
            THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv or %Qlv, actual %Qlv",
                path,
                EObjectType::Table,
                EObjectType::File,
                file.Type);
        }
    }

    LockInputTables();
    LockUserFiles(&Files);

    GetOutputTablesSchema();
    PrepareOutputTables();

    BeginUploadOutputTables();
    GetOutputTablesUploadParams();
}

void TOperationControllerBase::Materialize()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    if (State == EControllerState::Running) {
        // Operation is successfully revived, skipping materialization.
        return;
    }

    try {
        FetchInputTables();
        FetchUserFiles(&Files);

        PickIntermediateDataCell();
        InitChunkListPool();

        CreateLivePreviewTables();

        LockLivePreviewTables();

        CollectTotals();

        CustomPrepare();

        if (InputChunkMap.empty()) {
            // Possible reasons:
            // - All input chunks are unavailable && Strategy == Skip
            // - Merge decided to passthrough all input chunks
            // - Anything else?
            LOG_INFO("No jobs needed");
            OnOperationCompleted(false /* interrupted */);
            return;
        }

        SuspendUnavailableInputStripes();

        AddAllTaskPendingHints();

        if (Config->EnableSnapshotCycleAfterMaterialization) {
            TStringStream stringStream;
            SaveSnapshot(&stringStream);
            auto sharedRef = TSharedRef::FromString(stringStream.Str());
            DoLoadSnapshot(sharedRef);
        }

        // Input chunk scraper initialization should be the last step to avoid races,
        // because input chunk scraper works in control thread.
        InitInputChunkScraper();

        CheckTimeLimitExecutor->Start();

        State = EControllerState::Running;
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Materialization failed");
        auto wrappedError = TError("Materialization failed") << ex;
        OnOperationFailed(wrappedError);
        return;
    }

    LOG_INFO("Materialization finished");
}

void TOperationControllerBase::SaveSnapshot(TOutputStream* output)
{
    auto codicilGuard = MakeCodicilGuard();

    DoSaveSnapshot(output);
}

void TOperationControllerBase::DoSaveSnapshot(TOutputStream* output)
{
    TSaveContext context;
    context.SetOutput(output);

    Save(context, this);
}

void TOperationControllerBase::Revive()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    if (!Snapshot) {
        Prepare();
        return;
    }

    InitChunkListPool();

    DoLoadSnapshot(Snapshot);

    LockLivePreviewTables();

    AbortAllJoblets();

    AddAllTaskPendingHints();

    // Input chunk scraper initialization should be the last step to avoid races.
    InitInputChunkScraper();

    ReinstallLivePreview();

    CheckTimeLimitExecutor->Start();

    State = EControllerState::Running;
}

void TOperationControllerBase::InitializeTransactions()
{
    StartAsyncSchedulerTransaction();
    StartSyncSchedulerTransaction();
    StartInputTransaction(SyncSchedulerTransaction->GetId());
    StartOutputTransaction(SyncSchedulerTransaction->GetId());
    AreTransactionsActive = true;
}

ITransactionPtr TOperationControllerBase::StartTransaction(
    const Stroka& transactionName,
    IClientPtr client,
    const TTransactionId& parentTransactionId = NullTransactionId)
{
    LOG_INFO("Starting %v transaction", transactionName);

    TTransactionStartOptions options;
    options.AutoAbort = false;
    options.PingAncestors = false;
    auto attributes = CreateEphemeralAttributes();
    attributes->Set(
        "title",
        Format("Scheduler %v for operation %v", transactionName, OperationId));
    attributes->Set("operation_id", OperationId);
    if (Spec->Title) {
        attributes->Set("operation_title", Spec->Title);
    }
    options.Attributes = std::move(attributes);
    options.ParentId = parentTransactionId;
    options.Timeout = Config->OperationTransactionTimeout;

    auto transactionOrError = WaitFor(
        client->StartTransaction(NTransactionClient::ETransactionType::Master, options));
    THROW_ERROR_EXCEPTION_IF_FAILED(
        transactionOrError,
        "Error starting %v transaction",
        transactionName);
    auto transaction = transactionOrError.Value();

    return transaction;
}

void TOperationControllerBase::StartSyncSchedulerTransaction()
{
    SyncSchedulerTransaction = StartTransaction("sync", AuthenticatedMasterClient, UserTransactionId);

    LOG_INFO("Scheduler sync transaction started (SyncTransactionId: %v)",
        SyncSchedulerTransaction->GetId());
}

void TOperationControllerBase::StartAsyncSchedulerTransaction()
{
    AsyncSchedulerTransaction = StartTransaction("async", AuthenticatedMasterClient);

    LOG_INFO("Scheduler async transaction started (AsyncTransactionId: %v)",
        AsyncSchedulerTransaction->GetId());
}

void TOperationControllerBase::StartInputTransaction(const TTransactionId& parentTransactionId)
{
    InputTransaction = StartTransaction(
        "input",
        AuthenticatedInputMasterClient,
        parentTransactionId);

    LOG_INFO("Input transaction started (InputTransactionId: %v)",
        InputTransaction->GetId());
}

void TOperationControllerBase::StartOutputTransaction(const TTransactionId& parentTransactionId)
{
    OutputTransaction = StartTransaction(
        "output",
        AuthenticatedOutputMasterClient,
        parentTransactionId);

    LOG_INFO("Output transaction started (OutputTransactionId: %v)",
        OutputTransaction->GetId());
}

void TOperationControllerBase::PickIntermediateDataCell()
{
    auto connection = AuthenticatedOutputMasterClient->GetConnection();
    const auto& secondaryCellTags = connection->GetSecondaryMasterCellTags();
    IntermediateOutputCellTag = secondaryCellTags.empty()
        ? connection->GetPrimaryMasterCellTag()
        : secondaryCellTags[rand() % secondaryCellTags.size()];
}

void TOperationControllerBase::InitChunkListPool()
{
    ChunkListPool = New<TChunkListPool>(
        Config,
        AuthenticatedOutputMasterClient,
        CancelableInvoker,
        OperationId,
        OutputTransaction->GetId());

    for (const auto& table : OutputTables) {
        ++CellTagToOutputTableCount[table.CellTag];
    }
}

void TOperationControllerBase::InitInputChunkScraper()
{
    yhash_set<TChunkId> chunkIds;
    for (const auto& pair : InputChunkMap) {
        chunkIds.insert(pair.first);
    }

    YCHECK(!InputChunkScraper);
    InputChunkScraper = New<TChunkScraper>(
        Config,
        CancelableInvoker,
        Host->GetChunkLocationThrottlerManager(),
        AuthenticatedInputMasterClient,
        InputNodeDirectory,
        std::move(chunkIds),
        BIND(&TThis::OnInputChunkLocated, MakeWeak(this))
            .Via(CancelableInvoker),
        Logger
    );

    if (UnavailableInputChunkCount > 0) {
        LOG_INFO("Waiting for %v unavailable input chunks", UnavailableInputChunkCount);
        InputChunkScraper->Start();
    }
}

void TOperationControllerBase::SuspendUnavailableInputStripes()
{
    YCHECK(UnavailableInputChunkCount == 0);

    for (const auto& pair : InputChunkMap) {
        const auto& chunkDescriptor = pair.second;
        if (chunkDescriptor.State == EInputChunkState::Waiting) {
            LOG_TRACE("Input chunk is unavailable (ChunkId: %v)", pair.first);
            for (const auto& inputStripe : chunkDescriptor.InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            ++UnavailableInputChunkCount;
        }
    }
}

void TOperationControllerBase::ReinstallLivePreview()
{
    auto masterConnector = Host->GetMasterConnector();

    if (IsOutputLivePreviewSupported()) {
        for (const auto& table : OutputTables) {
            std::vector<TChunkTreeId> childIds;
            childIds.reserve(table.OutputChunkTreeIds.size());
            for (const auto& pair : table.OutputChunkTreeIds) {
                childIds.push_back(pair.second);
            }
            masterConnector->AttachToLivePreview(
                OperationId,
                AsyncSchedulerTransaction->GetId(),
                table.LivePreviewTableId,
                childIds);
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        std::vector<TChunkTreeId> childIds;
        childIds.reserve(ChunkOriginMap.size());
        for (const auto& pair : ChunkOriginMap) {
            if (!pair.second->Lost) {
                childIds.push_back(pair.first);
            }
        }
        masterConnector->AttachToLivePreview(
            OperationId,
            AsyncSchedulerTransaction->GetId(),
            IntermediateTable.LivePreviewTableId,
            childIds);
    }
}

void TOperationControllerBase::AbortAllJoblets()
{
    for (const auto& pair : JobletMap) {
        auto joblet = pair.second;
        JobCounter.Aborted(1, EAbortReason::Scheduler);
        joblet->Task->OnJobAborted(joblet, TAbortedJobSummary(pair.first, EAbortReason::Scheduler));
    }
    JobletMap.clear();
}

void TOperationControllerBase::DoLoadSnapshot(TSharedRef snapshot)
{
    LOG_INFO("Started loading snapshot");

    TMemoryInput input(snapshot.Begin(), snapshot.Size());

    TLoadContext context;
    context.SetInput(&input);
    context.SetRowBuffer(RowBuffer);

    NPhoenix::TSerializer::InplaceLoad(context, this);

    LOG_INFO("Finished loading snapshot");
}

void TOperationControllerBase::Commit()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    // XXX(babenko): hotfix for YT-4636
    {
        auto client = Host->GetMasterClient();

        // NB: use root credentials.
        auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto path = GetOperationPath(OperationId) + "/@committing";

        {
            auto req = TYPathProxy::Exists(path);
            auto rsp = WaitFor(proxy.Execute(req))
                .ValueOrThrow();
            if (ConvertTo<bool>(rsp->value())) {
                THROW_ERROR_EXCEPTION("Operation is already committing");
            }
        }

        {
            auto req = TYPathProxy::Set(path);
            req->set_value(ConvertToYsonString(true).Data());
            WaitFor(proxy.Execute(req))
                .ThrowOnError();
        }
    }

    TeleportOutputChunks();
    AttachOutputChunks();
    EndUploadOutputTables();
    CustomCommit();

    CommitTransactions();

    LOG_INFO("Results committed");
}

void TOperationControllerBase::CommitTransactions()
{
    LOG_INFO("Committing scheduler transactions");

    auto commitTransaction = [&] (ITransactionPtr transaction) {
        if (!transaction) {
            return;
        }
        auto result = WaitFor(transaction->Commit());
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Operation has failed to commit");
    };

    AreTransactionsActive = false;

    commitTransaction(InputTransaction);
    commitTransaction(OutputTransaction);
    commitTransaction(SyncSchedulerTransaction);

    LOG_INFO("Scheduler transactions committed");

    // NB: Never commit async transaction since it's used for writing Live Preview tables.
    AsyncSchedulerTransaction->Abort();
}

void TOperationControllerBase::TeleportOutputChunks()
{
    auto teleporter = New<TChunkTeleporter>(
        Config,
        AuthenticatedOutputMasterClient,
        CancelableInvoker,
        OutputTransaction->GetId(),
        Logger);

    for (auto& table : OutputTables) {
        for (const auto& pair : table.OutputChunkTreeIds) {
            const auto& id = pair.second;
            if (TypeFromId(id) == EObjectType::ChunkList)
                continue;
            table.ChunkPropertiesUpdateNeeded = true;
            teleporter->RegisterChunk(id, table.CellTag);
        }
    }

    WaitFor(teleporter->Run())
        .ThrowOnError();
}

void TOperationControllerBase::AttachOutputChunks()
{
    for (auto& table : OutputTables) {
        auto objectIdPath = FromObjectId(table.ObjectId);
        const auto& path = table.Path.GetPath();

        LOG_INFO("Attaching output chunks (Path: %v)",
            path);

        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::Leader,
            table.CellTag);
        TChunkServiceProxy proxy(channel);

        // Split large outputs into separate requests.
        TChunkServiceProxy::TReqExecuteBatch::TAttachChunkTreesSubrequest* req = nullptr;
        TChunkServiceProxy::TReqExecuteBatchPtr batchReq;

        auto flushCurrentReq = [&] (bool requestStatistics) {
            if (req) {
                req->set_request_statistics(requestStatistics);

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error attaching chunks to output table %v",
                    path);

                const auto& batchRsp = batchRspOrError.Value();
                const auto& rsp = batchRsp->attach_chunk_trees_subresponses(0);
                if (requestStatistics) {
                    table.DataStatistics = rsp.statistics();
                }
            }

            req = nullptr;
            batchReq.Reset();
        };

        auto addChunkTree = [&] (const TChunkTreeId& chunkTreeId) {
            if (req && req->child_ids_size() >= Config->MaxChildrenPerAttachRequest) {
                // NB: No need for a statistics for an intermediate request.
                flushCurrentReq(false);
            }

            if (!req) {
                batchReq = proxy.ExecuteBatch();
                GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);
                req = batchReq->add_attach_chunk_trees_subrequests();
                ToProto(req->mutable_parent_id(), table.OutputChunkListId);
            }

            ToProto(req->add_child_ids(), chunkTreeId);
        };

        if (table.TableUploadOptions.TableSchema.IsSorted() && ShouldVerifySortedOutput()) {
            // Sorted output generated by user operation requires rearranging.
            LOG_DEBUG("Sorting %v boundary key pairs", table.BoundaryKeys.size());
            std::sort(
                table.BoundaryKeys.begin(),
                table.BoundaryKeys.end(),
                [&] (const TJobBoundaryKeys& lhs, const TJobBoundaryKeys& rhs) -> bool {
                    auto minKeyResult = CompareRows(lhs.MinKey, rhs.MinKey);
                    if (minKeyResult != 0) {
                        return minKeyResult < 0;
                    }
                    return lhs.MaxKey < rhs.MaxKey;
                });

            for (auto current = table.BoundaryKeys.begin(); current != table.BoundaryKeys.end(); ++current) {
                auto next = current + 1;
                if (next != table.BoundaryKeys.end()) {
                    int cmp = CompareRows(next->MinKey, current->MaxKey);

                    if (cmp < 0) {
                        THROW_ERROR_EXCEPTION("Output table %v is not sorted: job outputs have overlapping key ranges",
                            table.Path.GetPath())
                            << TErrorAttribute("current_range_max_key", current->MaxKey)
                            << TErrorAttribute("next_range_min_key", next->MinKey);
                    }

                    if (cmp == 0 && table.Options->ValidateUniqueKeys) {
                        THROW_ERROR_EXCEPTION("Output table %v contains duplicate keys: job outputs have overlapping key ranges",
                            table.Path.GetPath())
                            << TErrorAttribute("current_range_max_key", current->MaxKey)
                            << TErrorAttribute("next_range_min_key", next->MinKey);
                    }
                }

                if (current->ChunkTreeId) {
                    // Chunk tree may be absent if no data was written in the job.
                    addChunkTree(current->ChunkTreeId);
                }
            }
        } else {
            for (const auto& pair : table.OutputChunkTreeIds) {
                addChunkTree(pair.second);
            }
        }

        // NB: Don't forget to ask for the statistics in the last request.
        flushCurrentReq(true);

        LOG_INFO("Output chunks attached (Path: %v, Statistics: %v)",
            path,
            table.DataStatistics);
    }
}

void TOperationControllerBase::CustomCommit()
{ }

void TOperationControllerBase::EndUploadOutputTables()
{
    auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : OutputTables) {
        auto objectIdPath = FromObjectId(table.ObjectId);
        const auto& path = table.Path.GetPath();

        LOG_INFO("Finishing upload to output table (Path: %v, Schema: %v)",
            path,
            table.TableUploadOptions.TableSchema);

        {
            auto req = TTableYPathProxy::EndUpload(objectIdPath);
            *req->mutable_statistics() = table.DataStatistics;
            req->set_chunk_properties_update_needed(table.ChunkPropertiesUpdateNeeded);
            ToProto(req->mutable_table_schema(), table.TableUploadOptions.TableSchema);
            req->set_schema_mode(static_cast<int>(table.TableUploadOptions.SchemaMode));
            SetTransactionId(req, table.UploadTransactionId);
            GenerateMutationId(req);
            batchReq->AddRequest(req, "end_upload");
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error finishing upload to output tables");
}

void TOperationControllerBase::OnJobStarted(const TJobId& jobId, TInstant startTime)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    auto joblet = GetJoblet(jobId);
    joblet->StartTime = startTime;

    LogEventFluently(ELogEventType::JobStarted)
        .Item("job_id").Value(jobId)
        .Item("operation_id").Value(OperationId)
        .Item("resource_limits").Value(joblet->ResourceLimits)
        .Item("node_address").Value(joblet->NodeDescriptor.Address)
        .Item("job_type").Value(joblet->JobType);
}

void TOperationControllerBase::UpdateMemoryDigests(TJobletPtr joblet, TStatistics statistics)
{
    auto jobType = joblet->JobType;
    bool taskUpdateNeeded = false;

    auto userJobMaxMemoryUsage = FindNumericValue(statistics, "/user_job/max_memory");
    if (userJobMaxMemoryUsage) {
        auto* digest = GetUserJobMemoryDigest(jobType);
        double actualFactor = static_cast<double>(*userJobMaxMemoryUsage) / joblet->EstimatedResourceUsage.GetUserJobMemory();
        LOG_TRACE("Adding sample to the job proxy memory digest (JobType: %v, Sample: %v, JobId: %v)",
            jobType,
            actualFactor,
            joblet->JobId);
        digest->AddSample(actualFactor);
        taskUpdateNeeded = true;
    }

    auto jobProxyMaxMemoryUsage = FindNumericValue(statistics, "/job_proxy/max_memory");
    if (jobProxyMaxMemoryUsage) {
        auto* digest = GetJobProxyMemoryDigest(jobType);
        double actualFactor = static_cast<double>(*jobProxyMaxMemoryUsage) /
            (joblet->EstimatedResourceUsage.GetJobProxyMemory() + joblet->EstimatedResourceUsage.GetFootprintMemory());
        LOG_TRACE("Adding sample to the user job memory digest (JobType: %v, Sample: %v, JobId: %v)",
            jobType,
            actualFactor,
            joblet->JobId);
        digest->AddSample(actualFactor);
        taskUpdateNeeded = true;
    }

    if (taskUpdateNeeded) {
        UpdateAllTasksIfNeeded();
    }
}

void TOperationControllerBase::OnJobCompleted(std::unique_ptr<TCompletedJobSummary> jobSummary)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    jobSummary->ParseStatistics();

    const auto& jobId = jobSummary->Id;
    const auto& result = jobSummary->Result;

    JobCounter.Completed(1);

    auto joblet = GetJoblet(jobId);

    UpdateMemoryDigests(joblet, jobSummary->Statistics);

    FinalizeJoblet(joblet, jobSummary.get());
    LogFinishedJobFluently(ELogEventType::JobCompleted, joblet, *jobSummary);

    UpdateJobStatistics(*jobSummary);

    const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    // Populate node directory by adding additional nodes returned from the job.
    // NB: Job's output may become some other job's input.
    InputNodeDirectory->MergeFrom(schedulerResultExt.output_node_directory());

    joblet->Task->OnJobCompleted(joblet, *jobSummary);

    RemoveJoblet(jobId);

    UpdateTask(joblet->Task);

    if (IsCompleted()) {
        OnOperationCompleted(false /* interrupted */);
        return;
    }

    if (RowCountLimitTableIndex) {
        switch (joblet->JobType) {
            default:
                break;
            case EJobType::Map:
            case EJobType::OrderedMap:
            case EJobType::SortedReduce:
            case EJobType::PartitionReduce:
                auto path = Format("/data/output/%d/row_count%s", *RowCountLimitTableIndex, jobSummary->StatisticsSuffix);
                i64 count = GetNumericValue(JobStatistics, path);
                if (count >= RowCountLimit) {
                    OnOperationCompleted(true /* interrupted */);
                }
        }
    }

}

void TOperationControllerBase::OnJobFailed(std::unique_ptr<TFailedJobSummary> jobSummary)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    jobSummary->ParseStatistics();

    const auto& jobId = jobSummary->Id;
    const auto& result = jobSummary->Result;

    auto error = FromProto<TError>(result.error());

    JobCounter.Failed(1);

    auto joblet = GetJoblet(jobId);

    FinalizeJoblet(joblet, jobSummary.get());
    LogFinishedJobFluently(ELogEventType::JobFailed, joblet, *jobSummary)
        .Item("error").Value(error);

    UpdateJobStatistics(*jobSummary);

    joblet->Task->OnJobFailed(joblet, *jobSummary);

    RemoveJoblet(jobId);

    if (error.Attributes().Get<bool>("fatal", false)) {
        auto wrappedError = TError("Job failed with fatal error") << error;
        OnOperationFailed(wrappedError);
        return;
    }

    int failedJobCount = JobCounter.GetFailed();
    int maxFailedJobCount = Spec->MaxFailedJobCount;
    if (failedJobCount >= maxFailedJobCount) {
        OnOperationFailed(TError("Failed jobs limit exceeded")
            << TErrorAttribute("max_failed_job_count", maxFailedJobCount));
    }
}

void TOperationControllerBase::OnJobAborted(std::unique_ptr<TAbortedJobSummary> jobSummary)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    jobSummary->ParseStatistics();

    const auto& jobId = jobSummary->Id;
    auto abortReason = jobSummary->AbortReason;

    JobCounter.Aborted(1, abortReason);

    auto joblet = GetJoblet(jobId);
    if (abortReason == EAbortReason::ResourceOverdraft) {
        UpdateMemoryDigests(joblet, jobSummary->Statistics);
    }

    if (jobSummary->ShouldLog) {
        FinalizeJoblet(joblet, jobSummary.get());
        LogFinishedJobFluently(ELogEventType::JobAborted, joblet, *jobSummary)
            .Item("reason").Value(abortReason);

        UpdateJobStatistics(*jobSummary);
    }

    joblet->Task->OnJobAborted(joblet, *jobSummary);

    RemoveJoblet(jobId);

    if (abortReason == EAbortReason::FailedChunks) {
        const auto& result = jobSummary->Result;
        const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
        for (const auto& chunkId : schedulerResultExt.failed_chunk_ids()) {
            OnChunkFailed(FromProto<TChunkId>(chunkId));
        }
    }
}

void TOperationControllerBase::FinalizeJoblet(
    const TJobletPtr& joblet,
    TJobSummary* jobSummary)
{
    auto& statistics = jobSummary->Statistics;

    joblet->FinishTime = jobSummary->FinishTime;
    {
        auto duration = joblet->FinishTime - joblet->StartTime;
        statistics.AddSample("/time/total", duration.MilliSeconds());
    }

    if (jobSummary->PrepareDuration) {
        statistics.AddSample("/time/prepare", jobSummary->PrepareDuration->MilliSeconds());
    }
    if (jobSummary->ExecDuration) {
        statistics.AddSample("/time/exec", jobSummary->ExecDuration->MilliSeconds());
    }

    statistics.AddSample("/job_proxy/memory_reserve_factor_x10000", static_cast<int>(1e4 * joblet->JobProxyMemoryReserveFactor));
}

TFluentLogEvent TOperationControllerBase::LogFinishedJobFluently(
    ELogEventType eventType,
    const TJobletPtr& joblet,
    const TJobSummary& jobSummary)
{
    return LogEventFluently(eventType)
        .Item("job_id").Value(joblet->JobId)
        .Item("operation_id").Value(OperationId)
        .Item("start_time").Value(joblet->StartTime)
        .Item("finish_time").Value(joblet->FinishTime)
        .Item("resource_limits").Value(joblet->ResourceLimits)
        .Item("statistics").Value(jobSummary.Statistics)
        .Item("node_address").Value(joblet->NodeDescriptor.Address)
        .Item("job_type").Value(joblet->JobType);
}

IYsonConsumer* TOperationControllerBase::GetEventLogConsumer()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return EventLogTableConsumer_.get();
}


void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    auto it = InputChunkMap.find(chunkId);
    if (it == InputChunkMap.end()) {
        LOG_WARNING("Intermediate chunk %v has failed", chunkId);
        OnIntermediateChunkUnavailable(chunkId);
    } else {
        LOG_WARNING("Input chunk %v has failed", chunkId);
        OnInputChunkUnavailable(chunkId, it->second);
    }
}

void TOperationControllerBase::OnInputChunkLocated(const TChunkId& chunkId, const TChunkReplicaList& replicas)
{
    auto it = InputChunkMap.find(chunkId);
    YCHECK(it != InputChunkMap.end());

    auto& descriptor = it->second;
    YCHECK(!descriptor.InputChunks.empty());
    auto& chunkSpec = descriptor.InputChunks.front();
    auto codecId = NErasure::ECodec(chunkSpec->GetErasureCodec());

    if (IsUnavailable(replicas, codecId, IsParityReplicasFetchEnabled())) {
        OnInputChunkUnavailable(chunkId, descriptor);
    } else {
        OnInputChunkAvailable(chunkId, descriptor, replicas);
    }
}

void TOperationControllerBase::OnInputChunkAvailable(const TChunkId& chunkId, TInputChunkDescriptor& descriptor, const TChunkReplicaList& replicas)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (descriptor.State != EInputChunkState::Waiting)
        return;

    LOG_TRACE("Input chunk is available (ChunkId: %v)", chunkId);

    --UnavailableInputChunkCount;
    YCHECK(UnavailableInputChunkCount >= 0);

    if (UnavailableInputChunkCount == 0) {
        InputChunkScraper->Stop();
    }

    // Update replicas in place for all input chunks with current chunkId.
    for (auto& chunkSpec : descriptor.InputChunks) {
        chunkSpec->SetReplicaList(replicas);
    }

    descriptor.State = EInputChunkState::Active;

    for (const auto& inputStripe : descriptor.InputStripes) {
        --inputStripe.Stripe->WaitingChunkCount;
        if (inputStripe.Stripe->WaitingChunkCount > 0)
            continue;

        auto task = inputStripe.Task;
        task->GetChunkPoolInput()->Resume(inputStripe.Cookie, inputStripe.Stripe);
        if (task->HasInputLocality()) {
            AddTaskLocalityHint(task, inputStripe.Stripe);
        }
        AddTaskPendingHint(task);
    }
}

void TOperationControllerBase::OnInputChunkUnavailable(const TChunkId& chunkId, TInputChunkDescriptor& descriptor)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (descriptor.State != EInputChunkState::Active)
        return;

    ++ChunkLocatedCallCount;
    if (ChunkLocatedCallCount >= Config->MaxChunksPerScratch) {
        ChunkLocatedCallCount = 0;
        LOG_DEBUG("Located another batch of chunks (Count: %v, UnavailableInputChunkCount: %v)",
            Config->MaxChunksPerScratch,
            UnavailableInputChunkCount);
    }

    LOG_TRACE("Input chunk is unavailable (ChunkId: %v)", chunkId);

    ++UnavailableInputChunkCount;

    switch (Spec->UnavailableChunkTactics) {
        case EUnavailableChunkAction::Fail:
            OnOperationFailed(TError("Input chunk %v is unavailable",
                chunkId));
            break;

        case EUnavailableChunkAction::Skip: {
            descriptor.State = EInputChunkState::Skipped;
            for (const auto& inputStripe : descriptor.InputStripes) {
                inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);

                // Remove given chunk from the stripe list.
                SmallVector<TInputSlicePtr, 1> slices;
                std::swap(inputStripe.Stripe->ChunkSlices, slices);

                std::copy_if(
                    slices.begin(),
                    slices.end(),
                    inputStripe.Stripe->ChunkSlices.begin(),
                    [&] (TInputSlicePtr slice) {
                        return chunkId != slice->GetInputChunk()->ChunkId();
                    });

                // Reinstall patched stripe.
                inputStripe.Task->GetChunkPoolInput()->Resume(inputStripe.Cookie, inputStripe.Stripe);
                AddTaskPendingHint(inputStripe.Task);
            }
            InputChunkScraper->Start();
            break;
        }

        case EUnavailableChunkAction::Wait: {
            descriptor.State = EInputChunkState::Waiting;
            for (const auto& inputStripe : descriptor.InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            InputChunkScraper->Start();
            break;
        }

        default:
            YUNREACHABLE();
    }
}

void TOperationControllerBase::OnIntermediateChunkUnavailable(const TChunkId& chunkId)
{
    auto it = ChunkOriginMap.find(chunkId);
    YCHECK(it != ChunkOriginMap.end());
    auto completedJob = it->second;
    if (completedJob->Lost)
        return;

    LOG_DEBUG("Job is lost (Address: %v, JobId: %v, SourceTask: %v, OutputCookie: %v, InputCookie: %v)",
        completedJob->NodeDescriptor.Address,
        completedJob->JobId,
        completedJob->SourceTask->GetId(),
        completedJob->OutputCookie,
        completedJob->InputCookie);

    JobCounter.Lost(1);
    completedJob->Lost = true;
    completedJob->DestinationPool->Suspend(completedJob->InputCookie);
    completedJob->SourceTask->GetChunkPoolOutput()->Lost(completedJob->OutputCookie);
    completedJob->SourceTask->OnJobLost(completedJob);
    AddTaskPendingHint(completedJob->SourceTask);
}

bool TOperationControllerBase::IsOutputLivePreviewSupported() const
{
    return false;
}

bool TOperationControllerBase::IsIntermediateLivePreviewSupported() const
{
    return false;
}

std::vector<ITransactionPtr> TOperationControllerBase::GetTransactions()
{
    if (AreTransactionsActive) {
        return {AsyncSchedulerTransaction, SyncSchedulerTransaction, InputTransaction, OutputTransaction};
    } else {
        return {};
    }
}

void TOperationControllerBase::Abort()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto codicilGuard = MakeCodicilGuard();

    LOG_INFO("Aborting operation");

    auto abortTransaction = [&] (ITransactionPtr transaction) {
        if (transaction) {
            // Fire-and-forget.
            transaction->Abort();
        }
    };

    AreTransactionsActive = false;

    abortTransaction(InputTransaction);
    abortTransaction(OutputTransaction);
    abortTransaction(SyncSchedulerTransaction);
    abortTransaction(AsyncSchedulerTransaction);

    State = EControllerState::Finished;

    CancelableContext->Cancel();

    LOG_INFO("Operation aborted");
}

void TOperationControllerBase::Complete()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    BIND(&TOperationControllerBase::OnOperationCompleted, MakeStrong(this), true /* interrupted */)
        .Via(GetCancelableInvoker())
        .Run();
}

void TOperationControllerBase::CheckTimeLimit()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto timeLimit = Config->OperationTimeLimit;
    if (Spec->TimeLimit) {
        timeLimit = Spec->TimeLimit;
    }

    if (timeLimit) {
        if (TInstant::Now() - StartTime > *timeLimit) {
            OnOperationFailed(TError("Operation is running for too long, aborted")
                << TErrorAttribute("time_limit", *timeLimit));
        }
    }
}

TScheduleJobResultPtr TOperationControllerBase::ScheduleJob(
    ISchedulingContextPtr context,
    const TJobResources& jobLimits)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    // ScheduleJob must be a synchronous action, any context switches are prohibited.
    TContextSwitchedGuard contextSwitchGuard(BIND([] { YUNREACHABLE(); }));

    NProfiling::TScopedTimer timer;
    auto scheduleJobResult = New<TScheduleJobResult>();
    DoScheduleJob(context.Get(), jobLimits, scheduleJobResult.Get());
    if (scheduleJobResult->JobStartRequest) {
        JobCounter.Start(1);
    }
    scheduleJobResult->Duration = timer.GetElapsed();
    return scheduleJobResult;
}

void TOperationControllerBase::UpdateConfig(TSchedulerConfigPtr config)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    Config = config;
}

void TOperationControllerBase::CustomizeJoblet(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::CustomizeJobSpec(TJobletPtr /* joblet */, TJobSpec* /* jobSpec */)
{ }

void TOperationControllerBase::RegisterTask(TTaskPtr task)
{
    Tasks.push_back(std::move(task));
}

void TOperationControllerBase::RegisterTaskGroup(TTaskGroupPtr group)
{
    TaskGroups.push_back(std::move(group));
}

void TOperationControllerBase::UpdateTask(TTaskPtr task)
{
    int oldPendingJobCount = CachedPendingJobCount;
    int newPendingJobCount = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newPendingJobCount;

    int oldTotalJobCount = JobCounter.GetTotal();
    JobCounter.Increment(task->GetTotalJobCountDelta());
    int newTotalJobCount = JobCounter.GetTotal();

    IncreaseNeededResources(task->GetTotalNeededResourcesDelta());

    LOG_DEBUG_IF(
        newPendingJobCount != oldPendingJobCount || newTotalJobCount != oldTotalJobCount,
        "Task updated (Task: %v, PendingJobCount: %v -> %v, TotalJobCount: %v -> %v, NeededResources: %v)",
        task->GetId(),
        oldPendingJobCount,
        newPendingJobCount,
        oldTotalJobCount,
        newTotalJobCount,
        FormatResources(CachedNeededResources));

    i64 outputTablesTimesJobsCount = OutputTables.size() * newTotalJobCount;
    if (outputTablesTimesJobsCount > Config->MaxOutputTablesTimesJobsCount) {
        OnOperationFailed(TError(
                "Maximum allowed number of output tables times job count violated: %v > %v",
                outputTablesTimesJobsCount,
                Config->MaxOutputTablesTimesJobsCount)
            << TErrorAttribute("output_table_count", OutputTables.size())
            << TErrorAttribute("job_count", newTotalJobCount));
    }

    task->CheckCompleted();
}

void TOperationControllerBase::UpdateAllTasks()
{
    for (const auto& task: Tasks) {
        UpdateTask(task);
    }
}

void TOperationControllerBase::UpdateAllTasksIfNeeded()
{
    if (TInstant::Now() - LastTaskUpdateTime_ >= Config->TaskUpdatePeriod) {
        UpdateAllTasks();
        LastTaskUpdateTime_ = TInstant::Now();
    }
}

void TOperationControllerBase::MoveTaskToCandidates(
    TTaskPtr task,
    std::multimap<i64, TTaskPtr>& candidateTasks)
{
    const auto& neededResources = task->GetMinNeededResources();
    task->CheckResourceDemandSanity(neededResources);
    i64 minMemory = neededResources.GetMemory();
    candidateTasks.insert(std::make_pair(minMemory, task));
    LOG_DEBUG("Task moved to candidates (Task: %v, MinMemory: %v)",
        task->GetId(),
        minMemory / (1024 * 1024));

}

void TOperationControllerBase::AddTaskPendingHint(TTaskPtr task)
{
    if (task->GetPendingJobCount() > 0) {
        auto group = task->GetGroup();
        if (group->NonLocalTasks.insert(task).second) {
            LOG_DEBUG("Task pending hint added (Task: %v)", task->GetId());
            MoveTaskToCandidates(task, group->CandidateTasks);
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::AddAllTaskPendingHints()
{
    for (const auto& task : Tasks) {
        AddTaskPendingHint(task);
    }
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, TNodeId nodeId)
{
    auto group = task->GetGroup();
    if (group->NodeIdToTasks[nodeId].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %v, Address: %v)",
            task->GetId(),
            InputNodeDirectory->GetDescriptor(nodeId).GetDefaultAddress());
    }
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TNodeId nodeId)
{
    DoAddTaskLocalityHint(task, nodeId);
    UpdateTask(task);
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        for (auto replica : chunkSlice->GetInputChunk()->GetReplicaList()) {
            auto locality = chunkSlice->GetLocality(replica.GetIndex());
            if (locality > 0) {
                DoAddTaskLocalityHint(task, replica.GetNodeId());
            }
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::ResetTaskLocalityDelays()
{
    LOG_DEBUG("Task locality delays are reset");
    for (auto group : TaskGroups) {
        for (const auto& pair : group->DelayedTasks) {
            auto task = pair.second;
            if (task->GetPendingJobCount() > 0) {
                MoveTaskToCandidates(task, group->CandidateTasks);
            }
        }
        group->DelayedTasks.clear();
    }
}

bool TOperationControllerBase::CheckJobLimits(
    TTaskPtr task,
    const TJobResources& jobLimits,
    const TJobResources& nodeResourceLimits)
{
    auto neededResources = task->GetMinNeededResources();
    if (Dominates(jobLimits, neededResources)) {
        return true;
    }
    task->CheckResourceDemandSanity(nodeResourceLimits, neededResources);
    return false;
}

void TOperationControllerBase::DoScheduleJob(
    ISchedulingContext* context,
    const TJobResources& jobLimits,
    TScheduleJobResult* scheduleJobResult)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (Spec->TestingOperationOptions) {
        Sleep(Spec->TestingOperationOptions->SchedulingDelay);
    }

    if (!IsRunning()) {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        scheduleJobResult->RecordFail(EScheduleJobFailReason::OperationNotRunning);
    } else if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        scheduleJobResult->RecordFail(EScheduleJobFailReason::NoPendingJobs);
    } else {
        DoScheduleLocalJob(context, jobLimits, scheduleJobResult);
        if (!scheduleJobResult->JobStartRequest) {
            DoScheduleNonLocalJob(context, jobLimits, scheduleJobResult);
        }
    }
}

void TOperationControllerBase::DoScheduleLocalJob(
    ISchedulingContext* context,
    const TJobResources& jobLimits,
    TScheduleJobResult* scheduleJobResult)
{
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetNodeDescriptor().Address;
    auto nodeId = context->GetNodeDescriptor().Id;

    for (const auto& group : TaskGroups) {
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
            continue;
        }

        auto localTasksIt = group->NodeIdToTasks.find(nodeId);
        if (localTasksIt == group->NodeIdToTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask = nullptr;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task has positive locality.
            // Remove pending hint if not.
            auto locality = task->GetLocality(nodeId);
            if (locality <= 0) {
                localTasks.erase(jt);
                LOG_TRACE("Task locality hint removed (Task: %v, Address: %v)",
                    task->GetId(),
                    address);
                continue;
            }

            if (locality <= bestLocality) {
                continue;
            }

            if (task->GetPendingJobCount() == 0) {
                UpdateTask(task);
                continue;
            }

            if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (!IsRunning()) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::OperationNotRunning);
            return;
        }

        if (bestTask) {
            LOG_DEBUG(
                "Attempting to schedule a local job (Task: %v, Address: %v, Locality: %v, JobLimits: %v, "
                "PendingDataSize: %v, PendingJobCount: %v)",
                bestTask->GetId(),
                address,
                bestLocality,
                FormatResources(jobLimits),
                bestTask->GetPendingDataSize(),
                bestTask->GetPendingJobCount());

            if (!HasEnoughChunkLists(bestTask->IsIntermediateOutput())) {
                LOG_DEBUG("Job chunk list demand is not met");
                scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughChunkLists);
                return;
            }

            bestTask->ScheduleJob(context, jobLimits, scheduleJobResult);
            if (scheduleJobResult->JobStartRequest) {
                UpdateTask(bestTask);
                return;
            }
        } else {
            // NB: This is one of the possible reasons, hopefully the most probable.
            scheduleJobResult->RecordFail(EScheduleJobFailReason::NoLocalJobs);
        }
    }
}

void TOperationControllerBase::DoScheduleNonLocalJob(
    ISchedulingContext* context,
    const TJobResources& jobLimits,
    TScheduleJobResult* scheduleJobResult)
{
    auto now = context->GetNow();
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetNodeDescriptor().Address;

    for (const auto& group : TaskGroups) {
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
            continue;
        }

        auto& nonLocalTasks = group->NonLocalTasks;
        auto& candidateTasks = group->CandidateTasks;
        auto& delayedTasks = group->DelayedTasks;

        // Move tasks from delayed to candidates.
        while (!delayedTasks.empty()) {
            auto it = delayedTasks.begin();
            auto deadline = it->first;
            if (now < deadline) {
                break;
            }
            auto task = it->second;
            delayedTasks.erase(it);
            if (task->GetPendingJobCount() == 0) {
                LOG_DEBUG("Task pending hint removed (Task: %v)",
                    task->GetId());
                YCHECK(nonLocalTasks.erase(task) == 1);
                UpdateTask(task);
            } else {
                LOG_DEBUG("Task delay deadline reached (Task: %v)", task->GetId());
                MoveTaskToCandidates(task, candidateTasks);
            }
        }

        // Consider candidates in the order of increasing memory demand.
        {
            int processedTaskCount = 0;
            auto it = candidateTasks.begin();
            while (it != candidateTasks.end()) {
                ++processedTaskCount;
                auto task = it->second;

                // Make sure that the task is ready to launch jobs.
                // Remove pending hint if not.
                if (task->GetPendingJobCount() == 0) {
                    LOG_DEBUG("Task pending hint removed (Task: %v)", task->GetId());
                    candidateTasks.erase(it++);
                    YCHECK(nonLocalTasks.erase(task) == 1);
                    UpdateTask(task);
                    continue;
                }

                // Check min memory demand for early exit.
                if (task->GetMinNeededResources().GetMemory() > jobLimits.GetMemory()) {
                    break;
                }

                if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                    ++it;
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
                    continue;
                }

                if (!task->GetDelayedTime()) {
                    task->SetDelayedTime(now);
                }

                auto deadline = *task->GetDelayedTime() + task->GetLocalityTimeout();
                if (deadline > now) {
                    LOG_DEBUG("Task delayed (Task: %v, Deadline: %v)",
                        task->GetId(),
                        deadline);
                    delayedTasks.insert(std::make_pair(deadline, task));
                    candidateTasks.erase(it++);
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::TaskDelayed);
                    continue;
                }

                if (!IsRunning()) {
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::OperationNotRunning);
                    return;
                }

                LOG_DEBUG(
                    "Attempting to schedule a non-local job (Task: %v, Address: %v, JobLimits: %v, "
                    "PendingDataSize: %v, PendingJobCount: %v)",
                    task->GetId(),
                    address,
                    FormatResources(jobLimits),
                    task->GetPendingDataSize(),
                    task->GetPendingJobCount());

                if (!HasEnoughChunkLists(task->IsIntermediateOutput())) {
                    LOG_DEBUG("Job chunk list demand is not met");
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughChunkLists);
                    return;
                }

                task->ScheduleJob(context, jobLimits, scheduleJobResult);
                if (scheduleJobResult->JobStartRequest) {
                    UpdateTask(task);
                    LOG_DEBUG("Processed %v tasks", processedTaskCount);
                    return;
                }

                // If task failed to schedule job, its min resources might have been updated.
                auto minMemory = task->GetMinNeededResources().GetMemory();
                if (it->first == minMemory) {
                    ++it;
                } else {
                    it = candidateTasks.erase(it);
                    candidateTasks.insert(std::make_pair(minMemory, task));
                }
            }
            if (processedTaskCount == 0) {
                scheduleJobResult->RecordFail(EScheduleJobFailReason::NoCandidateTasks);
            }

            LOG_DEBUG("Processed %v tasks", processedTaskCount);
        }
    }
}

TCancelableContextPtr TOperationControllerBase::GetCancelableContext() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableContext;
}

IInvokerPtr TOperationControllerBase::GetCancelableControlInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableControlInvoker;
}

IInvokerPtr TOperationControllerBase::GetCancelableInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableInvoker;
}

IInvokerPtr TOperationControllerBase::GetInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return SuspendableInvoker;
}

TFuture<void> TOperationControllerBase::Suspend()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto codicilGuard = MakeCodicilGuard();

    return SuspendableInvoker->Suspend();
}

void TOperationControllerBase::Resume()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto codicilGuard = MakeCodicilGuard();

    SuspendableInvoker->Resume();
}

int TOperationControllerBase::GetPendingJobCount() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto codicilGuard = MakeCodicilGuard();

    // Avoid accessing the state while not prepared.
    if (!IsPrepared()) {
        return 0;
    }

    // NB: For suspended operations we still report proper pending job count
    // but zero demand.
    if (!IsRunning()) {
        return 0;
    }

    return CachedPendingJobCount;
}

int TOperationControllerBase::GetTotalJobCount() const
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto codicilGuard = MakeCodicilGuard();

    // Avoid accessing the state while not prepared.
    if (!IsPrepared()) {
        return 0;
    }

    return JobCounter.GetTotal();
}

void TOperationControllerBase::IncreaseNeededResources(const TJobResources& resourcesDelta)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(CachedNeededResourcesLock);
    CachedNeededResources += resourcesDelta;
}

TJobResources TOperationControllerBase::GetNeededResources() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(CachedNeededResourcesLock);
    return CachedNeededResources;
}

i64 TOperationControllerBase::ComputeUserJobMemoryReserve(EJobType jobType, TUserJobSpecPtr userJobSpec) const
{
    if (userJobSpec) {
        return userJobSpec->MemoryLimit * GetUserJobMemoryDigest(jobType)->GetQuantile(Config->UserJobMemoryReserveQuantile);
    } else {
        return 0;
    }
}

void TOperationControllerBase::OnOperationCompleted(bool interrupted)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);
    Y_UNUSED(interrupted);

    // This can happen if operation failed during completion in derived class (e.x. SortController).
    if (IsFinished()) {
        return;
    }

    LOG_INFO("Operation completed");

    State = EControllerState::Finished;

    Host->OnOperationCompleted(Operation);
}

void TOperationControllerBase::OnOperationFailed(const TError& error)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // During operation failing job aborting can lead to another operation fail, we don't want to invoke it twice.
    if (IsFinished()) {
        return;
    }

    State = EControllerState::Finished;

    Host->OnOperationFailed(Operation, error);
}

bool TOperationControllerBase::IsPrepared() const
{
    return State != EControllerState::Preparing;
}

bool TOperationControllerBase::IsRunning() const
{
    return State == EControllerState::Running;
}

bool TOperationControllerBase::IsFinished() const
{
    return State == EControllerState::Finished;
}

void TOperationControllerBase::CreateLivePreviewTables()
{
    auto client = Host->GetMasterClient();
    auto connection = client->GetConnection();

    // NB: use root credentials.
    auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    auto addRequest = [&] (
        const Stroka& path,
        TCellTag cellTag,
        int replicationFactor,
        const Stroka& key,
        const TYsonString& acl)
    {
        auto req = TCypressYPathProxy::Create(path);
        req->set_type(static_cast<int>(EObjectType::Table));
        req->set_ignore_existing(true);
        req->set_enable_accounting(false);

        auto attributes = CreateEphemeralAttributes();
        attributes->Set("replication_factor", replicationFactor);
        if (cellTag == connection->GetPrimaryMasterCellTag()) {
            attributes->Set("external", false);
        } else {
            attributes->Set("external_cell_tag", cellTag);
        }
        attributes->Set("acl", acl);
        attributes->Set("inherit_acl", false);
        ToProto(req->mutable_node_attributes(), *attributes);

        batchReq->AddRequest(req, key);
    };

    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Creating output tables for live preview");

        for (int index = 0; index < OutputTables.size(); ++index) {
            const auto& table = OutputTables[index];
            auto path = GetLivePreviewOutputPath(OperationId, index);
            addRequest(
                path,
                table.CellTag,
                table.Options->ReplicationFactor,
                "create_output",
                OutputTables[index].EffectiveAcl);
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Creating intermediate table for live preview");

        auto path = GetLivePreviewIntermediatePath(OperationId);
        addRequest(
            path,
            IntermediateOutputCellTag,
            1,
            "create_intermediate",
            ConvertToYsonString(Spec->IntermediateDataAcl));
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error creating live preview tables");
    const auto& batchRsp = batchRspOrError.Value();

    auto handleResponse = [&] (TLivePreviewTableBase& table, TCypressYPathProxy::TRspCreatePtr rsp) {
        table.LivePreviewTableId = FromProto<NCypressClient::TNodeId>(rsp->node_id());
    };

    if (IsOutputLivePreviewSupported()) {
        auto rspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_output");
        YCHECK(rspsOrError.size() == OutputTables.size());
        for (int index = 0; index < OutputTables.size(); ++index) {
            handleResponse(OutputTables[index], rspsOrError[index].Value());
        }

        LOG_INFO("Output live preview tables created");
    }

    if (IsIntermediateLivePreviewSupported()) {
        auto rspOrError = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>("create_intermediate");
        handleResponse(IntermediateTable, rspOrError.Value());

        LOG_INFO("Intermediate live preview table created");
    }
}

void TOperationControllerBase::LockLivePreviewTables()
{
    auto channel = Host->GetMasterClient()->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    auto addRequest = [&] (const TLivePreviewTableBase& table, const Stroka& key) {
        auto req = TCypressYPathProxy::Lock(FromObjectId(table.LivePreviewTableId));
        req->set_mode(static_cast<int>(ELockMode::Exclusive));
        SetTransactionId(req, AsyncSchedulerTransaction->GetId());
        batchReq->AddRequest(req, key);
    };

    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Locking live preview output tables");
        for (const auto& table : OutputTables) {
            addRequest(table, "lock_output");
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Locking live preview intermediate table");
        addRequest(IntermediateTable, "lock_intermediate");
    }

    if (batchReq->GetSize() == 0) {
        return;
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking live preview tables");

    LOG_INFO("Live preview tables locked");
}

void TOperationControllerBase::FetchInputTables()
{
    for (int tableIndex = 0; tableIndex < static_cast<int>(InputTables.size()); ++tableIndex) {
        auto& table = InputTables[tableIndex];
        auto objectIdPath = FromObjectId(table.ObjectId);
        const auto& path = table.Path.GetPath();
        const auto& ranges = table.Path.GetRanges();
        if (ranges.empty())
            continue;

        LOG_INFO("Fetching input table (Path: %v, RangeCount: %v)",
            path,
            ranges.size());

        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::Follower,
            table.CellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();
        std::vector<int> rangeIndices;

        for (int rangeIndex = 0; rangeIndex < static_cast<int>(ranges.size()); ++rangeIndex) {
            for (i64 index = 0; index * Config->MaxChunksPerFetch < table.ChunkCount; ++index) {
                auto adjustedRange = ranges[rangeIndex];
                auto chunkCountLowerLimit = index * Config->MaxChunksPerFetch;
                if (adjustedRange.LowerLimit().HasChunkIndex()) {
                    chunkCountLowerLimit = std::max(chunkCountLowerLimit, adjustedRange.LowerLimit().GetChunkIndex());
                }
                adjustedRange.LowerLimit().SetChunkIndex(chunkCountLowerLimit);

                auto chunkCountUpperLimit = (index + 1) * Config->MaxChunksPerFetch;
                if (adjustedRange.UpperLimit().HasChunkIndex()) {
                    chunkCountUpperLimit = std::min(chunkCountUpperLimit, adjustedRange.UpperLimit().GetChunkIndex());
                }
                adjustedRange.UpperLimit().SetChunkIndex(chunkCountUpperLimit);

                auto req = TTableYPathProxy::Fetch(FromObjectId(table.ObjectId));
                InitializeFetchRequest(req.Get(), table.Path);
                ToProto(req->mutable_ranges(), std::vector<TReadRange>({adjustedRange}));
                req->set_fetch_all_meta_extensions(false);
                req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                if (IsBoundaryKeysFetchEnabled()) {
                    req->add_extension_tags(TProtoExtensionTag<TBoundaryKeysExt>::Value);
                    req->add_extension_tags(TProtoExtensionTag<TOldBoundaryKeysExt>::Value);
                }
                req->set_fetch_parity_replicas(IsParityReplicasFetchEnabled());
                SetTransactionId(req, InputTransaction->GetId());
                batchReq->AddRequest(req, "fetch");
                rangeIndices.push_back(rangeIndex);
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error fetching input table %v",
            path);
        const auto& batchRsp = batchRspOrError.Value();

        auto rspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>("fetch");
        for (int resultIndex = 0; resultIndex < static_cast<int>(rspsOrError.size()); ++resultIndex) {
            const auto& rsp = rspsOrError[resultIndex].Value();
            std::vector<NChunkClient::NProto::TChunkSpec> chunkSpecs;
            ProcessFetchResponse(
                AuthenticatedInputMasterClient,
                rsp,
                table.CellTag,
                InputNodeDirectory,
                Config->MaxChunksPerLocateRequest,
                rangeIndices[resultIndex],
                Logger,
                &chunkSpecs);

            for (auto& chunk : chunkSpecs) {
                auto chunkSpec = New<TInputChunk>(std::move(chunk));
                chunkSpec->SetTableIndex(tableIndex);
                table.Chunks.push_back(chunkSpec);
            }
        }

        LOG_INFO("Input table fetched (Path: %v, ChunkCount: %v)",
            path,
            table.Chunks.size());
    }
}

void TOperationControllerBase::LockInputTables()
{
    LOG_INFO("Locking input tables");

    {
        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : InputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Lock(objectIdPath);
                req->set_mode(static_cast<int>(ELockMode::Snapshot));
                SetTransactionId(req, InputTransaction->GetId());
                GenerateMutationId(req);
                batchReq->AddRequest(req, "lock");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking input tables");
    }

    LOG_INFO("Getting input tables attributes");

    {
        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : InputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath + "/@");
                std::vector<Stroka> attributeKeys{
                    "dynamic",
                    "chunk_count",
                    "schema_mode",
                    "schema"
                };
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                SetTransactionId(req, InputTransaction->GetId());
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of input tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto lockInRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspLock>("lock");
        auto getInAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < InputTables.size(); ++index) {
            auto& table = InputTables[index];
            auto path = table.Path.GetPath();
            {
                const auto& rsp = getInAttributesRspsOrError[index].Value();
                auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

                if (attributes->Get<bool>("dynamic")) {
                    THROW_ERROR_EXCEPTION("Expected a static table, but got dynamic")
                        << TErrorAttribute("input_table", path);
                }

                table.Schema = attributes->Get<TTableSchema>("schema");
                table.SchemaMode = attributes->Get<ETableSchemaMode>("schema_mode");

                table.ChunkCount = attributes->Get<int>("chunk_count");
            }
            LOG_INFO("Input table locked (Path: %v, Schema: %v, ChunkCount: %v)",
                path,
                table.Schema,
                table.ChunkCount);
        }
    }
}

void TOperationControllerBase::GetOutputTablesSchema()
{
    LOG_INFO("Getting output tables schema");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : OutputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath + "/@");
                std::vector<Stroka> attributeKeys{
                    "schema_mode",
                    "schema"
                };
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                SetTransactionId(req, OutputTransaction->GetId());
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getOutAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < OutputTables.size(); ++index) {
            auto& table = OutputTables[index];
            const auto& path = table.Path;

            const auto& rsp = getOutAttributesRspsOrError[index].Value();
            auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

            table.TableUploadOptions = GetTableUploadOptions(
                path,
                attributes->Get<TTableSchema>("schema"),
                attributes->Get<ETableSchemaMode>("schema_mode"),
                0); // Here we assume zero row count, we will do additional check later.
        }
    }
}

void TOperationControllerBase::PrepareOutputTables()
{ }

void TOperationControllerBase::BeginUploadOutputTables()
{
    LOG_INFO("Locking output tables");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        {
            auto batchReq = proxy.ExecuteBatch();
            for (const auto& table : OutputTables) {
                auto objectIdPath = FromObjectId(table.ObjectId);
                auto req = TTableYPathProxy::Lock(objectIdPath);
                req->set_mode(static_cast<int>(table.TableUploadOptions.LockMode));
                GenerateMutationId(req);
                SetTransactionId(req, OutputTransaction->GetId());
                batchReq->AddRequest(req, "lock");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                "Error locking output tables");
        }

        {
            auto batchReq = proxy.ExecuteBatch();
            for (const auto& table : OutputTables) {
                auto objectIdPath = FromObjectId(table.ObjectId);
                auto req = TTableYPathProxy::BeginUpload(objectIdPath);
                SetTransactionId(req, OutputTransaction->GetId());
                GenerateMutationId(req);
                req->set_update_mode(static_cast<int>(table.TableUploadOptions.UpdateMode));
                req->set_lock_mode(static_cast<int>(table.TableUploadOptions.LockMode));
                batchReq->AddRequest(req, "begin_upload");
            }
            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                "Error starting upload transactions for output tables");
            const auto& batchRsp = batchRspOrError.Value();

            auto beginUploadRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspBeginUpload>("begin_upload");
            for (int index = 0; index < OutputTables.size(); ++index) {
                auto& table = OutputTables[index];
                {
                    const auto& rsp = beginUploadRspsOrError[index].Value();
                    table.UploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());
                }
            }
        }
    }

    LOG_INFO("Getting output tables attributes");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : OutputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath + "/@");

                std::vector<Stroka> attributeKeys{
                    "account",
                    "compression_codec",
                    "effective_acl",
                    "erasure_codec",
                    "replication_factor",
                    "row_count",
                    "vital",
                    "optimize_for"
                };
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                SetTransactionId(req, OutputTransaction->GetId());
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            GetCumulativeError(batchRspOrError),
            "Error getting attributes of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getOutAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < OutputTables.size(); ++index) {
            auto& table = OutputTables[index];
            const auto& path = table.Path.GetPath();
            {
                const auto& rsp = getOutAttributesRspsOrError[index].Value();
                auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

                if (attributes->Get<i64>("row_count") > 0 &&
                    table.TableUploadOptions.TableSchema.IsSorted() &&
                    table.TableUploadOptions.UpdateMode == EUpdateMode::Append)
                {
                    THROW_ERROR_EXCEPTION("Cannot append sorted data to non-empty output table %v",
                        path);
                }

                if (table.TableUploadOptions.TableSchema.IsSorted()) {
                    table.Options->ValidateSorted = true;
                    table.Options->ValidateUniqueKeys = table.TableUploadOptions.TableSchema.GetUniqueKeys();
                } else {
                    table.Options->ValidateSorted = false;
                }
                table.Options->CompressionCodec = attributes->Get<NCompression::ECodec>("compression_codec");
                table.Options->ErasureCodec = attributes->Get<NErasure::ECodec>("erasure_codec", NErasure::ECodec::None);
                table.Options->ReplicationFactor = attributes->Get<int>("replication_factor");
                table.Options->Account = attributes->Get<Stroka>("account");
                table.Options->ChunksVital = attributes->Get<bool>("vital");
                table.Options->OptimizeFor = attributes->Get<EOptimizeFor>("optimize_for", EOptimizeFor::Lookup);

                table.EffectiveAcl = attributes->GetYson("effective_acl");
            }
            LOG_INFO("Output table locked (Path: %v, Options: %v, UploadTransactionId: %v)",
                path,
                ConvertToYsonString(table.Options, EYsonFormat::Text).Data(),
                table.UploadTransactionId);
        }
    }
}

void TOperationControllerBase::GetOutputTablesUploadParams()
{
    yhash<TCellTag, std::vector<TOutputTable*>> cellTagToTables;
    for (auto& table : OutputTables) {
        cellTagToTables[table.CellTag].push_back(&table);
    }

    for (const auto& pair : cellTagToTables) {
        auto cellTag = pair.first;
        const auto& tables = pair.second;

        LOG_INFO("Getting output tables upload parameters (CellTag: %v)", cellTag);

        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::Follower,
            cellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();
        for (const auto& table : tables) {
            auto objectIdPath = FromObjectId(table->ObjectId);
            {
                auto req = TTableYPathProxy::GetUploadParams(objectIdPath);
                SetTransactionId(req, table->UploadTransactionId);
                batchReq->AddRequest(req, "get_upload_params");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting upload parameters of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getUploadParamsRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGetUploadParams>("get_upload_params");
        for (int index = 0; index < tables.size(); ++index) {
            auto* table = tables[index];
            const auto& path = table->Path.GetPath();
            {
                const auto& rspOrError = getUploadParamsRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting upload parameters of output table %v",
                    path);

                const auto& rsp = rspOrError.Value();
                table->OutputChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());

                LOG_INFO("Upload parameters of output table received (Path: %v, ChunkListId: %v)",
                    path,
                    table->OutputChunkListId);
            }
        }
    }
}

void TOperationControllerBase::FetchUserFiles(std::vector<TUserFile>* files)
{
    for (auto& file : *files) {
        auto objectIdPath = FromObjectId(file.ObjectId);
        const auto& path = file.Path.GetPath();

        LOG_INFO("Fetching user file (Path: %v)",
            path);

        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::Follower,
            file.CellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        {
            auto req = TChunkOwnerYPathProxy::Fetch(objectIdPath);
            ToProto(req->mutable_ranges(), std::vector<TReadRange>({TReadRange()}));
            switch (file.Type) {
                case EObjectType::Table:
                    req->set_fetch_all_meta_extensions(true);
                    InitializeFetchRequest(req.Get(), file.Path);
                    break;

                case EObjectType::File:
                    req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                    break;

                default:
                    YUNREACHABLE();
            }
            SetTransactionId(req, InputTransaction->GetId());
            batchReq->AddRequest(req, "fetch");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error fetching user file %v",
             path);
        const auto& batchRsp = batchRspOrError.Value();

        {
            auto rsp = batchRsp->GetResponse<TChunkOwnerYPathProxy::TRspFetch>("fetch").Value();
            ProcessFetchResponse(
                AuthenticatedInputMasterClient,
                rsp,
                file.CellTag,
                AuxNodeDirectory,
                Config->MaxChunksPerLocateRequest,
                Null,
                Logger,
                &file.ChunkSpecs);
        }

        LOG_INFO("User file fetched (Path: %v, FileName: %v)",
            path,
            file.FileName);
    }
}

void TOperationControllerBase::LockUserFiles(std::vector<TUserFile>* files)
{
    LOG_INFO("Locking user files");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& file : *files) {
            auto objectIdPath = FromObjectId(file.ObjectId);

            {
                auto req = TCypressYPathProxy::Lock(objectIdPath);
                req->set_mode(static_cast<int>(ELockMode::Snapshot));
                GenerateMutationId(req);
                SetTransactionId(req, InputTransaction->GetId());
                batchReq->AddRequest(req, "lock");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking user files");
    }

    LOG_INFO("Getting user files attributes");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& file : *files) {
            auto objectIdPath = FromObjectId(file.ObjectId);
            {
                auto req = TYPathProxy::Get(objectIdPath + "/@");
                SetTransactionId(req, InputTransaction->GetId());
                std::vector<Stroka> attributeKeys;
                attributeKeys.push_back("file_name");
                switch (file.Type) {
                    case EObjectType::File:
                        attributeKeys.push_back("executable");
                        break;

                    case EObjectType::Table:
                        attributeKeys.push_back("format");
                        break;

                    default:
                        YUNREACHABLE();
                }
                attributeKeys.push_back("key");
                attributeKeys.push_back("chunk_count");
                attributeKeys.push_back("uncompressed_data_size");
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                batchReq->AddRequest(req, "get_attributes");
            }

            {
                auto req = TYPathProxy::Get(file.Path.GetPath() + "&/@");
                SetTransactionId(req, InputTransaction->GetId());
                std::vector<Stroka> attributeKeys;
                attributeKeys.push_back("key");
                attributeKeys.push_back("file_name");
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                batchReq->AddRequest(req, "get_link_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting attributes of user files");
        const auto& batchRsp = batchRspOrError.Value();

        TEnumIndexedVector<yhash_set<Stroka>, EOperationStage> userFileNames;
        auto validateUserFileName = [&] (const TUserFile& file) {
            // TODO(babenko): more sanity checks?
            auto path = file.Path.GetPath();
            const auto& fileName = file.FileName;
            if (fileName.empty()) {
                THROW_ERROR_EXCEPTION("Empty user file name for %v",
                    path);
            }
            if (!userFileNames[file.Stage].insert(fileName).second) {
                THROW_ERROR_EXCEPTION("Duplicate user file name %Qv for %v",
                    fileName,
                    path);
            }
        };

        auto getAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_attributes");
        auto getLinkAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_link_attributes");
        for (int index = 0; index < files->size(); ++index) {
            auto& file = (*files)[index];
            const auto& path = file.Path.GetPath();

            {
                const auto& rspOrError = getAttributesRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting attributes of user file %Qv", path);
                const auto& rsp = rspOrError.Value();
                const auto& linkRsp = getLinkAttributesRspsOrError[index];

                file.Attributes = ConvertToAttributes(TYsonString(rsp->value()));
                const auto& attributes = *file.Attributes;

                try {
                    if (linkRsp.IsOK()) {
                        auto linkAttributes = ConvertToAttributes(TYsonString(linkRsp.Value()->value()));
                        file.FileName = linkAttributes->Get<Stroka>("key");
                        file.FileName = linkAttributes->Find<Stroka>("file_name").Get(file.FileName);
                    } else {
                        file.FileName = attributes.Get<Stroka>("key");
                        file.FileName = attributes.Find<Stroka>("file_name").Get(file.FileName);
                    }
                    file.FileName = file.Path.GetFileName().Get(file.FileName);
                } catch (const std::exception& ex) {
                    // NB: Some of the above Gets and Finds may throw due to, e.g., type mismatch.
                    THROW_ERROR_EXCEPTION("Error parsing attributes of user file %v",
                        path) << ex;
                }

                switch (file.Type) {
                    case EObjectType::File:
                        file.Executable = attributes.Find<bool>("executable").Get(file.Executable);
                        file.Executable = file.Path.GetExecutable().Get(file.Executable);
                        break;

                    case EObjectType::Table:
                        file.Format = attributes.FindYson("format").Get(TYsonString());
                        file.Format = file.Path.GetFormat().Get(file.Format);
                        // Validate that format is correct.
                        try {
                            if (file.Format.GetType() == EYsonType::None) {
                                THROW_ERROR_EXCEPTION("Format is missing");
                            } else {
                                ConvertTo<TFormat>(file.Format);
                            }
                        } catch (const std::exception& ex) {
                            THROW_ERROR_EXCEPTION("Failed to parse format of table file %v",
                                file.Path) << ex;
                        }
                        break;

                    default:
                        YUNREACHABLE();
                }

                i64 fileSize = attributes.Get<i64>("uncompressed_data_size");
                if (fileSize > Config->MaxFileSize) {
                    THROW_ERROR_EXCEPTION(
                        "User file %v exceeds size limit: %v > %v",
                        path,
                        fileSize,
                        Config->MaxFileSize);
                }

                i64 chunkCount = attributes.Get<i64>("chunk_count");
                if (chunkCount > Config->MaxChunksPerFetch) {
                    THROW_ERROR_EXCEPTION(
                        "User file %v exceeds chunk count limit: %v > %v",
                        path,
                        chunkCount,
                        Config->MaxChunksPerFetch);
                }

                LOG_INFO("User file locked (Path: %v, Stage: %v, FileName: %v)",
                    path,
                    file.Stage,
                    file.FileName);
            }

            validateUserFileName(file);
        }
    }
}

void TOperationControllerBase::InitQuerySpec(
    NProto::TSchedulerJobSpecExt* schedulerJobSpecExt,
    const Stroka& queryString,
    const TTableSchema& schema)
{
    auto externalCGInfo = New<TExternalCGInfo>();
    auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
    auto fetchFunctions = [&] (const std::vector<Stroka>& names, const TTypeInferrerMapPtr& typeInferrers) {
        MergeFrom(typeInferrers.Get(), BuiltinTypeInferrersMap.Get());

        std::vector<Stroka> externalNames;
        for (const auto& name : names) {
            auto found = typeInferrers->find(name);
            if (found == typeInferrers->end()) {
                externalNames.push_back(name);
            }
        }

        if (externalNames.empty()) {
            return;
        }

        if (!Config->UdfRegistryPath) {
            THROW_ERROR_EXCEPTION("External UDF registry is not configured");
        }

        auto descriptors = LookupAllUdfDescriptors(externalNames, Config->UdfRegistryPath.Get(), Host->GetMasterClient());

        AppendUdfDescriptors(typeInferrers, externalCGInfo, externalNames, descriptors);
    };

    auto query = PrepareJobQuery(queryString, schema, fetchFunctions);

    auto* querySpec = schedulerJobSpecExt->mutable_input_query_spec();
    ToProto(querySpec->mutable_query(), query);
    ToProto(querySpec->mutable_external_functions(), externalCGInfo->Functions);

    externalCGInfo->NodeDirectory->DumpTo(querySpec->mutable_node_directory());
}

void TOperationControllerBase::CollectTotals()
{
    for (const auto& table : InputTables) {
        for (const auto& chunkSpec : table.Chunks) {
            if (IsUnavailable(chunkSpec, IsParityReplicasFetchEnabled())) {
                const auto& chunkId = chunkSpec->ChunkId();
                switch (Spec->UnavailableChunkStrategy) {
                    case EUnavailableChunkAction::Fail:
                        THROW_ERROR_EXCEPTION("Input chunk %v is unavailable",
                            chunkId);

                    case EUnavailableChunkAction::Skip:
                        LOG_TRACE("Skipping unavailable chunk (ChunkId: %v)",
                            chunkId);
                        continue;

                    case EUnavailableChunkAction::Wait:
                        // Do nothing.
                        break;

                    default:
                        YUNREACHABLE();
                }
            }

            if (table.IsPrimary()) {
                PrimaryInputDataSize_ += chunkSpec->GetUncompressedDataSize();
            }

            TotalEstimatedInputDataSize += chunkSpec->GetUncompressedDataSize();
            TotalEstimatedInputRowCount += chunkSpec->GetRowCount();
            TotalEstimatedCompressedDataSize += chunkSpec->GetCompressedDataSize();
            ++TotalEstimatedInputChunkCount;
        }
    }

    LOG_INFO("Estimated input totals collected (ChunkCount: %v, DataSize: %v, RowCount: %v, CompressedDataSize: %v)",
        TotalEstimatedInputChunkCount,
        TotalEstimatedInputDataSize,
        TotalEstimatedInputRowCount,
        TotalEstimatedCompressedDataSize);
}

void TOperationControllerBase::CustomPrepare()
{ }

void TOperationControllerBase::ClearInputChunkBoundaryKeys()
{
    for (auto& pair : InputChunkMap) {
        auto& inputChunkDescriptor = pair.second;
        for (auto chunkSpec : inputChunkDescriptor.InputChunks) {
            // We don't need boundary key ext after preparation phase.
            chunkSpec->ReleaseBoundaryKeys();
        }
    }
}

// NB: must preserve order of chunks in the input tables, no shuffling.
std::vector<TInputChunkPtr> TOperationControllerBase::CollectPrimaryInputChunks() const
{
    std::vector<TInputChunkPtr> result;
    for (const auto& table : InputTables) {
        if (!table.IsForeign()) {
            for (const auto& chunkSpec : table.Chunks) {
                if (IsUnavailable(chunkSpec, IsParityReplicasFetchEnabled())) {
                    switch (Spec->UnavailableChunkStrategy) {
                        case EUnavailableChunkAction::Skip:
                            continue;

                        case EUnavailableChunkAction::Wait:
                            // Do nothing.
                            break;

                        default:
                            YUNREACHABLE();
                    }
                }
                result.push_back(chunkSpec);
            }
        }
    }
    return result;
}

std::vector<std::deque<TInputChunkPtr>> TOperationControllerBase::CollectForeignInputChunks() const
{
    std::vector<std::deque<TInputChunkPtr>> result;
    for (const auto& table : InputTables) {
        if (table.IsForeign()) {
            result.push_back(std::deque<TInputChunkPtr>());
            for (const auto& chunkSpec : table.Chunks) {
                if (IsUnavailable(chunkSpec, IsParityReplicasFetchEnabled())) {
                    switch (Spec->UnavailableChunkStrategy) {
                        case EUnavailableChunkAction::Skip:
                            continue;

                        case EUnavailableChunkAction::Wait:
                            // Do nothing.
                            break;

                        default:
                            YUNREACHABLE();
                    }
                }
                result.back().push_back(chunkSpec);
            }
        }
    }
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceChunks(
    const std::vector<TInputChunkPtr>& chunkSpecs,
    i64 maxSliceDataSize,
    TJobSizeLimits* jobSizeLimits)
{
    std::vector<TChunkStripePtr> result;
    auto appendStripes = [&] (const std::vector<TInputSlicePtr>& slices) {
        for (const auto& slice : slices) {
            result.push_back(New<TChunkStripe>(slice));
        }
    };

    double multiplier = Config->SliceDataSizeMultiplier;
    // Non-trivial multiplier should be used only if input data size is large enough.
    // Otherwise we do not want to have more slices than job count.
    if (TotalEstimatedInputDataSize < maxSliceDataSize) {
        multiplier = 1.0;
    }
    i64 sliceDataSize = Clamp(static_cast<i64>(jobSizeLimits->GetDataSizePerJob() * multiplier), 1, maxSliceDataSize); 

    for (const auto& chunkSpec : chunkSpecs) {
        int oldSize = result.size();

        bool hasNontrivialLimits = !chunkSpec->IsCompleteChunk();

        auto codecId = NErasure::ECodec(chunkSpec->GetErasureCodec());
        if (hasNontrivialLimits || codecId == NErasure::ECodec::None) {
            auto slices = SliceChunkByRowIndexes(chunkSpec, sliceDataSize);
            appendStripes(slices);
        } else {
            for (const auto& slice : CreateErasureInputSlices(chunkSpec, codecId)) {
                auto slices = slice->SliceEvenly(sliceDataSize);
                appendStripes(slices);
            }
        }

        LOG_TRACE("Slicing chunk (ChunkId: %v, SliceCount: %v)",
            chunkSpec->ChunkId(),
            result.size() - oldSize);
    }

    i64 stripeCount = result.size();
    i64 minJobCount = DivCeil(stripeCount, Config->MaxChunkStripesPerJob);
    i64 jobCount = Clamp(jobSizeLimits->GetJobCount(), minJobCount, stripeCount);
    jobSizeLimits->SetJobCount(jobCount);
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceInputChunks(
    i64 maxSliceDataSize,
    TJobSizeLimits* jobSizeLimits)
{
    return SliceChunks(CollectPrimaryInputChunks(), maxSliceDataSize, jobSizeLimits);
}

TKeyColumns TOperationControllerBase::CheckInputTablesSorted(
    const TKeyColumns& keyColumns,
    std::function<bool(const TInputTable& table)> inputTableFilter)
{
    YCHECK(!InputTables.empty());

    for (const auto& table : InputTables) {
        if (inputTableFilter(table) && !table.Schema.IsSorted()) {
            THROW_ERROR_EXCEPTION("Input table %v is not sorted",
                table.Path.GetPath());
        }
    }

    auto validateColumnFilter = [] (const TInputTable& table, const TKeyColumns& keyColumns) {
        for (const auto& keyColumn : keyColumns) {
            if (!table.Path.GetChannel().Contains(keyColumn)) {
                THROW_ERROR_EXCEPTION("Columm filter for input table %v doesn't include key column %Qv",
                    table.Path.GetPath(),
                    keyColumn);
            }
        }
    };

    if (!keyColumns.empty()) {
        for (const auto& table : InputTables) {
            if (!inputTableFilter(table)) {
                continue;
            }

            if (!CheckKeyColumnsCompatible(table.Schema.GetKeyColumns(), keyColumns)) {
                THROW_ERROR_EXCEPTION("Input table %v is sorted by columns %v that are not compatible "
                    "with the requested columns %v",
                    table.Path.GetPath(),
                    table.Schema.GetKeyColumns(),
                    keyColumns);
            }
            validateColumnFilter(table, keyColumns);
        }
        return keyColumns;
    } else {
        for (const auto& referenceTable : InputTables) {
            if (inputTableFilter(referenceTable)) {
                for (const auto& table : InputTables) {
                    if (!inputTableFilter(table)) {
                        continue;
                    }

                    if (table.Schema.GetKeyColumns() != referenceTable.Schema.GetKeyColumns()) {
                        THROW_ERROR_EXCEPTION("Key columns do not match: input table %v is sorted by columns %v "
                            "while input table %v is sorted by columns %v",
                            table.Path.GetPath(),
                            table.Schema.GetKeyColumns(),
                            referenceTable.Path.GetPath(),
                            referenceTable.Schema.GetKeyColumns());
                    }
                    validateColumnFilter(table, referenceTable.Schema.GetKeyColumns());
                }
                return referenceTable.Schema.GetKeyColumns();
            }
        }
    }
    YUNREACHABLE();
}

bool TOperationControllerBase::CheckKeyColumnsCompatible(
    const TKeyColumns& fullColumns,
    const TKeyColumns& prefixColumns)
{
    if (fullColumns.size() < prefixColumns.size()) {
        return false;
    }

    for (int index = 0; index < prefixColumns.size(); ++index) {
        if (fullColumns[index] != prefixColumns[index]) {
            return false;
        }
    }

    return true;
}


bool TOperationControllerBase::ShouldVerifySortedOutput() const
{
    return true;
}

bool TOperationControllerBase::IsParityReplicasFetchEnabled() const
{
    return false;
}

bool TOperationControllerBase::IsBoundaryKeysFetchEnabled() const
{
    return false;
}

void TOperationControllerBase::RegisterOutput(
    const TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex,
    TOutputTable& table)
{
    if (!chunkTreeId) {
        return;
    }

    table.OutputChunkTreeIds.insert(std::make_pair(key, chunkTreeId));

    if (IsOutputLivePreviewSupported()) {
        auto masterConnector = Host->GetMasterConnector();
        masterConnector->AttachToLivePreview(
            OperationId,
            AsyncSchedulerTransaction->GetId(),
            table.LivePreviewTableId,
            {chunkTreeId});
    }

    LOG_DEBUG("Output chunk tree registered (Table: %v, ChunkTreeId: %v, Key: %v)",
        tableIndex,
        chunkTreeId,
        key);
}

void TOperationControllerBase::RegisterBoundaryKeys(
    const TOutputResult& boundaryKeys,
    const TChunkTreeId& chunkTreeId,
    TOutputTable* outputTable)
{
    if (boundaryKeys.empty()) {
        return;
    }

    YCHECK(boundaryKeys.sorted());
    YCHECK(!outputTable->Options->ValidateUniqueKeys || boundaryKeys.unique_keys());

    auto trimAndCaptureKey = [&] (const TOwningKey& key) {
        int limit = outputTable->TableUploadOptions.TableSchema.GetKeyColumnCount();
        if (key.GetCount() > limit) {
            // NB: This can happen for a teleported chunk from a table with a wider key in sorted (but not unique_keys) mode.
            YCHECK(!outputTable->Options->ValidateUniqueKeys);
            return RowBuffer->Capture(key.Begin(), limit);
        } else {
            return RowBuffer->Capture(key.Begin(), key.GetCount());
        }
    };

    outputTable->BoundaryKeys.push_back(TJobBoundaryKeys{
        trimAndCaptureKey(FromProto<TOwningKey>(boundaryKeys.min())),
        trimAndCaptureKey(FromProto<TOwningKey>(boundaryKeys.max())),
        chunkTreeId
    });
}

void TOperationControllerBase::RegisterOutput(
    TInputChunkPtr chunkSpec,
    int key,
    int tableIndex)
{
    auto& table = OutputTables[tableIndex];

    if (table.TableUploadOptions.TableSchema.IsSorted() && ShouldVerifySortedOutput()) {
        YCHECK(chunkSpec->BoundaryKeys());

        TOutputResult resultBoundaryKeys;
        // Chunk must have at least one row.
        YCHECK(chunkSpec->GetRowCount() > 0);
        resultBoundaryKeys.set_empty(false);
        resultBoundaryKeys.set_sorted(true);
        resultBoundaryKeys.set_unique_keys(chunkSpec->GetUniqueKeys());
        ToProto(resultBoundaryKeys.mutable_min(), chunkSpec->BoundaryKeys()->MinKey);
        ToProto(resultBoundaryKeys.mutable_max(), chunkSpec->BoundaryKeys()->MaxKey);

        RegisterBoundaryKeys(resultBoundaryKeys, chunkSpec->ChunkId(), &table);
    }

    RegisterOutput(chunkSpec->ChunkId(), key, tableIndex, table);
}

void TOperationControllerBase::RegisterOutput(
    TJobletPtr joblet,
    int key,
    const TCompletedJobSummary& jobSummary)
{
    const auto& result = jobSummary.Result;
    const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    for (int tableIndex = 0; tableIndex < OutputTables.size(); ++tableIndex) {
        auto& table = OutputTables[tableIndex];
        RegisterOutput(joblet->ChunkListIds[tableIndex], key, tableIndex, table);

        if (table.TableUploadOptions.TableSchema.IsSorted() && ShouldVerifySortedOutput() && !jobSummary.Abandoned) {
            YCHECK(tableIndex < schedulerResultExt.output_boundary_keys_size());
            const auto& boundaryKeys = schedulerResultExt.output_boundary_keys(tableIndex);
            RegisterBoundaryKeys(boundaryKeys, joblet->ChunkListIds[tableIndex], &table);
        }
    }
}

void TOperationControllerBase::RegisterInputStripe(TChunkStripePtr stripe, TTaskPtr task)
{
    yhash_set<TChunkId> visitedChunks;

    TStripeDescriptor stripeDescriptor;
    stripeDescriptor.Stripe = stripe;
    stripeDescriptor.Task = task;
    stripeDescriptor.Cookie = task->GetChunkPoolInput()->Add(stripe);

    for (const auto& slice : stripe->ChunkSlices) {
        auto chunkSpec = slice->GetInputChunk();
        const auto& chunkId = chunkSpec->ChunkId();

        // Insert an empty TInputChunkDescriptor if a new chunkId is encountered.
        auto& chunkDescriptor = InputChunkMap[chunkId];

        if (InputChunkSpecs.insert(chunkSpec).second) {
            chunkDescriptor.InputChunks.push_back(chunkSpec);
        }

        if (IsUnavailable(chunkSpec, IsParityReplicasFetchEnabled())) {
            chunkDescriptor.State = EInputChunkState::Waiting;
        }

        if (visitedChunks.insert(chunkId).second) {
            chunkDescriptor.InputStripes.push_back(stripeDescriptor);
        }
    }
}

void TOperationControllerBase::RegisterIntermediate(
    TJobletPtr joblet,
    TCompletedJobPtr completedJob,
    TChunkStripePtr stripe,
    bool attachToLivePreview)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        const auto& chunkId = chunkSlice->GetInputChunk()->ChunkId();
        YCHECK(ChunkOriginMap.insert(std::make_pair(chunkId, completedJob)).second);

        if (attachToLivePreview && IsIntermediateLivePreviewSupported()) {
            auto masterConnector = Host->GetMasterConnector();
            masterConnector->AttachToLivePreview(
                OperationId,
                AsyncSchedulerTransaction->GetId(),
                IntermediateTable.LivePreviewTableId,
                {chunkId});
        }
    }
}

bool TOperationControllerBase::HasEnoughChunkLists(bool intermediate)
{
    if (intermediate) {
        return ChunkListPool->HasEnough(IntermediateOutputCellTag, 1);
    } else {
        for (const auto& pair : CellTagToOutputTableCount) {
            if (!ChunkListPool->HasEnough(pair.first, pair.second)) {
                return false;
            }
        }
        return true;
    }
}

TChunkListId TOperationControllerBase::ExtractChunkList(TCellTag cellTag)
{
    return ChunkListPool->Extract(cellTag);
}

void TOperationControllerBase::ReleaseChunkLists(const std::vector<TChunkListId>& ids)
{
    ChunkListPool->Release(ids);
}

void TOperationControllerBase::RegisterJoblet(TJobletPtr joblet)
{
    YCHECK(JobletMap.insert(std::make_pair(joblet->JobId, joblet)).second);
}

TOperationControllerBase::TJobletPtr TOperationControllerBase::FindJoblet(const TJobId& jobId) const
{
    auto it = JobletMap.find(jobId);
    return it == JobletMap.end() ? nullptr : it->second;
}

TOperationControllerBase::TJobletPtr TOperationControllerBase::GetJoblet(const TJobId& jobId) const
{
    auto joblet = FindJoblet(jobId);
    YCHECK(joblet);
    return joblet;
}

TOperationControllerBase::TJobletPtr TOperationControllerBase::GetJobletOrThrow(const TJobId& jobId) const
{
    auto joblet = FindJoblet(jobId);
    if (!joblet) {
        THROW_ERROR_EXCEPTION("No such job %v", jobId);
    }
    return joblet;
}

void TOperationControllerBase::RemoveJoblet(const TJobId& jobId)
{
    YCHECK(JobletMap.erase(jobId) == 1);
}

bool TOperationControllerBase::HasProgress() const
{
    return IsPrepared();
}

void TOperationControllerBase::BuildOperationAttributes(IYsonConsumer* consumer) const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    BuildYsonMapFluently(consumer)
        .Item("sync_scheduler_transaction_id").Value(SyncSchedulerTransaction ? SyncSchedulerTransaction->GetId() : NullTransactionId)
        .Item("async_scheduler_transaction_id").Value(AsyncSchedulerTransaction ? AsyncSchedulerTransaction->GetId() : NullTransactionId)
        .Item("input_transaction_id").Value(InputTransaction ? InputTransaction->GetId() : NullTransactionId)
        .Item("output_transaction_id").Value(OutputTransaction ? OutputTransaction->GetId() : NullTransactionId);
}

void TOperationControllerBase::BuildProgress(IYsonConsumer* consumer) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    BuildYsonMapFluently(consumer)
        .Item("jobs").Value(JobCounter)
        .Item("ready_job_count").Value(GetPendingJobCount())
        .Item("job_statistics").Value(JobStatistics)
        .Item("estimated_input_statistics").BeginMap()
            .Item("chunk_count").Value(TotalEstimatedInputChunkCount)
            .Item("uncompressed_data_size").Value(TotalEstimatedInputDataSize)
            .Item("compressed_data_size").Value(TotalEstimatedCompressedDataSize)
            .Item("row_count").Value(TotalEstimatedInputRowCount)
            .Item("unavailable_chunk_count").Value(UnavailableInputChunkCount)
        .EndMap()
        .Item("live_preview").BeginMap()
            .Item("output_supported").Value(IsOutputLivePreviewSupported())
            .Item("intermediate_supported").Value(IsIntermediateLivePreviewSupported())
        .EndMap();
}

void TOperationControllerBase::BuildBriefProgress(IYsonConsumer* consumer) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    BuildYsonMapFluently(consumer)
        .Item("jobs").Value(JobCounter);
}

void TOperationControllerBase::UpdateJobStatistics(const TJobSummary& jobSummary)
{
    // NB: There is a copy happening here that can be eliminated.
    auto statistics = jobSummary.Statistics;
    LOG_TRACE("Job data statistics (JobId: %v, Input: %v, Output: %v)",
        jobSummary.Id,
        GetTotalInputDataStatistics(statistics),
        GetTotalOutputDataStatistics(statistics));

    statistics.AddSuffixToNames(jobSummary.StatisticsSuffix);
    JobStatistics.Update(statistics);
}

void TOperationControllerBase::BuildBriefSpec(IYsonConsumer* consumer) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    BuildYsonMapFluently(consumer)
        .DoIf(Spec->Title.HasValue(), [&] (TFluentMap fluent) {
            fluent
                .Item("title").Value(*Spec->Title);
        })
        .Item("input_table_paths").ListLimited(GetInputTablePaths(), 1)
        .Item("output_table_paths").ListLimited(GetOutputTablePaths(), 1);
}

TNullable<TYsonString> TOperationControllerBase::BuildInputPathYson(const TJobId& jobId) const
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto joblet = GetJobletOrThrow(jobId);
    return BuildInputPaths(
        GetInputTablePaths(),
        joblet->InputStripeList,
        Operation->GetType(),
        joblet->JobType);
}

std::vector<TOperationControllerBase::TPathWithStage> TOperationControllerBase::GetFilePaths() const
{
    return std::vector<TPathWithStage>();
}

bool TOperationControllerBase::IsRowCountPreserved() const
{
    return false;
}

int TOperationControllerBase::GetMaxJobCount(
    TNullable<int> userMaxJobCount,
    int maxJobCount)
{
    if (userMaxJobCount) {
        maxJobCount = std::min(maxJobCount, *userMaxJobCount);
    }
    return maxJobCount;
}

void TOperationControllerBase::InitUserJobSpecTemplate(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TUserJobSpecPtr config,
    const std::vector<TUserFile>& files,
    const Stroka& fileAccount)
{
    jobSpec->set_shell_command(config->Command);
    if (config->JobTimeLimit) {
        jobSpec->set_job_time_limit(config->JobTimeLimit.Get().MilliSeconds());
    }
    jobSpec->set_memory_limit(config->MemoryLimit);
    jobSpec->set_include_memory_mapped_files(config->IncludeMemoryMappedFiles);
    jobSpec->set_iops_threshold(config->IopsThreshold);
    jobSpec->set_use_yamr_descriptors(config->UseYamrDescriptors);
    jobSpec->set_check_input_fully_consumed(config->CheckInputFullyConsumed);
    jobSpec->set_max_stderr_size(config->MaxStderrSize);
    jobSpec->set_enable_core_dump(config->EnableCoreDump);
    jobSpec->set_custom_statistics_count_limit(config->CustomStatisticsCountLimit);
    jobSpec->set_copy_files(config->CopyFiles);
    jobSpec->set_file_account(fileAccount);

    if (config->TmpfsPath && Config->EnableTmpfs) {
        auto tmpfsSize = config->TmpfsSize
            ? *config->TmpfsSize
            : config->MemoryLimit;
        jobSpec->set_tmpfs_size(tmpfsSize);
        jobSpec->set_tmpfs_path(*config->TmpfsPath);
    }

    if (Config->IopsThreshold) {
        jobSpec->set_iops_threshold(*Config->IopsThreshold);
        if (Config->IopsThrottlerLimit) {
            jobSpec->set_iops_throttler_limit(*Config->IopsThrottlerLimit);
        }
    }

    {
        // Set input and output format.
        TFormat inputFormat(EFormatType::Yson);
        TFormat outputFormat(EFormatType::Yson);

        if (config->Format) {
            inputFormat = outputFormat = *config->Format;
        }

        if (config->InputFormat) {
            inputFormat = *config->InputFormat;
        }

        if (config->OutputFormat) {
            outputFormat = *config->OutputFormat;
        }

        jobSpec->set_input_format(ConvertToYsonString(inputFormat).Data());
        jobSpec->set_output_format(ConvertToYsonString(outputFormat).Data());
    }

    auto fillEnvironment = [&] (yhash_map<Stroka, Stroka>& env) {
        for (const auto& pair : env) {
            jobSpec->add_environment(Format("%v=%v", pair.first, pair.second));
        }
    };

    // Global environment.
    fillEnvironment(Config->Environment);

    // Local environment.
    fillEnvironment(config->Environment);

    jobSpec->add_environment(Format("YT_OPERATION_ID=%v", OperationId));

    for (const auto& file : files) {
        auto *descriptor = jobSpec->add_files();
        descriptor->set_type(static_cast<int>(file.Type));
        descriptor->set_file_name(file.FileName);
        ToProto(descriptor->mutable_chunks(), file.ChunkSpecs);
        switch (file.Type) {
            case EObjectType::File:
                descriptor->set_executable(file.Executable);
                break;
            case EObjectType::Table:
                descriptor->set_format(file.Format.Data());
                break;
            default:
                YUNREACHABLE();
        }
    }
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TJobletPtr joblet)
{
    ToProto(jobSpec->mutable_async_scheduler_transaction_id(), AsyncSchedulerTransaction->GetId());

    i64 memoryReserve = joblet->EstimatedResourceUsage.GetUserJobMemory() * joblet->UserJobMemoryReserveFactor;
    // Memory reserve should greater than or equal to tmpfs_size (see YT-5518 for more details).
    // This is ensured by adjusting memory reserve factor in user job config as initialization,
    // but just in case we also limit the actual memory_reserve value here.
    if (jobSpec->has_tmpfs_size()) {
        memoryReserve = std::max(memoryReserve, jobSpec->tmpfs_size());
    }
    jobSpec->set_memory_reserve(memoryReserve);

    jobSpec->add_environment(Format("YT_JOB_INDEX=%v", joblet->JobIndex));
    jobSpec->add_environment(Format("YT_JOB_ID=%v", joblet->JobId));
    if (joblet->StartRowIndex >= 0) {
        jobSpec->add_environment(Format("YT_START_ROW_INDEX=%v", joblet->StartRowIndex));
    }

    if (Operation->GetSecureVault()) {
        // NB: These environment variables should be added to user job spec, not to the user job spec template.
        // They may contain sensitive information that should not be persisted with a controller.

        // We add a single variable storing the whole secure vault and all top-level scalar values.
        jobSpec->add_environment(Format("YT_SECURE_VAULT=%v",
            ConvertToYsonString(Operation->GetSecureVault(), EYsonFormat::Text)));

        for (const auto& pair : Operation->GetSecureVault()->GetChildren()) {
            Stroka value;
            auto node = pair.second;
            if (node->GetType() == ENodeType::Int64) {
                value = ToString(node->GetValue<i64>());
            } else if (node->GetType() == ENodeType::Uint64) {
                value = ToString(node->GetValue<ui64>());
            } else if (node->GetType() == ENodeType::Boolean) {
                value = ToString(node->GetValue<bool>());
            } else if (node->GetType() == ENodeType::Double) {
                value = ToString(node->GetValue<double>());
            } else if (node->GetType() == ENodeType::String) {
                value = node->GetValue<Stroka>();
            } else {
                // We do not export composite values as a separate environment variables.
                continue;
            }
            jobSpec->add_environment(Format("YT_SECURE_VAULT_%v=\"%v\"", pair.first, value));
        }
    }
}

i64 TOperationControllerBase::GetFinalOutputIOMemorySize(TJobIOConfigPtr ioConfig) const
{
    i64 result = 0;
    for (const auto& outputTable : OutputTables) {
        if (outputTable.Options->ErasureCodec == NErasure::ECodec::None) {
            i64 maxBufferSize = std::max(
                ioConfig->TableWriter->MaxRowWeight,
                ioConfig->TableWriter->MaxBufferSize);
            result += GetOutputWindowMemorySize(ioConfig) + maxBufferSize;
        } else {
            auto* codec = NErasure::GetCodec(outputTable.Options->ErasureCodec);
            double replicationFactor = (double) codec->GetTotalPartCount() / codec->GetDataPartCount();
            result += static_cast<i64>(ioConfig->TableWriter->DesiredChunkSize * replicationFactor);
        }
    }
    return result;
}

i64 TOperationControllerBase::GetFinalIOMemorySize(
    TJobIOConfigPtr ioConfig,
    const TChunkStripeStatisticsVector& stripeStatistics) const
{
    i64 result = 0;
    for (const auto& stat : stripeStatistics) {
        result += GetInputIOMemorySize(ioConfig, stat);
    }
    result += GetFinalOutputIOMemorySize(ioConfig);
    return result;
}

void TOperationControllerBase::InitIntermediateOutputConfig(TJobIOConfigPtr config)
{
    // Don't replicate intermediate output.
    config->TableWriter->UploadReplicationFactor = 1;
    config->TableWriter->MinUploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->PopulateCache = true;

    // Don't sync intermediate chunks.
    config->TableWriter->SyncOnClose = false;

    // Distribute intermediate chunks uniformly across storage locations.
    config->TableWriter->EnableUniformPlacement = true;
}

void TOperationControllerBase::InitFinalOutputConfig(TJobIOConfigPtr /* config */)
{ }

NTableClient::TTableReaderOptionsPtr TOperationControllerBase::CreateTableReaderOptions(TJobIOConfigPtr ioConfig)
{
    auto options = New<TTableReaderOptions>();
    options->EnableRowIndex = ioConfig->ControlAttributes->EnableRowIndex;
    options->EnableTableIndex = ioConfig->ControlAttributes->EnableTableIndex;
    options->EnableRangeIndex = ioConfig->ControlAttributes->EnableRangeIndex;
    return options;
}

NTableClient::TTableReaderOptionsPtr TOperationControllerBase::CreateIntermediateTableReaderOptions()
{
    auto options = New<TTableReaderOptions>();
    options->AllowFetchingSeedsFromMaster = false;
    return options;
}

IClientPtr TOperationControllerBase::CreateClient()
{
    TClientOptions options;
    options.User = AuthenticatedUser;
    return Host
        ->GetMasterClient()
        ->GetConnection()
        ->CreateClient(options);
}

void TOperationControllerBase::ValidateUserFileCount(TUserJobSpecPtr spec, const Stroka& operation)
{
    if (spec && spec->FilePaths.size() > Config->MaxUserFileCount) {
        THROW_ERROR_EXCEPTION("Too many user files in %v: maximum allowed %v, actual %v",
            operation,
            Config->MaxUserFileCount,
            spec->FilePaths.size());
    }
}

void TOperationControllerBase::GetExecNodesInformation()
{
    auto now = TInstant::Now();
    if (LastGetExecNodesInformationTime_ + Config->GetExecNodesInformationDelay > now) {
        return;
    }

    ExecNodeCount_ = Host->GetExecNodeCount();
    ExecNodesDescriptors_ = Host->GetExecNodeDescriptors(Spec->SchedulingTag);

    LastGetExecNodesInformationTime_ = TInstant::Now();
}

int TOperationControllerBase::GetExecNodeCount()
{
    GetExecNodesInformation();
    return ExecNodeCount_;
}

const std::vector<TExecNodeDescriptor>& TOperationControllerBase::GetExecNodeDescriptors()
{
    GetExecNodesInformation();
    return ExecNodesDescriptors_;
}

void TOperationControllerBase::BuildMemoryDigestStatistics(IYsonConsumer* consumer) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    BuildYsonMapFluently(consumer)
        .Item("job_proxy_memory_digest")
        .DoMapFor(JobProxyMemoryDigests_, [&] (
                TFluentMap fluent,
                const TMemoryDigestMap::value_type& item)
        {
            BuildYsonMapFluently(fluent)
                .Item(ToString(item.first)).Value(
                    item.second->GetQuantile(Config->JobProxyMemoryReserveQuantile));
        })
        .Item("user_job_memory_digest")
        .DoMapFor(JobProxyMemoryDigests_, [&] (
                TFluentMap fluent,
                const TMemoryDigestMap::value_type& item)
        {
            BuildYsonMapFluently(fluent)
                .Item(ToString(item.first)).Value(
                    item.second->GetQuantile(Config->UserJobMemoryReserveQuantile));
        });
}

void TOperationControllerBase::RegisterUserJobMemoryDigest(EJobType jobType, double memoryReserveFactor)
{
    YCHECK(UserJobMemoryDigests_.find(jobType) == UserJobMemoryDigests_.end());
    auto config = New<TLogDigestConfig>();
    config->LowerBound = memoryReserveFactor;
    config->UpperBound = 1.0;
    config->RelativePrecision = Config->UserJobMemoryDigestPrecision;
    UserJobMemoryDigests_[jobType] = CreateLogDigest(config);
}

IDigest* TOperationControllerBase::GetUserJobMemoryDigest(EJobType jobType)
{
    auto iter = UserJobMemoryDigests_.find(jobType);
    YCHECK(iter != UserJobMemoryDigests_.end());
    return iter->second.get();
}

const IDigest* TOperationControllerBase::GetUserJobMemoryDigest(EJobType jobType) const
{
    auto iter = UserJobMemoryDigests_.find(jobType);
    YCHECK(iter != UserJobMemoryDigests_.end());
    return iter->second.get();
}

void TOperationControllerBase::RegisterJobProxyMemoryDigest(EJobType jobType, const TLogDigestConfigPtr& config)
{
    YCHECK(JobProxyMemoryDigests_.find(jobType) == JobProxyMemoryDigests_.end());
    JobProxyMemoryDigests_[jobType] = CreateLogDigest(config);
}

void TOperationControllerBase::InferSchemaFromInputSorted(const TKeyColumns& keyColumns)
{
    // We infer schema only for operations with one output table.
    YCHECK(OutputTables.size() == 1);
    YCHECK(InputTables.size() >= 1);

    InferSchemaFromInputUnordered();

    if (OutputTables[0].TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
        OutputTables[0].TableUploadOptions.TableSchema = TTableSchema::FromKeyColumns(keyColumns);
    } else {
        OutputTables[0].TableUploadOptions.TableSchema =
            OutputTables[0].TableUploadOptions.TableSchema.ToSorted(keyColumns);
    }
}

void TOperationControllerBase::InferSchemaFromInputUnordered()
{
    // We infer schema only for operations with one output table.
    YCHECK(OutputTables.size() == 1);
    YCHECK(InputTables.size() >= 1);

    OutputTables[0].TableUploadOptions.SchemaMode = InputTables[0].SchemaMode;
    for (const auto& table : InputTables) {
        if (table.SchemaMode != OutputTables[0].TableUploadOptions.SchemaMode) {
            THROW_ERROR_EXCEPTION("Cannot infer output schema from input, tables have different schema modes");
        }
    }

    if (OutputTables[0].TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
        OutputTables[0].TableUploadOptions.TableSchema = TTableSchema();
    } else {
        auto schema = InputTables[0].Schema
            .ToStrippedColumnAttributes()
            .ToCanonical();

        for (const auto& table : InputTables) {
            if (table.Schema.ToStrippedColumnAttributes().ToCanonical() != schema) {
                THROW_ERROR_EXCEPTION("Cannot infer output schema from input in strong schema mode, tables have incompatible schemas");
            }
        }

        OutputTables[0].TableUploadOptions.TableSchema = schema;
    }
}

void TOperationControllerBase::InferSchemaFromInputOrdered()
{
    // We infer schema only for operations with one output table.
    YCHECK(OutputTables.size() == 1);
    YCHECK(InputTables.size() >= 1);

    auto& outputUploadOptions = OutputTables[0].TableUploadOptions;

    if (InputTables.size() == 1 && outputUploadOptions.UpdateMode == EUpdateMode::Overwrite) {
        // If only only one input table given, we inherit the whole schema including column attributes.
        outputUploadOptions.SchemaMode = InputTables[0].SchemaMode;
        outputUploadOptions.TableSchema = InputTables[0].Schema;
        return;
    }

    InferSchemaFromInputUnordered();
}

void TOperationControllerBase::ValidateOutputSchemaOrdered() const
{
    YCHECK(OutputTables.size() == 1);
    YCHECK(InputTables.size() >= 1);

    if (InputTables.size() > 1 && OutputTables[0].TableUploadOptions.TableSchema.IsSorted()) {
        THROW_ERROR_EXCEPTION("Cannot generate sorted output for ordered operation with multiple input tables")
            << TErrorAttribute("output_schema", OutputTables[0].TableUploadOptions.TableSchema);
    }
}

IDigest* TOperationControllerBase::GetJobProxyMemoryDigest(EJobType jobType)
{
    auto iter = JobProxyMemoryDigests_.find(jobType);
    YCHECK(iter != JobProxyMemoryDigests_.end());
    return iter->second.get();
}

const IDigest* TOperationControllerBase::GetJobProxyMemoryDigest(EJobType jobType) const
{
    auto iter = JobProxyMemoryDigests_.find(jobType);
    YCHECK(iter != JobProxyMemoryDigests_.end());
    return iter->second.get();
}

void TOperationControllerBase::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, TotalEstimatedInputChunkCount);
    Persist(context, TotalEstimatedInputDataSize);
    Persist(context, TotalEstimatedInputRowCount);
    Persist(context, TotalEstimatedCompressedDataSize);

    Persist(context, UnavailableInputChunkCount);

    Persist(context, JobCounter);

    Persist(context, InputNodeDirectory);
    Persist(context, AuxNodeDirectory);

    Persist(context, InputTables);

    Persist(context, OutputTables);

    Persist(context, IntermediateTable);

    Persist(context, Files);

    Persist(context, Tasks);

    Persist(context, TaskGroups);

    Persist(context, InputChunkMap);

    Persist(context, IntermediateOutputCellTag);

    Persist(context, CellTagToOutputTableCount);

    Persist(context, CachedPendingJobCount);

    Persist(context, CachedNeededResources);

    Persist(context, ChunkOriginMap);

    Persist(context, JobletMap);

    // NB: Scheduler snapshots need not be stable.
    Persist<
        TSetSerializer<
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, InputChunkSpecs);

    Persist(context, JobIndexGenerator);

    Persist(context, JobStatistics);

    Persist(context, RowCountLimitTableIndex);
    Persist(context, RowCountLimit);

    Persist<
        TMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, JobProxyMemoryDigests_);

    Persist<
        TMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, UserJobMemoryDigests_);

    if (context.IsLoad()) {
        for (const auto& task : Tasks) {
            task->Initialize();
        }
    }
}

TCodicilGuard TOperationControllerBase::MakeCodicilGuard() const
{
    return TCodicilGuard(CodicilData_);
}

////////////////////////////////////////////////////////////////////

//! Ensures that operation controllers are being destroyed in a
//! dedicated invoker.
class TOperationControllerWrapper
    : public IOperationController
{
public:
    TOperationControllerWrapper(
        const TOperationId& id,
        IOperationControllerPtr underlying,
        IInvokerPtr dtorInvoker)
        : Id_(id)
        , Underlying_(std::move(underlying))
        , DtorInvoker_(std::move(dtorInvoker))
    { }

    virtual ~TOperationControllerWrapper()
    {
        DtorInvoker_->Invoke(BIND([underlying = std::move(Underlying_), id = Id_] () mutable {
            auto Logger = OperationLogger;
            Logger.AddTag("OperationId: %v", id);
            LOG_INFO("Started destroying operation controller");
            underlying.Reset();
            LOG_INFO("Finished destroying operation controller");
        }));
    }

    virtual void Initialize() override
    {
        Underlying_->Initialize();
    }

    virtual void InitializeReviving(TControllerTransactionsPtr controllerTransactions) override
    {
        Underlying_->InitializeReviving(controllerTransactions);
    }

    virtual void Prepare() override
    {
        Underlying_->Prepare();
    }

    virtual void Materialize() override
    {
        Underlying_->Materialize();
    }

    virtual void Commit() override
    {
        Underlying_->Commit();
    }

    virtual void SaveSnapshot(TOutputStream* stream) override
    {
        Underlying_->SaveSnapshot(stream);
    }

    virtual void Revive() override
    {
        Underlying_->Revive();
    }

    virtual void Abort() override
    {
        Underlying_->Abort();
    }

    virtual std::vector<ITransactionPtr> GetTransactions() override
    {
        return Underlying_->GetTransactions();
    }

    virtual void Complete() override
    {
        Underlying_->Complete();
    }

    virtual TCancelableContextPtr GetCancelableContext() const override
    {
        return Underlying_->GetCancelableContext();
    }

    virtual IInvokerPtr GetCancelableControlInvoker() const override
    {
        return Underlying_->GetCancelableControlInvoker();
    }

    virtual IInvokerPtr GetCancelableInvoker() const override
    {
        return Underlying_->GetCancelableInvoker();
    }

    virtual IInvokerPtr GetInvoker() const override
    {
        return Underlying_->GetInvoker();
    }

    virtual TFuture<void> Suspend() override
    {
        return Underlying_->Suspend();
    }

    virtual void Resume() override
    {
        Underlying_->Resume();
    }

    virtual int GetPendingJobCount() const override
    {
        return Underlying_->GetPendingJobCount();
    }

    virtual int GetTotalJobCount() const override
    {
        return Underlying_->GetTotalJobCount();
    }

    virtual TJobResources GetNeededResources() const override
    {
        return Underlying_->GetNeededResources();
    }

    virtual void OnJobStarted(const TJobId& jobId, TInstant startTime) override
    {
        Underlying_->OnJobStarted(jobId, startTime);
    }

    virtual void OnJobCompleted(std::unique_ptr<TCompletedJobSummary> jobSummary) override
    {
        Underlying_->OnJobCompleted(std::move(jobSummary));
    }

    virtual void OnJobFailed(std::unique_ptr<TFailedJobSummary> jobSummary) override
    {
        Underlying_->OnJobFailed(std::move(jobSummary));
    }

    virtual void OnJobAborted(std::unique_ptr<TAbortedJobSummary> jobSummary) override
    {
        Underlying_->OnJobAborted(std::move(jobSummary));
    }

    virtual TScheduleJobResultPtr ScheduleJob(
        ISchedulingContextPtr context,
        const TJobResources& jobLimits) override
    {
        return Underlying_->ScheduleJob(std::move(context), jobLimits);
    }

    virtual void UpdateConfig(TSchedulerConfigPtr config) override
    {
        Underlying_->UpdateConfig(std::move(config));
    }

    virtual bool HasProgress() const override
    {
        return Underlying_->HasProgress();
    }

    virtual void BuildOperationAttributes(NYson::IYsonConsumer* consumer) const override
    {
        Underlying_->BuildOperationAttributes(consumer);
    }

    virtual void BuildProgress(IYsonConsumer* consumer) const override
    {
        Underlying_->BuildProgress(consumer);
    }

    virtual void BuildBriefProgress(IYsonConsumer* consumer) const override
    {
        Underlying_->BuildBriefProgress(consumer);
    }

    virtual Stroka GetLoggingProgress() const override
    {
        return Underlying_->GetLoggingProgress();
    }

    virtual void BuildMemoryDigestStatistics(IYsonConsumer* consumer) const override
    {
        Underlying_->BuildMemoryDigestStatistics(consumer);
    }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        Underlying_->BuildBriefSpec(consumer);
    }

    virtual TNullable<TYsonString> BuildInputPathYson(const TJobId& jobId) const override
    {
        return Underlying_->BuildInputPathYson(jobId);
    }

private:
    const TOperationId Id_;
    const IOperationControllerPtr Underlying_;
    const IInvokerPtr DtorInvoker_;
};

////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateControllerWrapper(
    const TOperationId& id,
    const IOperationControllerPtr& controller,
    const IInvokerPtr& dtorInvoker)
{
    return New<TOperationControllerWrapper>(id, controller, dtorInvoker);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

