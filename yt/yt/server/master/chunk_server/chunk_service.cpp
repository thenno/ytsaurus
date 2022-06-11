#include "chunk_service.h"
#include "private.h"
#include "config.h"
#include "chunk.h"
#include "chunk_manager.h"
#include "helpers.h"
#include "chunk_owner_base.h"
#include "medium.h"
#include "dynamic_store.h"
#include "chunk_owner_node_proxy.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>
#include <yt/yt/server/master/cell_master/master_hydra_service.h>
#include <yt/yt/server/master/cell_master/multicell_manager.h>

#include <yt/yt/server/master/node_tracker_server/node.h>
#include <yt/yt/server/master/node_tracker_server/node_directory_builder.h>
#include <yt/yt/server/master/node_tracker_server/node_tracker.h>

#include <yt/yt/server/master/sequoia_server/config.h>

#include <yt/yt/server/master/tablet_server/tablet_manager.h>

#include <yt/yt/server/master/transaction_server/transaction.h>
#include <yt/yt/server/master/transaction_server/transaction_replication_session.h>

#include <yt/yt/server/lib/hive/hive_manager.h>

#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/yt/ytlib/chunk_client/session_id.h>

#include <yt/yt/ytlib/hive/cell_directory.h>

#include <yt/yt/ytlib/object_client/helpers.h>

#include <yt/yt/core/rpc/helpers.h>

#include <yt/yt/library/erasure/impl/codec.h>

namespace NYT::NChunkServer {

using namespace NHydra;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NConcurrency;
using namespace NNodeTrackerServer;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NCellMaster;
using namespace NHydra;
using namespace NTransactionClient;
using namespace NRpc;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

class TChunkService
    : public NCellMaster::TMasterHydraServiceBase
{
public:
    explicit TChunkService(TBootstrap* bootstrap)
        : TMasterHydraServiceBase(
            bootstrap,
            TChunkServiceProxy::GetDescriptor(),
            EAutomatonThreadQueue::ChunkService,
            ChunkServerLogger)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(LocateChunks)
            .SetInvoker(GetGuardedAutomatonInvoker(EAutomatonThreadQueue::ChunkLocator))
            .SetHeavy(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(LocateDynamicStores)
            .SetInvoker(GetGuardedAutomatonInvoker(EAutomatonThreadQueue::ChunkLocator))
            .SetHeavy(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(TouchChunks)
            .SetInvoker(GetGuardedAutomatonInvoker(EAutomatonThreadQueue::ChunkLocator))
            .SetHeavy(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AllocateWriteTargets)
            .SetInvoker(GetGuardedAutomatonInvoker(EAutomatonThreadQueue::ChunkReplicaAllocator))
            .SetHeavy(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ExportChunks)
            .SetHeavy(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ImportChunks)
            .SetHeavy(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetChunkOwningNodes)
            .SetHeavy(true));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ExecuteBatch)
            .SetHeavy(true)
            .SetQueueSizeLimit(10000)
            .SetConcurrencyLimit(10000));

        DeclareServerFeature(EMasterFeature::OverlayedJournals);
    }

