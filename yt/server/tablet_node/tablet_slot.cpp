#include "tablet_slot.h"
#include "config.h"
#include "tablet_slot_manager.h"
#include "serialize.h"
#include "automaton.h"
#include "tablet_manager.h"
#include "transaction_manager.h"
#include "tablet_service.h"
#include "private.h"

#include <core/concurrency/thread_affinity.h>
#include <core/concurrency/scheduler.h>
#include <core/concurrency/action_queue.h>

#include <core/ytree/fluent.h>

#include <core/rpc/server.h>

#include <core/logging/log.h>

#include <ytlib/election/config.h>
#include <ytlib/election/cell_manager.h>

#include <ytlib/hive/cell_directory.h>

#include <ytlib/transaction_client/timestamp_provider.h>

#include <ytlib/tablet_client/config.h>

#include <ytlib/api/connection.h>
#include <ytlib/api/client.h>

#include <server/election/election_manager.h>

#include <server/hydra/changelog.h>
#include <server/hydra/remote_changelog_store.h>
#include <server/hydra/snapshot.h>
#include <server/hydra/remote_snapshot_store.h>
#include <server/hydra/hydra_manager.h>
#include <server/hydra/distributed_hydra_manager.h>

#include <server/hive/hive_manager.h>
#include <server/hive/mailbox.h>
#include <server/hive/transaction_supervisor.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

#include <server/data_node/config.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NElection;
using namespace NHydra;
using namespace NHive;
using namespace NNodeTrackerClient::NProto;
using namespace NObjectClient;

using NHydra::EPeerState;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

class TTabletSlot::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TTabletSlot* owner,
        int slotIndex,
        TTabletNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Owner_(owner)
        , SlotIndex_(slotIndex)
        , Config_(config)
        , Bootstrap_(bootstrap)
        , State_(EPeerState::None)
        , PeerId_(InvalidPeerId)
        , AutomatonQueue_(New<TFairShareActionQueue>(
            Format("TabletSlot:%v", SlotIndex_),
            EAutomatonThreadQueue::GetDomainNames()))
        , Logger(TabletNodeLogger)
    {
        VERIFY_INVOKER_AFFINITY(GetAutomatonInvoker(EAutomatonThreadQueue::Write), AutomatonThread);

        SetCellGuid(NullCellGuid);
    }


    int GetIndex() const
    {
        return SlotIndex_;
    }

    const TCellGuid& GetCellGuid() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return CellGuid_;
    }

    EPeerState GetControlState() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (HydraManager_) {
            State_ = HydraManager_->GetControlState();
        }

        return State_;
    }

    EPeerState GetAutomatonState() const
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return HydraManager_ ? HydraManager_->GetAutomatonState() : EPeerState(EPeerState::None);
    }

    TPeerId GetPeerId() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return PeerId_;
    }

    int GetCellConfigVersion() const
    {
        return CellConfigVersion_;
    }

    TTabletCellConfigPtr GetCellConfig() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return CellConfig_;
    }

    IHydraManagerPtr GetHydraManager() const
    {
        return HydraManager_;
    }

    TTabletAutomatonPtr GetAutomaton() const
    {
        return Automaton_;
    }

    IInvokerPtr GetAutomatonInvoker(EAutomatonThreadQueue queue) const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return AutomatonQueue_->GetInvoker(queue);
    }

    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue) const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(InvokersSpinLock_);
        return EpochAutomatonInvokers_.empty() ? nullptr : EpochAutomatonInvokers_[queue];
    }

    IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue) const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(InvokersSpinLock_);
        return GuardedAutomatonInvokers_.empty() ? nullptr : GuardedAutomatonInvokers_[queue];
    }

    THiveManagerPtr GetHiveManager() const
    {
        return HiveManager_;
    }

    TMailbox* GetMasterMailbox()
    {
        // Create master mailbox lazily.
        auto masterCellGuid = Bootstrap_->GetCellGuid();
        return HiveManager_->GetOrCreateMailbox(masterCellGuid);
    }

    TTransactionManagerPtr GetTransactionManager() const
    {
        return TransactionManager_;
    }

    TTransactionSupervisorPtr GetTransactionSupervisor() const
    {
        return TransactionSupervisor_;
    }

    TTabletManagerPtr GetTabletManager() const
    {
        return TabletManager_;
    }

    TObjectId GenerateId(EObjectType type)
    {
        auto* mutationContext = HydraManager_->GetMutationContext();

        const auto& version = mutationContext->GetVersion();

        auto random = mutationContext->RandomGenerator().Generate<ui64>();

        int typeValue = static_cast<int>(type);
        YASSERT(typeValue >= 0 && typeValue <= MaxObjectType);

        return TObjectId(
            random ^ CellGuid_.Parts[0],
            (CellGuid_.Parts[1] & 0xffff0000) + typeValue,
            version.RecordId,
            version.SegmentId);
    }


    void Create(const TCreateTabletSlotInfo& createInfo)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(State_ == EPeerState::None);

        auto cellGuid = FromProto<TCellGuid>(createInfo.cell_guid());
        SetCellGuid(cellGuid);

        Options_ = ConvertTo<TTabletCellOptionsPtr>(TYsonString(createInfo.options()));

        State_ = EPeerState::Stopped;

        LOG_INFO("Slot created");
    }

    void Configure(const TConfigureTabletSlotInfo& configureInfo)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(State_ != EPeerState::None);

        CellConfigVersion_ = configureInfo.config_version();
        CellConfig_ = ConvertTo<TTabletCellConfigPtr>(TYsonString(configureInfo.config()));

        if (HydraManager_) {
            CellManager_->Reconfigure(CellConfig_->ToElection(CellGuid_));
        } else {
            PeerId_ = configureInfo.peer_id();
            State_ = EPeerState::Elections;

            CellManager_ = New<TCellManager>(
                CellConfig_->ToElection(CellGuid_),
                Bootstrap_->GetTabletChannelFactory(),
                configureInfo.peer_id());

            Automaton_ = New<TTabletAutomaton>(Bootstrap_, Owner_);

            auto rpcServer = Bootstrap_->GetRpcServer();

            auto snapshotStore = CreateRemoteSnapshotStore(
                Config_->Snapshots,
                Options_,
                Format("//sys/tablet_cells/%v/snapshots", CellGuid_),
                Bootstrap_->GetMasterClient());

            auto changelogStore = CreateRemoteChangelogStore(
                Config_->Changelogs,
                Options_,
                Format("//sys/tablet_cells/%v/changelogs", CellGuid_),
                Bootstrap_->GetMasterClient());

            HydraManager_ = CreateDistributedHydraManager(
                Config_->HydraManager,
                Bootstrap_->GetControlInvoker(),
                GetAutomatonInvoker(EAutomatonThreadQueue::Write),
                Automaton_,
                rpcServer,
                CellManager_,
                changelogStore,
                snapshotStore);

            HydraManager_->SubscribeStartLeading(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
            HydraManager_->SubscribeStartFollowing(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
            
            HydraManager_->SubscribeStopLeading(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));
            HydraManager_->SubscribeStopFollowing(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));

            {
                TGuard<TSpinLock> guard(InvokersSpinLock_);
                GuardedAutomatonInvokers_.resize(EAutomatonThreadQueue::GetDomainSize());
                for (auto queue : EAutomatonThreadQueue::GetDomainValues()) {
                    GuardedAutomatonInvokers_[queue] = HydraManager_->CreateGuardedAutomatonInvoker(
                        GetAutomatonInvoker(queue));
                }
            }

            HiveManager_ = New<THiveManager>(
                CellGuid_,
                Config_->HiveManager,
                Bootstrap_->GetMasterClient()->GetConnection()->GetCellDirectory(),
                GetAutomatonInvoker(EAutomatonThreadQueue::Write),
                HydraManager_,
                Automaton_);

            // NB: Tablet Manager must register before Transaction Manager since the latter
            // will be writing and deleting rows during snapshot loading.
            TabletManager_ = New<TTabletManager>(
                Config_->TabletManager,
                Owner_,
                Bootstrap_);

            TransactionManager_ = New<TTransactionManager>(
                Config_->TransactionManager,
                Owner_,
                Bootstrap_);

            TransactionSupervisor_ = New<TTransactionSupervisor>(
                Config_->TransactionSupervisor,
                GetAutomatonInvoker(EAutomatonThreadQueue::Write),
                HydraManager_,
                Automaton_,
                HiveManager_,
                TransactionManager_,
                Bootstrap_->GetMasterClient()->GetConnection()->GetTimestampProvider());

            TabletService_ = CreateTabletService(
                Owner_,
                Bootstrap_);

            TabletManager_->Initialize();
            HydraManager_->Start();

            rpcServer->RegisterService(TransactionSupervisor_->GetRpcService());
            rpcServer->RegisterService(HiveManager_->GetRpcService());
            rpcServer->RegisterService(TabletService_);
        }

        LOG_INFO("Slot configured");
    }

    void Remove()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(State_ != EPeerState::None);

        auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
        tabletSlotManager->UnregisterTablets(Owner_);

        if (HydraManager_) {
            HydraManager_->Stop();
        }

        auto rpcServer = Bootstrap_->GetRpcServer();
        if (TransactionSupervisor_) {
            rpcServer->UnregisterService(TransactionSupervisor_->GetRpcService());
        }
        if (HiveManager_) {
            rpcServer->UnregisterService(HiveManager_->GetRpcService());
        }
        if (TabletService_) {
            rpcServer->UnregisterService(TabletService_);
        }

        {
            TGuard<TSpinLock> guard(InvokersSpinLock_);
            EpochAutomatonInvokers_.clear();
            GuardedAutomatonInvokers_.clear();
        }

        State_ = EPeerState::None;

        LOG_INFO("Slot removed");
    }


    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        BuildYsonFluently(consumer)
            .BeginMap()
                .Do(BIND(&TImpl::BuildOrchidYsonControl, Unretained(this)))
                .Do(BIND(&TImpl::BuildOrchidYsonAutomaton, Unretained(this)))
            .EndMap();
    }