private:
    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, LocateChunks)
    {
        context->SetRequestInfo("SubrequestCount: %v",
            request->subrequests_size());

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::LeaderOrFollower);
        // TODO(shakurov): only sync with the leader is really needed,
        // not with the primary cell.
        SyncWithUpstream();

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        auto addressType = request->has_address_type()
            ? CheckedEnumCast<NNodeTrackerClient::EAddressType>(request->address_type())
            : NNodeTrackerClient::EAddressType::InternalRpc;
        TNodeDirectoryBuilder nodeDirectoryBuilder(response->mutable_node_directory(), addressType);

        const auto& cellDirectory = Bootstrap_->GetCellDirectory();
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto leaderChannel = cellDirectory->GetChannelByCellId(multicellManager->GetCellId(), EPeerKind::Leader);

        TChunkServiceProxy leaderProxy(std::move(leaderChannel));
        auto leaderRequest = leaderProxy.TouchChunks();
        leaderRequest->SetTimeout(context->GetTimeout());
        NRpc::SetCurrentAuthenticationIdentity(leaderRequest);

        const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
        bool follower = hydraManager->IsFollower();
        auto revision = hydraManager->GetAutomatonVersion().ToRevision();

        for (const auto& protoChunkId : request->subrequests()) {
            auto chunkId = FromProto<TChunkId>(protoChunkId);
            auto chunkIdWithIndex = DecodeChunkId(chunkId);

            auto* subresponse = response->add_subresponses();

            auto* chunk = chunkManager->FindChunk(chunkIdWithIndex.Id);
            if (!IsObjectAlive(chunk)) {
                subresponse->set_missing(true);
                continue;
            }

            TChunkPtrWithIndexes chunkWithIndexes(
                chunk,
                chunkIdWithIndex.ReplicaIndex,
                AllMediaIndex);
            auto replicas = chunkManager->LocateChunk(chunkWithIndexes);

            ToProto(subresponse->mutable_replicas(), replicas);
            subresponse->set_erasure_codec(static_cast<int>(chunk->GetErasureCodec()));
            for (auto replica : replicas) {
                nodeDirectoryBuilder.Add(replica);
            }

            if (follower && chunk->IsErasure() && !chunk->IsAvailable()) {
                ToProto(leaderRequest->add_subrequests(), chunkId);
            }
        }

        response->set_revision(revision);

        if (leaderRequest->subrequests_size() > 0) {
            YT_LOG_DEBUG("Touching chunks at leader (Count: %v)",
                leaderRequest->subrequests_size());
            leaderRequest->Invoke();
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, LocateDynamicStores)
    {
        context->SetRequestInfo("SubrequestCount: %v",
            request->subrequests_size());

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::LeaderOrFollower);
        SyncWithUpstream();

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        auto addressType = request->has_address_type()
            ? CheckedEnumCast<NNodeTrackerClient::EAddressType>(request->address_type())
            : NNodeTrackerClient::EAddressType::InternalRpc;
        TNodeDirectoryBuilder nodeDirectoryBuilder(response->mutable_node_directory(), addressType);

        const auto& sequoiaConfig = Bootstrap_->GetConfigManager()->GetConfig()->SequoiaManager;
        auto fetchChunkMetaFromSequoia = sequoiaConfig->Enable && sequoiaConfig->FetchChunkMetaFromSequoia;

        std::vector<TFuture<void>> metaFetchFutures;

        for (const auto& protoStoreId : request->subrequests()) {
            auto storeId = FromProto<TDynamicStoreId>(protoStoreId);
            auto* subresponse = response->add_subresponses();

            auto* dynamicStore = chunkManager->FindDynamicStore(storeId);
            if (!IsObjectAlive(dynamicStore) || dynamicStore->IsAbandoned()) {
                subresponse->set_missing(true);
                continue;
            }

            THashSet<int> extensionTags;
            if (!request->fetch_all_meta_extensions()) {
                extensionTags.insert(request->extension_tags().begin(), request->extension_tags().end());
            }

            if (dynamicStore->IsFlushed()) {
                auto* chunk = dynamicStore->GetFlushedChunk();
                if (chunk) {
                    auto rowIndex = dynamicStore->GetType() == EObjectType::OrderedDynamicTabletStore
                        ? std::make_optional(dynamicStore->GetTableRowIndex())
                        : std::nullopt;
                    auto* spec = subresponse->mutable_chunk_spec();
                    BuildChunkSpec(
                        chunk,
                        rowIndex,
                        /*tabletIndex*/ {},
                        /*lowerLimit*/ {},
                        /*upperLimit*/ {},
                        /*timestampTransactionId*/ {},
                        /*fetchParityReplicas*/ true,
                        request->fetch_all_meta_extensions(),
                        fetchChunkMetaFromSequoia,
                        extensionTags,
                        &nodeDirectoryBuilder,
                        Bootstrap_,
                        spec);

                    if (dynamicStore->GetType() == EObjectType::OrderedDynamicTabletStore) {
                        spec->set_row_index_is_absolute(true);
                    }

                    if (ShouldFetchChunkMetaFromSequoia(chunk, fetchChunkMetaFromSequoia)) {
                        metaFetchFutures.push_back(FetchChunkMetasFromSequoia(
                            request->fetch_all_meta_extensions(),
                            extensionTags,
                            {spec},
                            Bootstrap_));
                    }
                }
            } else {
                const auto& tabletManager = Bootstrap_->GetTabletManager();
                auto* chunkSpec = subresponse->mutable_chunk_spec();
                auto* tablet = dynamicStore->GetTablet();

                ToProto(chunkSpec->mutable_chunk_id(), dynamicStore->GetId());
                if (auto* node = tabletManager->FindTabletLeaderNode(tablet)) {
                    auto replica = TNodePtrWithIndexes(node, GenericChunkReplicaIndex, GenericMediumIndex);
                    nodeDirectoryBuilder.Add(replica);
                    chunkSpec->add_replicas(ToProto<ui64>(replica));
                }
                ToProto(chunkSpec->mutable_tablet_id(), tablet->GetId());
            }
        }

        if (!metaFetchFutures.empty()) {
            WaitFor(AllSucceeded(std::move(metaFetchFutures)))
                .ThrowOnError();
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, TouchChunks)
    {
        context->SetRequestInfo("SubrequestCount: %v",
            request->subrequests_size());

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::Leader);

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        for (const auto& protoChunkId : request->subrequests()) {
            auto chunkId = FromProto<TChunkId>(protoChunkId);
            auto chunkIdWithIndex = DecodeChunkId(chunkId);
            auto* chunk = chunkManager->FindChunk(chunkIdWithIndex.Id);
            if (IsObjectAlive(chunk)) {
                chunkManager->TouchChunk(chunk);
            }
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, AllocateWriteTargets)
    {
        context->SetRequestInfo("SubrequestCount: %v",
            request->subrequests_size());

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::Leader);

        TNodeDirectoryBuilder builder(response->mutable_node_directory());

        for (const auto& subrequest : request->subrequests()) {
            auto sessionId = FromProto<TSessionId>(subrequest.session_id());
            int desiredTargetCount = subrequest.desired_target_count();
            int minTargetCount = subrequest.min_target_count();
            auto replicationFactorOverride = subrequest.has_replication_factor_override()
                ? std::make_optional(subrequest.replication_factor_override())
                : std::nullopt;
            auto preferredHostName = subrequest.has_preferred_host_name()
                ? std::make_optional(subrequest.preferred_host_name())
                : std::nullopt;
            auto forbiddenAddresses = FromProto<std::vector<TString>>(subrequest.forbidden_addresses());

            auto* subresponse = response->add_subresponses();
            try {
                const auto& chunkManager = Bootstrap_->GetChunkManager();
                auto* medium = chunkManager->GetMediumByIndexOrThrow(sessionId.MediumIndex);
                auto* chunk = chunkManager->GetChunkOrThrow(sessionId.ChunkId);

                const auto& nodeTracker = Bootstrap_->GetNodeTracker();
                TNodeList forbiddenNodes;
                for (const auto& address : forbiddenAddresses) {
                    auto* node = nodeTracker->FindNodeByAddress(address);
                    if (node) {
                        forbiddenNodes.push_back(node);
                    }
                }
                std::sort(forbiddenNodes.begin(), forbiddenNodes.end());

                auto targets = chunkManager->AllocateWriteTargets(
                    medium,
                    chunk,
                    desiredTargetCount,
                    minTargetCount,
                    replicationFactorOverride,
                    &forbiddenNodes,
                    preferredHostName);

                for (int index = 0; index < static_cast<int>(targets.size()); ++index) {
                    auto* target = targets[index];
                    auto replica = TNodePtrWithIndexes(target, GenericChunkReplicaIndex, sessionId.MediumIndex);
                    builder.Add(replica);
                    subresponse->add_replicas(ToProto<ui64>(replica));
                }

                YT_LOG_DEBUG("Write targets allocated "
                    "(SessionId: %v%v, DesiredTargetCount: %v, MinTargetCount: %v, ReplicationFactorOverride: %v, "
                    "PreferredHostName: %v, ForbiddenAddresses: %v, Targets: %v)",
                    sessionId,
                    MakeFormatterWrapper([&] (auto* builder) {
                        if (chunk->HasConsistentReplicaPlacementHash()) {
                            builder->AppendFormat(
                                ", ConsistentReplicaPlacementHash: %llx",
                                chunk->GetConsistentReplicaPlacementHash());
                        }
                    }),
                    desiredTargetCount,
                    minTargetCount,
                    replicationFactorOverride,
                    preferredHostName,
                    forbiddenAddresses,
                    MakeFormattableView(targets, TNodePtrAddressFormatter()));
            } catch (const std::exception& ex) {
                auto error = TError(ex);
                YT_LOG_DEBUG(error, "Error allocating write targets "
                    "(SessionId: %v, DesiredTargetCount: %v, MinTargetCount: %v, ReplicationFactorOverride: %v, "
                    "PreferredHostName: %v, ForbiddenAddresses: %v)",
                    sessionId,
                    desiredTargetCount,
                    minTargetCount,
                    replicationFactorOverride,
                    preferredHostName,
                    forbiddenAddresses);
                ToProto(subresponse->mutable_error(), error);
            }
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, ExportChunks)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        context->SetRequestInfo("TransactionId: %v, ChunkCount: %v",
            transactionId,
            request->chunks_size());

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::Leader);
        SyncWithTransactionCoordinatorCell(context, transactionId);

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto mutation = chunkManager->CreateExportChunksMutation(context);
        mutation->SetCurrentTraceContext();
        mutation->CommitAndReply(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, ImportChunks)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        context->SetRequestInfo("TransactionId: %v, ChunkCount: %v",
            transactionId,
            request->chunks_size());

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::Leader);
        SyncWithTransactionCoordinatorCell(context, transactionId);

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto mutation = chunkManager->CreateImportChunksMutation(context);
        mutation->SetCurrentTraceContext();
        mutation->CommitAndReply(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, GetChunkOwningNodes)
    {
        auto chunkId = FromProto<TChunkId>(request->chunk_id());

        context->SetRequestInfo("ChunkId: %v",
            chunkId);

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::LeaderOrFollower);
        SyncWithUpstream();

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* chunk = chunkManager->GetChunkOrThrow(chunkId);

        auto owningNodes = GetOwningNodes(chunk);
        for (const auto* node : owningNodes) {
            auto* protoNode = response->add_nodes();
            ToProto(protoNode->mutable_node_id(), node->GetId());
            if (auto* transaction = node->GetTransaction()) {
                auto transactionId = transaction->IsExternalized()
                    ? transaction->GetOriginalTransactionId()
                    : transaction->GetId();
                ToProto(protoNode->mutable_transaction_id(), transactionId);
            }
        }

        context->SetResponseInfo("NodeCount: %v",
            response->nodes_size());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, ExecuteBatch)
    {
        // COMPAT(shakurov): remove the former.
        auto suppressUpstreamSync =
            request->suppress_upstream_sync() ||
            GetSuppressUpstreamSync(context->RequestHeader());

        context->SetRequestInfo(
            "CreateChunkCount: %v, "
            "ConfirmChunkCount: %v, "
            "SealChunkCount: %v, "
            "CreateChunkListsCount: %v, "
            "UsageChunkListsCount: %v, "
            "AttachChunkTreesCount: %v, "
            "SuppressUpstreamSync: %v",
            request->create_chunk_subrequests_size(),
            request->confirm_chunk_subrequests_size(),
            request->seal_chunk_subrequests_size(),
            request->create_chunk_lists_subrequests_size(),
            request->unstage_chunk_tree_subrequests_size(),
            request->attach_chunk_trees_subrequests_size(),
            suppressUpstreamSync);

        ValidateClusterInitialized();
        ValidatePeer(EPeerKind::Leader);

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        // NB: supporting lazy transaction replication here is required for (at
        // least) the following reason. When starting operations, controller
        // agents first start all the necessary transactions, only then getting
        // basic attributes for output & debug tables. Thus, at the moment of
        // starting a transaction the set of cells it'll be needed to be
        // replicated to is yet unknown.

        std::vector<TTransactionId> transactionIds;
        for (const auto& createChunkSubrequest : request->create_chunk_subrequests()) {
            transactionIds.push_back(FromProto<TTransactionId>(createChunkSubrequest.transaction_id()));
        }
        for (const auto& createChunkListsSubrequest : request->create_chunk_lists_subrequests()) {
            transactionIds.push_back(FromProto<TTransactionId>(createChunkListsSubrequest.transaction_id()));
        }
        SortUnique(transactionIds);

        const auto& configManager = Bootstrap_->GetConfigManager();
        const auto& config = configManager->GetConfig()->ChunkService;
        // TODO(shakurov): use mutation idempotizer when handling these
        // mutations and comply with config->EnableMutationBoomerangs.
        const auto enableMutationBoomerangs = false;

        // COMPAT(kvk1920)
        if (config->EnableAlertOnChunkConfirmationWithoutLocationUuid) {
            for (const auto& subrequest : request->confirm_chunk_subrequests()) {
                YT_LOG_ALERT_UNLESS(subrequest.location_uuids_supported(), "Chunk confirmation request without location uuids is received");
            }
        }

        if (!configManager->GetConfig()->SequoiaManager->Enable) {
            auto preparationFuture = NTransactionServer::RunTransactionReplicationSession(
                !suppressUpstreamSync,
                Bootstrap_,
                std::move(transactionIds),
                context,
                chunkManager->CreateExecuteBatchMutation(context),
                enableMutationBoomerangs);
            YT_VERIFY(preparationFuture);
        } else {
            // TODO(aleksandra-zh): YT-16872, Respect the Response Keeper!
            auto preparationFuture = NTransactionServer::RunTransactionReplicationSession(
                !suppressUpstreamSync,
                Bootstrap_,
                std::move(transactionIds),
                context->GetRequestId());

            preparationFuture.Apply(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
                if (error.IsOK()) {
                    auto preparedRequest = chunkManager->PrepareExecuteBatchRequest(context->Request());
                    auto mutation = chunkManager->CreateExecuteBatchMutation(
                        &preparedRequest->MutationRequest,
                        &preparedRequest->MutationResponse);

                    std::vector<TFuture<void>> futures({
                        mutation->Commit().AsVoid(),
                        chunkManager->ExecuteBatchSequoia(preparedRequest),
                    });
                    return AllSucceeded(std::move(futures))
                        .Apply(BIND([=, this_ = MakeStrong(this)] () mutable {
                            chunkManager->PrepareExecuteBatchResponse(preparedRequest, &context->Response());
                            context->Reply();
                        }).AsyncVia(NRpc::TDispatcher::Get()->GetHeavyInvoker()));
                } else {
                    context->Reply(error);
                    return VoidFuture;
                }
            }).AsyncVia(GetGuardedAutomatonInvoker(EAutomatonThreadQueue::ChunkService)));
        }
    }

    void SyncWithTransactionCoordinatorCell(const IServiceContextPtr& context, TTransactionId transactionId)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        const auto& hiveManager = Bootstrap_->GetHiveManager();

        auto cellTag = CellTagFromId(transactionId);
        auto cellId = multicellManager->GetCellId(cellTag);
        auto syncFuture = hiveManager->SyncWith(cellId, true);

        YT_LOG_DEBUG("Request will synchronize with another cell (RequestId: %v, CellTag: %v)",
            context->GetRequestId(),
            cellTag);

        WaitFor(syncFuture)
            .ThrowOnError();
    }
};

IServicePtr CreateChunkService(TBootstrap* boostrap)
{
    return New<TChunkService>(boostrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