private:
    TTabletSlot* Owner_;
    int SlotIndex_;
    TTabletNodeConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;

    TCellGuid CellGuid_;
    mutable EPeerState State_;
    TPeerId PeerId_;
    int CellConfigVersion_ = 0;
    TTabletCellConfigPtr CellConfig_;
    TTabletCellOptionsPtr Options_;

    TCellManagerPtr CellManager_;
    IHydraManagerPtr HydraManager_;
    
    THiveManagerPtr HiveManager_;

    TTabletManagerPtr TabletManager_;

    TTransactionManagerPtr TransactionManager_;
    TTransactionSupervisorPtr TransactionSupervisor_;

    NRpc::IServicePtr TabletService_;

    TTabletAutomatonPtr Automaton_;
    TFairShareActionQueuePtr AutomatonQueue_;

    TSpinLock InvokersSpinLock_;
    std::vector<IInvokerPtr> EpochAutomatonInvokers_;
    std::vector<IInvokerPtr> GuardedAutomatonInvokers_;

    NLog::TLogger Logger;


    void SetCellGuid(const TCellGuid& cellGuid)
    {
        CellGuid_ = cellGuid;
        InitLogger();
    }

    void InitLogger()
    {
        Logger = NLog::TLogger(TabletNodeLogger);
        Logger.AddTag("Slot: %v", SlotIndex_);
        if (CellGuid_ != NullCellGuid) {
            Logger.AddTag("CellGuid: %v", CellGuid_);
        }
    }


    void OnStartEpoch()
    {
        TGuard<TSpinLock> guard(InvokersSpinLock_);
        EpochAutomatonInvokers_.resize(EAutomatonThreadQueue::GetDomainSize());
        for (auto queue : EAutomatonThreadQueue::GetDomainValues()) {
            EpochAutomatonInvokers_[queue] = HydraManager_
                ->GetAutomatonEpochContext()
                ->CancelableContext
                ->CreateInvoker(GetAutomatonInvoker(queue));
        }
    }

    void OnStopEpoch()
    {
        TGuard<TSpinLock> guard(InvokersSpinLock_);
        EpochAutomatonInvokers_.clear();
    }


    void BuildOrchidYsonControl(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        BuildYsonMapFluently(consumer)
            .Item("state").Value(GetControlState())
            .DoIf(GetControlState() != EPeerState::None, [&] (TFluentMap fluent) {
                fluent
                    .Item("cell_guid").Value(CellGuid_)
                    .Item("options").Value(*Options_);
            });
    }

    void BuildOrchidYsonAutomaton(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!HydraManager_)
            return;
        
        auto epochContext = HydraManager_->GetEpochContext();
        if (!epochContext)
            return;

        auto cancelableContext = epochContext->CancelableContext;
        auto done = BIND(&TImpl::DoBuildOrchidYsonAutomaton, MakeStrong(this))
            .AsyncVia(GetGuardedAutomatonInvoker(EAutomatonThreadQueue::Read))
            .Run(cancelableContext, consumer)
            .Finally();
        WaitFor(done);
    }

    void DoBuildOrchidYsonAutomaton(TCancelableContextPtr context, IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // Make sure we're still using the same context.
        // Otherwise cell guid, which has alread been printed, might be wrong.
        if (context->IsCanceled())
            return;

        BuildYsonMapFluently(consumer)
            .Item("transactions").Do(BIND(&TTransactionManager::BuildOrchidYson, TransactionManager_))
            .Item("tablets").Do(BIND(&TTabletManager::BuildOrchidYson, TabletManager_))
            .Item("hive").Do(BIND(&THiveManager::BuildOrchidYson, HiveManager_));
    }


    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

};

////////////////////////////////////////////////////////////////////////////////

TTabletSlot::TTabletSlot(
    int slotIndex,
    TTabletNodeConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        this,
        slotIndex,
        config,
        bootstrap))
{ }

TTabletSlot::~TTabletSlot()
{ }

int TTabletSlot::GetIndex() const
{
    return Impl_->GetIndex();
}

const TCellGuid& TTabletSlot::GetCellGuid() const
{
    return Impl_->GetCellGuid();
}

EPeerState TTabletSlot::GetControlState() const
{
    return Impl_->GetControlState();
}

EPeerState TTabletSlot::GetAutomatonState() const
{
    return Impl_->GetAutomatonState();
}

TPeerId TTabletSlot::GetPeerId() const
{
    return Impl_->GetPeerId();
}

int TTabletSlot::GetCellConfigVersion() const
{
    return Impl_->GetCellConfigVersion();
}

TTabletCellConfigPtr TTabletSlot::GetCellConfig() const
{
    return Impl_->GetCellConfig();
}

IHydraManagerPtr TTabletSlot::GetHydraManager() const
{
    return Impl_->GetHydraManager();
}

TTabletAutomatonPtr TTabletSlot::GetAutomaton() const
{
    return Impl_->GetAutomaton();
}

IInvokerPtr TTabletSlot::GetAutomatonInvoker(EAutomatonThreadQueue queue) const
{
    return Impl_->GetAutomatonInvoker(queue);
}

IInvokerPtr TTabletSlot::GetEpochAutomatonInvoker(EAutomatonThreadQueue queue) const
{
    return Impl_->GetEpochAutomatonInvoker(queue);
}

IInvokerPtr TTabletSlot::GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue) const
{
    return Impl_->GetGuardedAutomatonInvoker(queue);
}

THiveManagerPtr TTabletSlot::GetHiveManager() const
{
    return Impl_->GetHiveManager();
}

TMailbox* TTabletSlot::GetMasterMailbox()
{
    return Impl_->GetMasterMailbox();
}

TTransactionManagerPtr TTabletSlot::GetTransactionManager() const
{
    return Impl_->GetTransactionManager();
}

TTransactionSupervisorPtr TTabletSlot::GetTransactionSupervisor() const
{
    return Impl_->GetTransactionSupervisor();
}

TTabletManagerPtr TTabletSlot::GetTabletManager() const
{
    return Impl_->GetTabletManager();
}

TObjectId TTabletSlot::GenerateId(EObjectType type)
{
    return Impl_->GenerateId(type);
}

void TTabletSlot::Create(const TCreateTabletSlotInfo& createInfo)
{
    Impl_->Create(createInfo);
}

void TTabletSlot::Configure(const TConfigureTabletSlotInfo& configureInfo)
{
    Impl_->Configure(configureInfo);
}

void TTabletSlot::Remove()
{
    Impl_->Remove();
}

void TTabletSlot::BuildOrchidYson(IYsonConsumer* consumer)
{
    return Impl_->BuildOrchidYson(consumer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
} // namespace NTabletNode
