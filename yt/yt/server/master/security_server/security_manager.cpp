#include "security_manager.h"
#include "private.h"
#include "account.h"
#include "account_proxy.h"
#include "account_resource_usage_lease.h"
#include "account_resource_usage_lease_proxy.h"
#include "acl.h"
#include "config.h"
#include "detailed_master_memory.h"
#include "group.h"
#include "group_proxy.h"
#include "network_project.h"
#include "network_project_proxy.h"
#include "proxy_role.h"
#include "proxy_role_proxy.h"
#include "request_tracker.h"
#include "user.h"
#include "user_proxy.h"
#include "security_tags.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/helpers.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>
#include <yt/yt/server/master/cell_master/multicell_manager.h>
#include <yt/yt/server/master/cell_master/serialize.h>
#include <yt/yt/server/master/cell_master/config.h>

#include <yt/yt/server/master/chunk_server/chunk_manager.h>
#include <yt/yt/server/master/chunk_server/chunk_requisition.h>
#include <yt/yt/server/master/chunk_server/medium.h>

#include <yt/yt/server/master/cypress_server/node.h>
#include <yt/yt/server/master/cypress_server/cypress_manager.h>

#include <yt/yt/server/master/tablet_server/tablet.h>

#include <yt/yt/server/lib/hydra_common/composite_automaton.h>
#include <yt/yt/server/lib/hydra_common/entity_map.h>

#include <yt/yt/server/master/object_server/map_object_type_handler.h>
#include <yt/yt/server/master/object_server/map_object.h>
#include <yt/yt/server/master/object_server/type_handler_detail.h>

#include <yt/yt/server/master/table_server/master_table_schema.h>
#include <yt/yt/server/master/table_server/table_node.h>

#include <yt/yt/server/master/transaction_server/transaction.h>
//#include <yt/yt/server/master/transaction_server/boomerang_tracker.h>

#include <yt/yt/server/master/object_server/object_manager.h>

//#include <yt/yt/server/lib/hive/hive_manager.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/ytlib/security_client/group_ypath_proxy.h>

#include <yt/yt/client/security_client/helpers.h>

#include <yt/yt/core/concurrency/fls.h>

#include <yt/yt/library/erasure/impl/codec.h>

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/intern_registry.h>

#include <yt/yt/core/logging/fluent_log.h>

#include <yt/yt/core/ypath/token.h>

namespace NYT::NSecurityServer {

using namespace NChunkServer;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NHydra;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NYson;
using namespace NYTree;
using namespace NYPath;
using namespace NCypressServer;
using namespace NSecurityClient;
using namespace NTableServer;
using namespace NObjectServer;
using namespace NHiveServer;
using namespace NProfiling;
using namespace NLogging;
using namespace NYTProf;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SecurityServerLogger;

namespace {

bool IsPermissionValidationSuppressed()
{
    return IsSubordinateMutation();
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TAuthenticatedUserGuard::TAuthenticatedUserGuard(
    TSecurityManagerPtr securityManager,
    TUser* user,
    const TString& userTag)
{
    if (!user) {
        return;
    }

    User_ = user;
    securityManager->SetAuthenticatedUser(user);
    SecurityManager_ = std::move(securityManager);

    AuthenticationIdentity_ = NRpc::TAuthenticationIdentity(
        user->GetName(),
        userTag ? userTag : user->GetName());
    AuthenticationIdentityGuard_ = NRpc::TCurrentAuthenticationIdentityGuard(&AuthenticationIdentity_);
    CpuProfilerTagGuard_ = TCpuProfilerTagGuard(SecurityManager_->GetUserCpuProfilerTag(User_));
}

TAuthenticatedUserGuard::TAuthenticatedUserGuard(
    TSecurityManagerPtr securityManager,
    NRpc::TAuthenticationIdentity identity)
{
    User_ = securityManager->GetUserByNameOrThrow(identity.User, true /*activeLifeStageOnly*/);
    securityManager->SetAuthenticatedUser(User_);
    SecurityManager_ = std::move(securityManager);

    AuthenticationIdentity_ = std::move(identity);
    AuthenticationIdentityGuard_ = NRpc::TCurrentAuthenticationIdentityGuard(&AuthenticationIdentity_);
    CpuProfilerTagGuard_ = TCpuProfilerTagGuard(SecurityManager_->GetUserCpuProfilerTag(User_));
}

TAuthenticatedUserGuard::TAuthenticatedUserGuard(
    TSecurityManagerPtr securityManager)
    : TAuthenticatedUserGuard(
        std::move(securityManager),
        NRpc::GetCurrentAuthenticationIdentity())
{ }

TAuthenticatedUserGuard::~TAuthenticatedUserGuard()
{
    if (SecurityManager_) {
        SecurityManager_->ResetAuthenticatedUser();
    }
}

TUser* TAuthenticatedUserGuard::GetUser() const
{
    return User_;
}

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TAccountTypeHandler
    : public TNonversionedMapObjectTypeHandlerBase<TAccount>
{
public:
    explicit TAccountTypeHandler(TImpl* owner);

    ETypeFlags GetFlags() const override
    {
        return
            TBase::GetFlags() |
            ETypeFlags::ReplicateCreate |
            ETypeFlags::ReplicateDestroy |
            ETypeFlags::ReplicateAttributes |
            ETypeFlags::TwoPhaseCreation |
            ETypeFlags::TwoPhaseRemoval;
    }

    EObjectType GetType() const override
    {
        return EObjectType::Account;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override;

    std::optional<TObject*> FindObjectByAttributes(
        const NYTree::IAttributeDictionary* attributes) override;

    void RegisterName(const TString& name, TAccount* account) noexcept override;
    void UnregisterName(const TString& name, TAccount* account) noexcept override;

    TString GetRootPath(const TAccount* rootAccount) const override;

protected:
    TCellTagList DoGetReplicationCellTags(const TAccount* /*account*/) override
    {
        return TNonversionedMapObjectTypeHandlerBase<TAccount>::AllSecondaryCellTags();
    }

private:
    using TBase = TNonversionedMapObjectTypeHandlerBase<TAccount>;

    TImpl* const Owner_;

    std::optional<int> GetDepthLimit() const override
    {
        return AccountTreeDepthLimit;
    }

    std::optional<int> GetSubtreeSizeLimit() const override;

    TProxyPtr GetMapObjectProxy(TAccount* account) override;

    void DoZombifyObject(TAccount* account) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TAccountResourceUsageLeaseTypeHandler
    : public TObjectTypeHandlerWithMapBase<TAccountResourceUsageLease>
{
public:
    explicit TAccountResourceUsageLeaseTypeHandler(TImpl* owner);

    ETypeFlags GetFlags() const override
    {
        return
            ETypeFlags::Creatable |
            ETypeFlags::Removable;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override;

    EObjectType GetType() const override
    {
        return EObjectType::AccountResourceUsageLease;
    }

private:
    using TBase = TObjectTypeHandlerWithMapBase<TAccountResourceUsageLease>;

    TImpl* const Owner_;

    IObjectProxyPtr DoGetProxy(
        TAccountResourceUsageLease* accountResourceUsageLease,
        TTransaction* /*transaction*/) override
    {
        return CreateAccountResourceUsageLeaseProxy(Bootstrap_, &Metadata_, accountResourceUsageLease);
    }

    void DoZombifyObject(TAccountResourceUsageLease* accountResourceUsageLease) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TUserTypeHandler
    : public TObjectTypeHandlerWithMapBase<TUser>
{
public:
    explicit TUserTypeHandler(TImpl* owner);

    ETypeFlags GetFlags() const override
    {
        return
            ETypeFlags::ReplicateCreate |
            ETypeFlags::ReplicateDestroy |
            ETypeFlags::ReplicateAttributes |
            ETypeFlags::Creatable |
            ETypeFlags::Removable |
            ETypeFlags::TwoPhaseCreation |
            ETypeFlags::TwoPhaseRemoval;
    }

    TCellTagList DoGetReplicationCellTags(const TUser* /*object*/) override
    {
        return AllSecondaryCellTags();
    }

    EObjectType GetType() const override
    {
        return EObjectType::User;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override;

    std::optional<TObject*> FindObjectByAttributes(
        const NYTree::IAttributeDictionary* attributes) override;

private:
    TImpl* const Owner_;

    TAccessControlDescriptor* DoFindAcd(TUser* user) override
    {
        return &user->Acd();
    }

    IObjectProxyPtr DoGetProxy(TUser* user, TTransaction* transaction) override;
    void DoZombifyObject(TUser* user) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TGroupTypeHandler
    : public TObjectTypeHandlerWithMapBase<TGroup>
{
public:
    explicit TGroupTypeHandler(TImpl* owner);

    ETypeFlags GetFlags() const override
    {
        return
            ETypeFlags::ReplicateCreate |
            ETypeFlags::ReplicateDestroy |
            ETypeFlags::ReplicateAttributes |
            ETypeFlags::Creatable |
            ETypeFlags::Removable;
    }

    EObjectType GetType() const override
    {
        return EObjectType::Group;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override;

    std::optional<TObject*> FindObjectByAttributes(
        const NYTree::IAttributeDictionary* attributes) override;

private:
    TImpl* const Owner_;

    TCellTagList DoGetReplicationCellTags(const TGroup* /*group*/) override
    {
        return AllSecondaryCellTags();
    }

    TAccessControlDescriptor* DoFindAcd(TGroup* group) override
    {
        return &group->Acd();
    }

    IObjectProxyPtr DoGetProxy(TGroup* group, TTransaction* transaction) override;
    void DoZombifyObject(TGroup* group) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TNetworkProjectTypeHandler
    : public TObjectTypeHandlerWithMapBase<TNetworkProject>
{
public:
    explicit TNetworkProjectTypeHandler(TImpl* owner);

    ETypeFlags GetFlags() const override
    {
        return
            ETypeFlags::Creatable |
            ETypeFlags::Removable;
    }

    EObjectType GetType() const override
    {
        return EObjectType::NetworkProject;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override;

    TAccessControlDescriptor* DoFindAcd(TNetworkProject* networkProject) override
    {
        return &networkProject->Acd();
    }

private:
    TImpl* const Owner_;

    IObjectProxyPtr DoGetProxy(TNetworkProject* networkProject, TTransaction* transaction) override;
    void DoZombifyObject(TNetworkProject* networkProject) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TProxyRoleTypeHandler
    : public TObjectTypeHandlerWithMapBase<TProxyRole>
{
public:
    explicit TProxyRoleTypeHandler(TImpl* owner);

    ETypeFlags GetFlags() const override
    {
        return
            ETypeFlags::Creatable |
            ETypeFlags::Removable;
    }

    EObjectType GetType() const override
    {
        return EObjectType::ProxyRole;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override;

    TAccessControlDescriptor* DoFindAcd(TProxyRole* proxyRole) override
    {
        return &proxyRole->Acd();
    }

private:
    TImpl* const Owner_;

    IObjectProxyPtr DoGetProxy(TProxyRole* proxyRoles, TTransaction* transaction) override;
    void DoZombifyObject(TProxyRole* proxyRole) override;
};

////////////////////////////////////////////////////////////////////////////////

class TSecurityManager::TImpl
    : public TMasterAutomatonPart
{
public:
    TImpl(
        const TSecurityManagerConfigPtr& config,
        NCellMaster::TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap, NCellMaster::EAutomatonThreadQueue::SecurityManager)
        , RequestTracker_(New<TRequestTracker>(config->UserThrottler, bootstrap))
    {
        RegisterLoader(
            "SecurityManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "SecurityManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "SecurityManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "SecurityManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));

        auto cellTag = Bootstrap_->GetMulticellManager()->GetPrimaryCellTag();

        RootAccountId_ = MakeWellKnownId(EObjectType::Account, cellTag, 0xfffffffffffffffb);
        SysAccountId_ = MakeWellKnownId(EObjectType::Account, cellTag, 0xffffffffffffffff);
        TmpAccountId_ = MakeWellKnownId(EObjectType::Account, cellTag, 0xfffffffffffffffe);
        IntermediateAccountId_ = MakeWellKnownId(EObjectType::Account, cellTag, 0xfffffffffffffffd);
        ChunkWiseAccountingMigrationAccountId_ = MakeWellKnownId(EObjectType::Account, cellTag, 0xfffffffffffffffc);

        RootUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffff);
        GuestUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xfffffffffffffffe);
        JobUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xfffffffffffffffd);
        SchedulerUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xfffffffffffffffc);
        ReplicatorUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xfffffffffffffffb);
        OwnerUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xfffffffffffffffa);
        FileCacheUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffef);
        OperationsCleanerUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffee);
        OperationsClientUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffed);
        TabletCellChangeloggerUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffec);
        TabletCellSnapshotterUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffeb);
        TableMountInformerUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffea);
        AlienCellSynchronizerUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffe9);
        QueueAgentUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffe8);
        TabletBalancerUserId_ = MakeWellKnownId(EObjectType::User, cellTag, 0xffffffffffffffe7);

        EveryoneGroupId_ = MakeWellKnownId(EObjectType::Group, cellTag, 0xffffffffffffffff);
        UsersGroupId_ = MakeWellKnownId(EObjectType::Group, cellTag, 0xfffffffffffffffe);
        SuperusersGroupId_ = MakeWellKnownId(EObjectType::Group, cellTag, 0xfffffffffffffffd);

        RegisterMethod(BIND(&TImpl::HydraSetAccountStatistics, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRecomputeMembershipClosure, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateAccountMasterMemoryUsage, Unretained(this)));
    }

    void Initialize()
    {
        const auto& configManager = Bootstrap_->GetConfigManager();
        configManager->SubscribeConfigChanged(BIND(&TImpl::OnDynamicConfigChanged, MakeWeak(this)));

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->SubscribeTransactionCommitted(BIND(
            &TImpl::OnTransactionFinished,
            MakeStrong(this)));
        transactionManager->SubscribeTransactionAborted(BIND(
            &TImpl::OnTransactionFinished,
            MakeStrong(this)));

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(New<TAccountTypeHandler>(this));
        objectManager->RegisterHandler(New<TAccountResourceUsageLeaseTypeHandler>(this));
        objectManager->RegisterHandler(New<TUserTypeHandler>(this));
        objectManager->RegisterHandler(New<TGroupTypeHandler>(this));
        objectManager->RegisterHandler(New<TNetworkProjectTypeHandler>(this));
        objectManager->RegisterHandler(New<TProxyRoleTypeHandler>(this));

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            multicellManager->SubscribeReplicateKeysToSecondaryMaster(
                BIND(&TImpl::OnReplicateKeysToSecondaryMaster, MakeWeak(this)));
            multicellManager->SubscribeReplicateValuesToSecondaryMaster(
                BIND(&TImpl::OnReplicateValuesToSecondaryMaster, MakeWeak(this)));
        }
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Account, TAccount);
    DECLARE_ENTITY_MAP_ACCESSORS(AccountResourceUsageLease, TAccountResourceUsageLease);
    DECLARE_ENTITY_MAP_ACCESSORS(User, TUser);
    DECLARE_ENTITY_MAP_ACCESSORS(Group, TGroup);
    DECLARE_ENTITY_MAP_ACCESSORS(NetworkProject, TNetworkProject);


    TAccount* CreateAccount(TObjectId hintId = NullObjectId)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::Account, hintId);
        return DoCreateAccount(id);
    }

    void DestroyAccount(TAccount* /*account*/)
    { }

    TAccount* GetAccountOrThrow(TAccountId id)
    {
        auto* account = FindAccount(id);
        if (!IsObjectAlive(account)) {
            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::NoSuchAccount,
                "No such account %v",
                id);
        }
        return account;
    }

    TAccount* DoFindAccountByName(const TString& name)
    {
        // Access buggy parentless accounts by id.
        if (name.StartsWith(NObjectClient::ObjectIdPathPrefix)) {
            TStringBuf idString(name, NObjectClient::ObjectIdPathPrefix.size());
            auto id = TObjectId::FromString(idString);
            auto* account = AccountMap_.Find(id);
            if (!IsObjectAlive(account) || account->GetName() != name) {
                return nullptr;
            }
            return account;
        }

        auto it = AccountNameMap_.find(name);
        auto* account = it == AccountNameMap_.end() ? nullptr : it->second;
        return IsObjectAlive(account) ? account : nullptr;
    }

    TAccount* FindAccountByName(const TString& name, bool activeLifeStageOnly)
    {
        auto* account = DoFindAccountByName(name);
        if (!account) {
            return account;
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            return objectManager->IsObjectLifeStageValid(account)
                ? account
                : nullptr;
        } else {
            return account;
        }
    }

    TAccount* GetAccountByNameOrThrow(const TString& name, bool activeLifeStageOnly)
    {
        auto* account = DoFindAccountByName(name);
        if (!account) {
            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::NoSuchAccount,
                "No such account %Qv",
                name);
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->ValidateObjectLifeStage(account);
        }

        return account;
    }

    void RegisterAccountName(const TString& name, TAccount* account) noexcept
    {
        YT_VERIFY(account);
        YT_VERIFY(AccountNameMap_.emplace(name, account).second);
    }

    void UnregisterAccountName(const TString& name) noexcept
    {
        YT_VERIFY(AccountNameMap_.erase(name) == 1);
    }

    TAccount* GetRootAccount()
    {
        return GetBuiltin(RootAccount_);
    }

    TAccount* GetSysAccount()
    {
        return GetBuiltin(SysAccount_);
    }

    TAccount* GetTmpAccount()
    {
        return GetBuiltin(TmpAccount_);
    }

    TAccount* GetIntermediateAccount()
    {
        return GetBuiltin(IntermediateAccount_);
    }

    TAccount* GetChunkWiseAccountingMigrationAccount()
    {
        return GetBuiltin(ChunkWiseAccountingMigrationAccount_);
    }

    template <class... TArgs>
    void ThrowWithDetailedViolatedResources(
        const TClusterResourceLimits& limits, const TClusterResourceLimits& usage, TArgs&&... args)
    {
        auto violatedResources = limits.GetViolatedBy(usage);

        TStringStream output;
        TYsonWriter writer(&output, EYsonFormat::Binary);
        SerializeViolatedClusterResourceLimitsInCompactFormat(violatedResources, &writer, Bootstrap_);
        writer.Flush();

        THROW_ERROR(TError(std::forward<TArgs>(args)...)
            << TErrorAttribute("violated_resources", TYsonString(output.Str())));
    }

    TClusterResourceLimits ComputeAccountTotalChildrenLimits(TAccount* account)
    {
        auto totalChildrenLimits = account->ComputeTotalChildrenLimits();
        const auto& multicellManager = Bootstrap_->GetMulticellManager();

        const auto& dynamicConfig = GetDynamicConfig();
        if (!dynamicConfig->EnableMasterMemoryUsageAccountOvercommitValidation) {
            totalChildrenLimits.SetMasterMemory(TMasterMemoryLimits(0, 0, {}));
            totalChildrenLimits.MasterMemory().PerCell[multicellManager->GetPrimaryCellTag()] = 0;
            for (auto cellTag : multicellManager->GetSecondaryCellTags()) {
                totalChildrenLimits.MasterMemory().PerCell[cellTag] = 0;
            }
        } else {
            const auto& cellMasterMemoryLimits = account->ClusterResourceLimits().MasterMemory().PerCell;
            for (auto cellTag : multicellManager->GetSecondaryCellTags()) {
                if (!cellMasterMemoryLimits.contains(cellTag)) {
                    totalChildrenLimits.MasterMemory().PerCell[cellTag] = 0;
                }
            }
            auto primaryCellTag = multicellManager->GetPrimaryCellTag();
            if (!cellMasterMemoryLimits.contains(primaryCellTag)) {
                totalChildrenLimits.MasterMemory().PerCell[primaryCellTag] = 0;
            }
        }
        return totalChildrenLimits;
    }

    bool IsAccountOvercommitted(TAccount* account, TClusterResourceLimits newResources)
    {
        auto totalChildrenLimits = ComputeAccountTotalChildrenLimits(account);
        return newResources.IsViolatedBy(totalChildrenLimits);
    }

    template <class... TArgs>
    void ThrowAccountOvercommitted(TAccount* account, TClusterResourceLimits newResourceLimits, TArgs&&... args)
    {
        auto totalChildrenLimits = ComputeAccountTotalChildrenLimits(account);
        ThrowWithDetailedViolatedResources(
            newResourceLimits,
            totalChildrenLimits,
            std::forward<TArgs>(args)...);
    }

    void ValidateResourceLimits(TAccount* account, const TClusterResourceLimits& resourceLimits)
    {
        if (!Bootstrap_->GetMulticellManager()->IsPrimaryMaster()) {
            return;
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto zeroResources = TClusterResourceLimits::Zero(multicellManager);
        if (resourceLimits.IsViolatedBy(zeroResources)) {
            ThrowWithDetailedViolatedResources(
                resourceLimits,
                zeroResources,
                "Failed to change resource limits for account %Qv: limits cannot be negative",
                account->GetName());
        }

        const auto& currentResourceLimits = account->ClusterResourceLimits();
        if (resourceLimits.IsViolatedBy(currentResourceLimits)) {
            for (const auto& [key, child] : SortHashMapByKeys(account->KeyToChild())) {
                if (resourceLimits.IsViolatedBy(child->ClusterResourceLimits())) {
                    ThrowWithDetailedViolatedResources(
                        resourceLimits,
                        child->ClusterResourceLimits(),
                        "Failed to change resource limits for account %Qv: "
                        "the limit cannot be below that of its child account %Qv",
                        account->GetName(),
                        child->GetName());
                }
            }

            if (!account->GetAllowChildrenLimitOvercommit() && IsAccountOvercommitted(account, resourceLimits)) {
                ThrowAccountOvercommitted(
                    account,
                    resourceLimits,
                    "Failed to change resource limits for account %Qv: "
                    "the limit cannot be below the sum of its children limits",
                    account->GetName());
            }
        }

        if (currentResourceLimits.IsViolatedBy(resourceLimits)) {
            if (auto* parent = account->GetParent()) {
                const auto& parentResourceLimits = parent->ClusterResourceLimits();
                if (parentResourceLimits.IsViolatedBy(resourceLimits)) {
                    ThrowWithDetailedViolatedResources(
                        parentResourceLimits,
                        resourceLimits,
                        "Failed to change resource limits for account %Qv: "
                        "the limit cannot be above that of its parent",
                        account->GetName());
                }

                if (!parent->GetAllowChildrenLimitOvercommit() &&
                    IsAccountOvercommitted(parent, parentResourceLimits + currentResourceLimits - resourceLimits))
                {
                    ThrowAccountOvercommitted(
                        account,
                        parentResourceLimits + currentResourceLimits - resourceLimits,
                        "Failed to change resource limits for account %Qv: "
                        "the change would overcommit its parent",
                        account->GetName());
                }
            }
        }
    }

    void TrySetResourceLimits(TAccount* account, const TClusterResourceLimits& resourceLimits)
    {
        ValidateResourceLimits(account, resourceLimits);
        account->ClusterResourceLimits() = resourceLimits;
    }

    void TransferAccountResources(TAccount* srcAccount, TAccount* dstAccount, const TClusterResourceLimits& resourceDelta)
    {
        YT_VERIFY(srcAccount);
        YT_VERIFY(dstAccount);

        TCompactFlatMap<TAccount*, TClusterResourceLimits, 4> limitsBackup;
        try {
            if (resourceDelta.IsViolatedBy(TClusterResourceLimits())) {
                THROW_ERROR_EXCEPTION("Resource delta cannot be negative");
            }

            auto* lcaAccount = FindMapObjectLCA(srcAccount, dstAccount);
            if (!lcaAccount) {
                YT_LOG_ALERT("Two accounts do not have a common ancestor (AccountIds: %v)",
                    std::vector({srcAccount->GetId(), dstAccount->GetId()}));
                THROW_ERROR_EXCEPTION("Accounts do not have a common ancestor");
            }

            for (auto* account = srcAccount; account != lcaAccount; account = account->GetParent()) {
                ValidatePermission(account, EPermission::Write);
            }
            for (auto* account = dstAccount; account != lcaAccount; account = account->GetParent()) {
                ValidatePermission(account, EPermission::Write);
            }

            auto tryIncrementLimits = [&] (TAccount* account, const TClusterResourceLimits& delta) {
                auto oldLimits = account->ClusterResourceLimits();
                auto newLimits = oldLimits + delta;

                ValidateResourceLimits(account, newLimits);
                YT_VERIFY(limitsBackup.insert({account, oldLimits}).second);
                account->ClusterResourceLimits() = newLimits;
            };

            for (auto* account = srcAccount; account != lcaAccount; account = account->GetParent()) {
                tryIncrementLimits(account, -resourceDelta);
            }

            TCompactVector<TAccount*, 4> dstAncestors;
            for (auto* account = dstAccount; account != lcaAccount; account = account->GetParent()) {
                dstAncestors.push_back(account);
            }
            std::reverse(dstAncestors.begin(), dstAncestors.end());
            for (auto* account : dstAncestors) {
                tryIncrementLimits(account, resourceDelta);
            }
        } catch (const std::exception& ex) {
            for (const auto& [account, oldLimits] : limitsBackup) {
                account->ClusterResourceLimits() = oldLimits;
            }
            THROW_ERROR_EXCEPTION("Failed to transfer resources from account %Qv to account %Qv",
                srcAccount->GetName(),
                dstAccount->GetName())
                << ex;
        }
    }

    void UpdateResourceUsage(const TChunk* chunk, const TChunkRequisition& requisition, i64 delta)
    {
        YT_VERIFY(chunk->IsNative());

        auto doCharge = [] (TClusterResources* usage, int mediumIndex, i64 chunkCount, i64 diskSpace) {
            usage->AddToMediumDiskSpace(mediumIndex, diskSpace);
            usage->SetChunkCount(usage->GetChunkCount() + chunkCount);
        };

        ComputeChunkResourceDelta(
            chunk,
            requisition,
            delta,
            [&] (TAccount* account, int mediumIndex, i64 chunkCount, i64 diskSpace, i64 masterMemory, bool committed) {
                account->DetailedMasterMemoryUsage()[EMasterMemoryType::Chunks] += masterMemory;
                doCharge(&account->ClusterStatistics().ResourceUsage, mediumIndex, chunkCount, diskSpace);
                doCharge(&account->LocalStatistics().ResourceUsage, mediumIndex, chunkCount, diskSpace);
                if (committed) {
                    doCharge(&account->ClusterStatistics().CommittedResourceUsage, mediumIndex, chunkCount, diskSpace);
                    doCharge(&account->LocalStatistics().CommittedResourceUsage, mediumIndex, chunkCount, diskSpace);
                }
            });
    }

    void UpdateAccountResourceUsageLease(
        TAccountResourceUsageLease* accountResourceUsageLease,
        const TClusterResources& resources)
    {
        auto* account = accountResourceUsageLease->GetAccount();
        auto* transaction = accountResourceUsageLease->GetTransaction();

        if (resources.GetNodeCount() > 0) {
            THROW_ERROR_EXCEPTION(
                "Account resource usage lease update supports only disk resources, but \"node_count\" is specified");
        }
        if (resources.GetChunkCount() > 0) {
            THROW_ERROR_EXCEPTION(
                "Account resource usage lease update supports only disk resources, but \"chunk_count\" is specified");
        }
        if (resources.GetTabletCount() > 0) {
            THROW_ERROR_EXCEPTION(
                "Account resource usage lease update supports only disk resources, but \"tablet_count\" is specified");
        }
        if (resources.GetTabletStaticMemory() > 0) {
            THROW_ERROR_EXCEPTION(
                "Account resource usage lease update supports only disk resources, but \"tablet_static_memory\" is specified");
        }

        ValidatePermission(account, EPermission::Use);
        ValidatePermission(transaction, EPermission::Write);

        auto resourcesDelta = resources - accountResourceUsageLease->Resources();

        ValidateResourceUsageIncrease(account, resourcesDelta, /*allowRootAccount*/ true);

        DoUpdateAccountResourceUsageLease(accountResourceUsageLease, resources);

    }

    void UpdateTransactionResourceUsage(
        const TChunk* chunk,
        const TChunkRequisition& requisition,
        i64 delta)
    {
        YT_ASSERT(chunk->IsStaged());
        YT_ASSERT(chunk->IsDiskSizeFinal());

        auto* stagingTransaction = chunk->GetStagingTransaction();
        auto* stagingAccount = chunk->GetStagingAccount();

        auto chargeTransaction = [&] (TAccount* account, int mediumIndex, i64 chunkCount, i64 diskSpace, i64 /*masterMemoryUsage*/, bool /*committed*/) {
            // If a chunk has been created before the migration but is being confirmed after it,
            // charge it to the staging account anyway: it's ok, because transaction resource usage accounting
            // isn't really delta-based, and it's nicer from the user's point of view.
            if (Y_UNLIKELY(account == ChunkWiseAccountingMigrationAccount_)) {
                account = stagingAccount;
            }

            auto* transactionUsage = GetTransactionAccountUsage(stagingTransaction, account);
            transactionUsage->AddToMediumDiskSpace(mediumIndex, diskSpace);
            transactionUsage->SetChunkCount(transactionUsage->GetChunkCount() + chunkCount);
        };

        ComputeChunkResourceDelta(chunk, requisition, delta, chargeTransaction);
    }

    void ResetMasterMemoryUsage(TCypressNode* node)
    {
        auto* account = node->GetAccount();
        auto chargedMasterMemoryUsage = ChargeMasterMemoryUsage(
            account,
            TDetailedMasterMemory(),
            node->ChargedDetailedMasterMemoryUsage());
        YT_ASSERT(chargedMasterMemoryUsage == TDetailedMasterMemory());
        node->ChargedDetailedMasterMemoryUsage() = chargedMasterMemoryUsage;

        if (IsTableType(node->GetType())) {
            // NB: this may also be a replicated table.
            const auto* table = node->As<TTableNode>();
            ResetMasterMemoryUsage(table->GetSchema(), account);
        }
    }

    void UpdateMasterMemoryUsage(TCypressNode* node, bool accountChanged = false)
    {
        auto* account = node->GetAccount();
        if (!account) {
            return;
        }

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        const auto& typeHandler = cypressManager->GetHandler(node);
        auto detailedMasterMemoryUsage = node->GetDetailedMasterMemoryUsage();
        auto staticMasterMemoryUsage = typeHandler->GetStaticMasterMemoryUsage();
        detailedMasterMemoryUsage[EMasterMemoryType::Nodes] += staticMasterMemoryUsage;
        auto chargedMasterMemoryUsage = ChargeMasterMemoryUsage(
            account,
            detailedMasterMemoryUsage,
            node->ChargedDetailedMasterMemoryUsage());
        YT_ASSERT(chargedMasterMemoryUsage == detailedMasterMemoryUsage);
        node->ChargedDetailedMasterMemoryUsage() = chargedMasterMemoryUsage;

        if (accountChanged && IsTableType(node->GetType())) {
            // NB: this may also be a replicated table.
            auto* table = node->As<TTableNode>();
            UpdateMasterMemoryUsage(table->GetSchema(), account);
        }
    }

    void ResetMasterMemoryUsage(TMasterTableSchema* schema, TAccount* account)
    {
        YT_VERIFY(IsObjectAlive(account));

        if (!schema) {
            return;
        }

        if (schema->UnrefBy(account)) {
            TDetailedMasterMemory currentMasterMemoryUsage; // Leaving empty.

            TDetailedMasterMemory chargedMasterMemoryUsage;
            chargedMasterMemoryUsage[EMasterMemoryType::Schemas] += schema->GetChargedMasterMemoryUsage(account);

            chargedMasterMemoryUsage = ChargeMasterMemoryUsage(account, currentMasterMemoryUsage, chargedMasterMemoryUsage);
            YT_ASSERT(chargedMasterMemoryUsage == currentMasterMemoryUsage);

            schema->SetChargedMasterMemoryUsage(
                account,
                chargedMasterMemoryUsage[EMasterMemoryType::Schemas]);
        }
    }

    void UpdateMasterMemoryUsage(TMasterTableSchema* schema, TAccount* account)
    {
        if (!schema) {
            return;
        }

        YT_VERIFY(IsObjectAlive(account));

        if (schema->RefBy(account)) {
            TDetailedMasterMemory currentMasterMemoryUsage;
            currentMasterMemoryUsage[EMasterMemoryType::Schemas] += schema->GetMasterMemoryUsage(account);

            TDetailedMasterMemory chargedMasterMemoryUsage;
            chargedMasterMemoryUsage[EMasterMemoryType::Schemas] += schema->GetChargedMasterMemoryUsage(account);

            chargedMasterMemoryUsage = ChargeMasterMemoryUsage(account, currentMasterMemoryUsage, chargedMasterMemoryUsage);
            YT_ASSERT(chargedMasterMemoryUsage == currentMasterMemoryUsage);
            schema->SetChargedMasterMemoryUsage(
                account,
                chargedMasterMemoryUsage[EMasterMemoryType::Schemas]);
        }
    }

    [[nodiscard]] TDetailedMasterMemory ChargeMasterMemoryUsage(
        TAccount* account,
        const TDetailedMasterMemory& currentDetailedMasterMemoryUsage,
        const TDetailedMasterMemory& chargedDetailedMasterMemoryUsage)
    {
        auto delta = currentDetailedMasterMemoryUsage - chargedDetailedMasterMemoryUsage;
        YT_LOG_TRACE_IF(IsMutationLoggingEnabled(), "Updating master memory usage (Account: %v, MasterMemoryUsage: %v, Delta: %v)",
            account->GetName(),
            account->DetailedMasterMemoryUsage(),
            delta);
        ChargeAccountAncestry(
            account,
            [&] (TAccount* account) {
                account->DetailedMasterMemoryUsage() += delta;
            });
        if (account->DetailedMasterMemoryUsage().IsNegative()) {
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Master memory usage is negative (Account: %v, MasterMemoryUsage: %v)",
                account->GetName(),
                account->DetailedMasterMemoryUsage());
        }
        return currentDetailedMasterMemoryUsage;
    }

    void ResetTransactionAccountResourceUsage(TTransaction* transaction)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        for (const auto& [account, usage] : transaction->AccountResourceUsage()) {
            objectManager->UnrefObject(account);
        }
        transaction->AccountResourceUsage().clear();
    }

    void RecomputeTransactionResourceUsage(TTransaction* transaction)
    {
        ResetTransactionAccountResourceUsage(transaction);

        auto addNodeResourceUsage = [&] (const TCypressNode* node) {
            if (node->IsExternal()) {
                return;
            }
            auto* account = node->GetAccount();
            auto* transactionUsage = GetTransactionAccountUsage(transaction, account);
            *transactionUsage += node->GetDeltaResourceUsage();
        };

        for (auto* node : transaction->BranchedNodes()) {
            addNodeResourceUsage(node);
        }
        for (auto* node : transaction->StagedNodes()) {
            addNodeResourceUsage(node);
        }
    }

    void SetAccount(
        TCypressNode* node,
        TAccount* newAccount,
        TTransaction* transaction) noexcept
    {
        YT_VERIFY(node);
        YT_VERIFY(newAccount);
        YT_VERIFY(node->IsTrunk() == !transaction);

        auto* oldAccount = node->GetAccount();
        YT_VERIFY(!oldAccount || !transaction);

        if (oldAccount == newAccount) {
            return;
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& cypressManager = Bootstrap_->GetCypressManager();

        if (oldAccount) {
            if (auto* shard = node->GetShard()) {
                cypressManager->UpdateShardNodeCount(shard, oldAccount, -1);
            }
            UpdateAccountNodeCountUsage(node, oldAccount, nullptr, -1);
            ResetMasterMemoryUsage(node);
            objectManager->UnrefObject(oldAccount);
        }

        if (auto* shard = node->GetShard()) {
            cypressManager->UpdateShardNodeCount(shard, newAccount, +1);
        }
        UpdateAccountNodeCountUsage(node, newAccount, transaction, +1);
        node->SetAccount(newAccount);
        UpdateMasterMemoryUsage(node, /*accountChanged*/ true);
        objectManager->RefObject(newAccount);
        UpdateAccountTabletResourceUsage(node, oldAccount, true, newAccount, !transaction);
    }

    void ResetAccount(TCypressNode* node)
    {
        auto* account = node->GetAccount();
        if (!account) {
            return;
        }

        ResetMasterMemoryUsage(node);
        node->SetAccount(nullptr);

        UpdateAccountNodeCountUsage(node, account, node->GetTransaction(), -1);
        UpdateAccountTabletResourceUsage(node, account, !node->GetTransaction(), nullptr, false);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->UnrefObject(account);
    }

    void UpdateAccountNodeCountUsage(TCypressNode* node, TAccount* account, TTransaction* transaction, i64 delta)
    {
        if (node->IsExternal()) {
            return;
        }

        auto resources = TClusterResources()
            .SetNodeCount(node->GetDeltaResourceUsage().GetNodeCount()) * delta;

        ChargeAccountAncestry(
            account,
            [&] (TAccount* account) {
                account->ClusterStatistics().ResourceUsage += resources;
                account->LocalStatistics().ResourceUsage += resources;
            });

        if (transaction) {
            auto* transactionUsage = GetTransactionAccountUsage(transaction, account);
            *transactionUsage += resources;
        } else {
            ChargeAccountAncestry(
                account,
                [&] (TAccount* account) {
                    account->ClusterStatistics().CommittedResourceUsage += resources;
                    account->LocalStatistics().CommittedResourceUsage += resources;
                });
        }
    }

    void UpdateAccountTabletResourceUsage(TCypressNode* node, TAccount* oldAccount, bool oldCommitted, TAccount* newAccount, bool newCommitted)
    {
        if (node->IsExternal()) {
            return;
        }

        auto resources = ConvertToClusterResources(node->GetTabletResourceUsage());

        UpdateTabletResourceUsage(oldAccount, -resources, oldCommitted);
        UpdateTabletResourceUsage(newAccount, resources, newCommitted);
    }

    void UpdateTabletResourceUsage(TCypressNode* node, const TClusterResources& resourceUsageDelta)
    {
        UpdateTabletResourceUsage(node->GetAccount(), resourceUsageDelta, node->IsTrunk());
    }

    void UpdateTabletResourceUsage(TAccount* account, const TClusterResources& resourceUsageDelta, bool committed)
    {
        if (!account) {
            return;
        }

        YT_ASSERT(resourceUsageDelta.GetNodeCount() == 0);
        YT_ASSERT(resourceUsageDelta.GetChunkCount() == 0);
        for (auto [mediumIndex, diskUsage] : resourceUsageDelta.DiskSpace()) {
            YT_ASSERT(diskUsage == 0);
        }
        YT_ASSERT(resourceUsageDelta.DetailedMasterMemory().IsZero());

        ChargeAccountAncestry(
            account,
            [&] (TAccount* account) {
                account->ClusterStatistics().ResourceUsage += resourceUsageDelta;
                account->LocalStatistics().ResourceUsage += resourceUsageDelta;
                if (committed) {
                    account->ClusterStatistics().CommittedResourceUsage += resourceUsageDelta;
                    account->LocalStatistics().CommittedResourceUsage += resourceUsageDelta;
                }
            });
    }

    TAccountResourceUsageLease* CreateAccountResourceUsageLease(const TString& accountName, TTransactionId transactionId, TObjectId hintId)
    {
        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);

        auto* account = GetAccountByNameOrThrow(accountName, /*activeLifeStageOnly*/ true);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::AccountResourceUsageLease, hintId);

        auto accountResourceUsageLeaseHolder = TPoolAllocator::New<TAccountResourceUsageLease>(
            id,
            transaction,
            account);
        auto* accountResourceUsageLease = AccountResourceUsageLeaseMap_.Insert(id, std::move(accountResourceUsageLeaseHolder));
        YT_VERIFY(accountResourceUsageLease->RefObject() == 1);

        // NB: lifetime of the lease is bound to the lifetime of the transaction.
        // Therefore we should not call RefObject on the transaction object.
        objectManager->RefObject(account);
        YT_VERIFY(transaction->AccountResourceUsageLeases().insert(accountResourceUsageLease).second);

        YT_LOG_DEBUG_IF(
            IsMutationLoggingEnabled(),
            "Account usage lease created (Id: %v, Account: %v, TransactionId: %v)",
            accountResourceUsageLease->GetId(),
            accountResourceUsageLease->GetAccount()->GetName(),
            accountResourceUsageLease->GetTransaction()->GetId());

        return accountResourceUsageLease;
    }

    void DestroyAccountResourceUsageLease(TAccountResourceUsageLease* accountResourceUsageLease)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();

        auto accountResourceUsageLeaseId = accountResourceUsageLease->GetId();

        DoUpdateAccountResourceUsageLease(accountResourceUsageLease, TClusterResources());

        auto* transaction = accountResourceUsageLease->GetTransaction();
        YT_VERIFY(transaction->AccountResourceUsageLeases().erase(accountResourceUsageLease) == 1);

        objectManager->UnrefObject(accountResourceUsageLease->GetAccount());

        YT_LOG_DEBUG_IF(
            IsMutationLoggingEnabled(),
            "Account usage lease destroyed (Id: %v)",
            accountResourceUsageLeaseId);
    }

    void DestroySubject(TSubject* subject)
    {
        for (auto* group : subject->MemberOf()) {
            YT_VERIFY(group->Members().erase(subject) == 1);
        }
        subject->MemberOf().clear();
        subject->RecursiveMemberOf().clear();

        for (const auto& alias : subject->Aliases()) {
            YT_VERIFY(SubjectAliasMap_.erase(alias) == 1);
        }

        for (auto [object, counter] : subject->LinkedObjects()) {
            auto* acd = GetAcd(object);
            acd->OnSubjectDestroyed(subject, GuestUser_);
        }
        subject->LinkedObjects().clear();
    }

    TUser* CreateUser(const TString& name, TObjectId hintId)
    {
        ValidateSubjectName(name);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::User, hintId);
        auto* user = DoCreateUser(id, name);
        if (user) {
            YT_LOG_DEBUG("User created (User: %v)", name);
            LogStructuredEventFluently(Logger, ELogLevel::Info)
                .Item("event").Value(EAccessControlEvent::UserCreated)
                .Item("name").Value(user->GetName());
        }
        return user;
    }

    void DestroyUser(TUser* user)
    {
        YT_VERIFY(UserNameMap_.erase(user->GetName()) == 1);
        DestroySubject(user);

        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::UserDestroyed)
            .Item("name").Value(user->GetName());
    }

    TUser* DoFindUserByName(const TString& name)
    {
        auto it = UserNameMap_.find(name);
        return it == UserNameMap_.end() ? nullptr : it->second;
    }

    TUser* FindUserByName(const TString& name, bool activeLifeStageOnly)
    {
        auto* user = DoFindUserByName(name);
        if (!user) {
            return user;
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            return objectManager->IsObjectLifeStageValid(user)
                ? user
                : nullptr;
        } else {
            return user;
        }
    }

    TUser* FindUserByNameOrAlias(const TString& name, bool activeLifeStageOnly)
    {
        auto* subjectByAlias = FindSubjectByAlias(name, activeLifeStageOnly);
        if (subjectByAlias && subjectByAlias->IsUser()) {
            return subjectByAlias->AsUser();
        }
        return FindUserByName(name, activeLifeStageOnly);
    }

    TUser* GetUserByNameOrThrow(const TString& name, bool activeLifeStageOnly)
    {
        auto* user = DoFindUserByName(name);

        if (!IsObjectAlive(user)) {
            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::AuthenticationError,
                "No such user %Qv; create user by requesting any IDM role on this cluster",
                name);
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->ValidateObjectLifeStage(user);
        }

        return user;
    }

    TUser* GetUserOrThrow(TUserId id)
    {
        auto* user = FindUser(id);
        if (!IsObjectAlive(user)) {
            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::AuthenticationError,
                "No such user %Qv",
                id);
        }
        return user;
    }


    TUser* GetRootUser()
    {
        return GetBuiltin(RootUser_);
    }

    TUser* GetGuestUser()
    {
        return GetBuiltin(GuestUser_);
    }

    TUser* GetOwnerUser()
    {
        return GetBuiltin(OwnerUser_);
    }


    TGroup* CreateGroup(const TString& name, TObjectId hintId)
    {
        ValidateSubjectName(name);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::Group, hintId);
        auto* group = DoCreateGroup(id, name);
        if (group) {
            YT_LOG_DEBUG("Group created (Group: %v)", name);
            LogStructuredEventFluently(Logger, ELogLevel::Info)
                .Item("event").Value(EAccessControlEvent::GroupCreated)
                .Item("name").Value(name);
        }
        return group;
    }

    void DestroyGroup(TGroup* group)
    {
        YT_VERIFY(GroupNameMap_.erase(group->GetName()) == 1);

        for (auto* subject : group->Members()) {
            YT_VERIFY(subject->MemberOf().erase(group) == 1);
        }
        group->Members().clear();

        auto removeFromRecursiveClosure = [&] <typename TSubjectMap> (TSubjectMap& subjectMap) {
            for (auto [subjectId, subject] : subjectMap) {
                subject->RecursiveMemberOf().erase(group);
            }
        };
        removeFromRecursiveClosure(UserMap_);
        removeFromRecursiveClosure(GroupMap_);

        DestroySubject(group);

        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::GroupDestroyed)
            .Item("name").Value(group->GetName());
    }

    // This is here just for the sake of symmetry with user and account counterparts.
    TGroup* DoFindGroupByName(const TString& name)
    {
        auto it = GroupNameMap_.find(name);
        return it == GroupNameMap_.end() ? nullptr : it->second;
    }

    TGroup* FindGroupByName(const TString& name)
    {
        return DoFindGroupByName(name);
    }

    TGroup* FindGroupByNameOrAlias(const TString& name)
    {
        auto* subjectByAlias = DoFindSubjectByAlias(name);
        if (subjectByAlias && subjectByAlias->IsGroup()) {
            return subjectByAlias->AsGroup();
        }
        return DoFindGroupByName(name);
    }

    TGroup* GetEveryoneGroup()
    {
        return GetBuiltin(EveryoneGroup_);
    }

    TGroup* GetUsersGroup()
    {
        return GetBuiltin(UsersGroup_);
    }

    TGroup* GetSuperusersGroup()
    {
        return GetBuiltin(SuperusersGroup_);
    }


    TSubject* FindSubject(TSubjectId id)
    {
        auto* user = FindUser(id);
        if (IsObjectAlive(user)) {
            return user;
        }
        auto* group = FindGroup(id);
        if (IsObjectAlive(group)) {
            return group;
        }
        return nullptr;
    }

    TSubject* GetSubjectOrThrow(TSubjectId id)
    {
        auto* subject = FindSubject(id);
        if (!IsObjectAlive(subject)) {
            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::NoSuchSubject,
                "No such subject %v",
                id);
        }
        return subject;
    }

    TNetworkProject* CreateNetworkProject(const TString& name, TObjectId hintId)
    {
        if (FindNetworkProjectByName(name)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Network project %Qv already exists",
                name);
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::NetworkProject, hintId);
        auto* networkProject = DoCreateNetworkProject(id, name);
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Network project created (NetworkProject: %v)", name);
        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::NetworkProjectCreated)
            .Item("name").Value(networkProject->GetName());
        return networkProject;
    }

    void DestroyNetworkProject(TNetworkProject* networkProject)
    {
        YT_VERIFY(NetworkProjectNameMap_.erase(networkProject->GetName()) == 1);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Network project destroyed (NetworkProject: %v)",
            networkProject->GetName());
        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::NetworkProjectDestroyed)
            .Item("name").Value(networkProject->GetName());
    }

    TSubject* DoFindSubjectByAlias(const TString& alias)
    {
        auto it = SubjectAliasMap_.find(alias);
        return it == SubjectAliasMap_.end() ? nullptr : it->second;
    }

    TSubject* FindSubjectByAlias(const TString& alias, bool activeLifeStageOnly)
    {
        auto* subject = DoFindSubjectByAlias(alias);
        if (!subject) {
            return subject;
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            return objectManager->IsObjectLifeStageValid(subject)
                ? subject
                : nullptr;
        } else {
            return subject;
        }
    }

    TSubject* FindSubjectByNameOrAlias(const TString& name, bool activeLifeStageOnly)
    {
        auto* user = FindUserByName(name, activeLifeStageOnly);
        if (user) {
            return user;
        }

        auto* group = FindGroupByName(name);
        if (group) {
            return group;
        }

        auto* subjectByAlias = FindSubjectByAlias(name, activeLifeStageOnly);
        if (subjectByAlias) {
            return subjectByAlias;
        }

        return nullptr;
    }

    TSubject* GetSubjectByNameOrAliasOrThrow(const TString& name, bool activeLifeStageOnly)
    {
        auto validateLifeStage = [&] (TSubject* subject) {
            if (activeLifeStageOnly) {
                const auto& objectManager = Bootstrap_->GetObjectManager();
                objectManager->ValidateObjectLifeStage(subject);
            }
        };

        auto* user = DoFindUserByName(name);
        if (user) {
            validateLifeStage(user);
            return user;
        }

        auto* group = DoFindGroupByName(name);
        if (group) {
            return group;
        }

        auto* subjectByAlias = DoFindSubjectByAlias(name);
        if (subjectByAlias) {
            validateLifeStage(subjectByAlias);
            return subjectByAlias;
        }

        THROW_ERROR_EXCEPTION(
            NSecurityClient::EErrorCode::NoSuchSubject,
            "No such subject %Qv",
            name);
    }

    TNetworkProject* FindNetworkProjectByName(const TString& name)
    {
        auto it = NetworkProjectNameMap_.find(name);
        return it == NetworkProjectNameMap_.end() ? nullptr : it->second;
    }

    void RenameNetworkProject(NYT::NSecurityServer::TNetworkProject* networkProject, const TString& newName)
    {
        if (FindNetworkProjectByName(newName)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Network project %Qv already exists",
                newName);
        }

        YT_VERIFY(NetworkProjectNameMap_.erase(networkProject->GetName()) == 1);
        YT_VERIFY(NetworkProjectNameMap_.emplace(newName, networkProject).second);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Network project renamed (NetworkProject: %v, OldName: %v, NewName: %v",
            networkProject->GetId(),
            networkProject->GetName(),
            newName);

        networkProject->SetName(newName);
    }

    TProxyRole* CreateProxyRole(const TString& name, EProxyKind proxyKind, TObjectId hintId)
    {
        if (ProxyRoleNameMaps_[proxyKind].contains(name)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Proxy role %Qv with proxy kind %Qlv already exists",
                name,
                proxyKind);
        }

        if (name.empty()) {
            THROW_ERROR_EXCEPTION("Proxy role name cannot be empty");
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::ProxyRole, hintId);
        auto* proxyRole = DoCreateProxyRole(id, name, proxyKind);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Proxy role created (Name: %v, ProxyKind: %v)",
            name,
            proxyKind);
        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::ProxyRoleCreated)
            .Item("name").Value(name)
            .Item("proxy_kind").Value(proxyKind);

        return proxyRole;
    }

    void DestroyProxyRole(TProxyRole* proxyRole)
    {
        auto name = proxyRole->GetName();
        auto proxyKind = proxyRole->GetProxyKind();

        YT_VERIFY(ProxyRoleNameMaps_[proxyKind].erase(name) == 1);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Proxy role destroyed (Name: %v, ProxyKind: %v)",
            name,
            proxyKind);
        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::ProxyRoleDestroyed)
            .Item("name").Value(name)
            .Item("proxy_kind").Value(proxyKind);
    }

    const THashMap<TString, TProxyRole*>& GetProxyRolesWithProxyKind(EProxyKind proxyKind) const
    {
        return ProxyRoleNameMaps_[proxyKind];
    }

    void AddMember(TGroup* group, TSubject* member, bool ignoreExisting)
    {
        ValidateMembershipUpdate(group, member);

        if (group->Members().find(member) != group->Members().end()) {
            if (ignoreExisting) {
                return;
            }
            THROW_ERROR_EXCEPTION("Member %Qv is already present in group %Qv",
                member->GetName(),
                group->GetName());
        }

        if (member->GetType() == EObjectType::Group) {
            auto* memberGroup = member->AsGroup();
            if (group == memberGroup || group->RecursiveMemberOf().find(memberGroup) != group->RecursiveMemberOf().end()) {
                THROW_ERROR_EXCEPTION("Adding group %Qv to group %Qv would produce a cycle",
                    memberGroup->GetName(),
                    group->GetName());
            }
        }

        DoAddMember(group, member);
        MaybeRecomputeMembershipClosure();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Group member added (Group: %v, Member: %v)",
            group->GetName(),
            member->GetName());

        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::MemberAdded)
            .Item("group_name").Value(group->GetName())
            .Item("member_type").Value(member->GetType())
            .Item("member_name").Value(member->GetName());
    }

    void RemoveMember(TGroup* group, TSubject* member, bool force)
    {
        ValidateMembershipUpdate(group, member);

        if (group->Members().find(member) == group->Members().end()) {
            if (force) {
                return;
            }
            THROW_ERROR_EXCEPTION("Member %Qv is not present in group %Qv",
                member->GetName(),
                group->GetName());
        }

        DoRemoveMember(group, member);
        MaybeRecomputeMembershipClosure();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Group member removed (Group: %v, Member: %v)",
            group->GetName(),
            member->GetName());

        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::MemberRemoved)
            .Item("group_name").Value(group->GetName())
            .Item("member_type").Value(member->GetType())
            .Item("member_name").Value(member->GetName());
    }

    void RenameSubject(TSubject* subject, const TString& newName)
    {
        ValidateSubjectName(newName);

        switch (subject->GetType()) {
            case EObjectType::User:
                YT_VERIFY(UserNameMap_.erase(subject->GetName()) == 1);
                YT_VERIFY(UserNameMap_.emplace(newName, subject->AsUser()).second);
                break;

            case EObjectType::Group:
                YT_VERIFY(GroupNameMap_.erase(subject->GetName()) == 1);
                YT_VERIFY(GroupNameMap_.emplace(newName, subject->AsGroup()).second);
                break;

            default:
                YT_ABORT();
        }

        LogStructuredEventFluently(Logger, ELogLevel::Info)
            .Item("event").Value(EAccessControlEvent::SubjectRenamed)
            .Item("subject_type").Value(subject->GetType())
            .Item("old_name").Value(subject->GetName())
            .Item("new_name").Value(newName);

        subject->SetName(newName);
    }

    TAccessControlDescriptor* FindAcd(TObject* object)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& handler = objectManager->GetHandler(object);
        return handler->FindAcd(object);
    }

    TAccessControlDescriptor* GetAcd(TObject* object)
    {
        auto* acd = FindAcd(object);
        YT_VERIFY(acd);
        return acd;
    }

    std::optional<TString> GetEffectiveAnnotation(TCypressNode* node)
    {
        while (node && !node->TryGetAnnotation()) {
            node = node->GetParent();
        }
        return node ? node->TryGetAnnotation() : std::nullopt;
    }

    TAccessControlList GetEffectiveAcl(NObjectServer::TObject* object)
    {
        TAccessControlList result;
        const auto& objectManager = Bootstrap_->GetObjectManager();
        int depth = 0;
        while (object) {
            const auto& handler = objectManager->GetHandler(object);
            auto* acd = handler->FindAcd(object);
            if (acd) {
                for (auto entry : acd->Acl().Entries) {
                    auto inheritedMode = GetInheritedInheritanceMode(entry.InheritanceMode, depth);
                    if (inheritedMode) {
                        entry.InheritanceMode = *inheritedMode;
                        result.Entries.push_back(entry);
                    }
                }
                if (!acd->GetInherit()) {
                    break;
                }
            }

            object = handler->GetParent(object);
            ++depth;
        }

        return result;
    }


    void SetAuthenticatedUser(TUser* user)
    {
        *AuthenticatedUser_ = user;
    }

    void ResetAuthenticatedUser()
    {
        *AuthenticatedUser_ = nullptr;
    }

    TUser* GetAuthenticatedUser()
    {
        TUser* result = nullptr;

        if (AuthenticatedUser_.IsInitialized()) {
            result = *AuthenticatedUser_;
        }

        return result ? result : RootUser_;
    }


    bool IsSafeMode()
    {
        return Bootstrap_->GetConfigManager()->GetConfig()->EnableSafeMode;
    }

    TPermissionCheckResponse CheckPermission(
        TObject* object,
        TUser* user,
        EPermission permission,
        TPermissionCheckOptions options = {})
    {
        if (IsVersionedType(object->GetType()) && object->IsForeign()) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Checking permission for a versioned foreign object (ObjectId: %v)",
                object->GetId());
        }

        if (permission == EPermission::FullRead) {
            YT_VERIFY(!options.Columns);
            const auto& objectManager = Bootstrap_->GetObjectManager();
            const auto& handler = objectManager->GetHandler(object);
            options.Columns = handler->ListColumns(object);
        }

        TPermissionChecker checker(
            this,
            user,
            permission,
            options);

        if (!checker.ShouldProceed()) {
            return checker.GetResponse();
        }

        // Slow lane: check ACLs through the object hierarchy.
        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        const auto* rootObject = cypressManager->GetRootNode();
        auto* currentObject = object;
        TSubject* owner = nullptr;
        int depth = 0;
        while (currentObject) {
            const auto& handler = objectManager->GetHandler(currentObject);
            auto* acd = handler->FindAcd(currentObject);

            // Check the current ACL, if any.
            if (acd) {
                if (!owner && currentObject == object) {
                    owner = acd->GetOwner();
                }

                for (const auto& ace: acd->Acl().Entries) {
                    checker.ProcessAce(ace, owner, currentObject, depth);
                    if (!checker.ShouldProceed()) {
                        break;
                    }
                }

                // Proceed to the parent object unless the current ACL explicitly forbids inheritance.
                if (!acd->GetInherit()) {
                    break;
                }
            }

            auto* parentObject = handler->GetParent(currentObject);

            // XXX(shakurov): YT-3005, YT-10896: remove this workaround.
            if (IsVersionedType(object->GetType())) {
                // Check if current object is orphaned.
                if (!parentObject && currentObject != rootObject) {
                    checker.ProcessAce(
                        TAccessControlEntry(
                            ESecurityAction::Allow,
                            GetEveryoneGroup(),
                            EPermissionSet(EPermission::Read)),
                        owner,
                        currentObject,
                        depth);
                }
            }

            currentObject = parentObject;
            ++depth;
        }

        return checker.GetResponse();
    }

    TPermissionCheckResponse CheckPermission(
        TUser* user,
        EPermission permission,
        const TAccessControlList& acl,
        TPermissionCheckOptions options = {})
    {
        TPermissionChecker checker(
            this,
            user,
            permission,
            options);

        if (!checker.ShouldProceed()) {
            return checker.GetResponse();
        }

        for (const auto& ace : acl.Entries) {
            checker.ProcessAce(ace, nullptr, nullptr, 0);
            if (!checker.ShouldProceed()) {
                break;
            }
        }

        return checker.GetResponse();
    }

    bool IsSuperuser(const TUser* user) const
    {
        // NB: This is also useful for migration when "superusers" is initially created.
        if (user == RootUser_) {
            return true;
        }

        if (user->RecursiveMemberOf().find(SuperusersGroup_) != user->RecursiveMemberOf().end()) {
            return true;
        }

        return false;
    }

    void ValidatePermission(
        TObject* object,
        TUser* user,
        EPermission permission,
        TPermissionCheckOptions options = {})
    {
        if (IsPermissionValidationSuppressed()) {
            return;
        }

        YT_VERIFY(!options.Columns);

        auto response = CheckPermission(object, user, permission, std::move(options));
        if (response.Action == ESecurityAction::Allow) {
            return;
        }

        TPermissionCheckTarget target;
        target.Object = object;
        LogAndThrowAuthorizationError(
            target,
            user,
            permission,
            response);
    }

    void ValidatePermission(
        TObject* object,
        EPermission permission,
        TPermissionCheckOptions options = {})
    {
        ValidatePermission(
            object,
            GetAuthenticatedUser(),
            permission,
            std::move(options));
    }

    void LogAndThrowAuthorizationError(
        const TPermissionCheckTarget& target,
        TUser* user,
        EPermission permission,
        const TPermissionCheckResult& result)
    {
        if (result.Action != ESecurityAction::Deny) {
            return;
        }

        auto objectName = GetPermissionCheckTargetName(target);

        TError error;

        if (IsSafeMode()) {
            error = TError(
                NSecurityClient::EErrorCode::SafeModeEnabled,
                "Access denied for user %Qv: cluster is in safe mode; "
                "check for announces at https://infra.yandex-team.ru before reporting any issues",
                user->GetName());
        } else {
            auto event = LogStructuredEventFluently(Logger, ELogLevel::Info)
                .Item("event").Value(EAccessControlEvent::AccessDenied)
                .Item("user").Value(user->GetName())
                .Item("permission").Value(permission)
                .Item("object_name").Value(objectName);

            if (target.Column) {
                event = event
                    .Item("object_column").Value(*target.Column);
            }

            if (result.Object && result.Subject) {
                const auto& objectManager = Bootstrap_->GetObjectManager();
                auto deniedBy = objectManager->GetHandler(result.Object)->GetName(result.Object);

                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied for user %Qv: %Qlv permission for %v is denied for %Qv by ACE at %v",
                    user->GetName(),
                    permission,
                    objectName,
                    result.Subject->GetName(),
                    deniedBy)
                    << TErrorAttribute("denied_by", result.Object->GetId())
                    << TErrorAttribute("denied_for", result.Subject->GetId());

                event
                    .Item("reason").Value(EAccessDeniedReason::DeniedByAce)
                    .Item("denied_for").Value(result.Subject->GetName())
                    .Item("denied_by").Value(deniedBy);
            } else {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied for user %Qv: %Qlv permission for %v is not allowed by any matching ACE",
                    user->GetName(),
                    permission,
                    objectName);

                event
                    .Item("reason").Value(EAccessDeniedReason::NoAllowingAce);
            }
        }

        error.MutableAttributes()->Set("permission", permission);
        error.MutableAttributes()->Set("user", user->GetName());
        error.MutableAttributes()->Set("object_id", target.Object->GetId());
        if (target.Column) {
            error.MutableAttributes()->Set("object_column", target.Column);
        }
        error.MutableAttributes()->Set("object_type", target.Object->GetType());
        THROW_ERROR(error);
    }

    void ValidateResourceUsageIncrease(
        TAccount* account,
        const TClusterResources& delta,
        bool allowRootAccount)
    {
        if (IsPermissionValidationSuppressed()) {
            return;
        }

        const auto& objectManager = this->Bootstrap_->GetObjectManager();
        objectManager->ValidateObjectLifeStage(account);

        if (!allowRootAccount && account == GetRootAccount()) {
            THROW_ERROR_EXCEPTION("Root account cannot be used");
        }

        auto* initialAccount = account;

        const auto& dynamicConfig = GetDynamicConfig();
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto cellTag = multicellManager->GetCellTag();

        auto throwOverdraftError = [&] (
            const TString& resourceType,
            TAccount* overdrawnAccount,
            const auto& usage,
            const auto& increase,
            const auto& limit,
            const TMedium* medium = nullptr)
        {
            auto errorMessage = Format("%v %Qv is over %v limit%v%v",
                overdrawnAccount == initialAccount
                    ? "Account"
                    : overdrawnAccount == initialAccount->GetParent()
                        ? "Parent account"
                        : "Ancestor account",
                overdrawnAccount->GetName(),
                resourceType,
                medium
                    ? Format(" in medium %Qv", medium->GetName())
                    : TString(),
                overdrawnAccount != initialAccount
                    ? Format(" (while validating account %Qv)", initialAccount->GetName())
                    : TString());

            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::AccountLimitExceeded, errorMessage)
                << TErrorAttribute("usage", usage)
                << TErrorAttribute("increase", increase)
                << TErrorAttribute("limit", limit);
        };

        auto validateMasterMemoryIncrease = [&] (TAccount* account) {
            auto totalMasterMemoryDelta = delta.GetTotalMasterMemory();
            if (!dynamicConfig->EnableMasterMemoryUsageValidation || totalMasterMemoryDelta <= 0) {
                return;
            }
            const auto& limits = account->ClusterResourceLimits();
            const auto& multicellStatistics = account->MulticellStatistics();

            const auto& perCellLimit = limits.MasterMemory().PerCell;
            auto cellLimitsIt = perCellLimit.find(cellTag);
            auto multicellStatisticsIt = multicellStatistics.find(cellTag);
            if (cellLimitsIt != perCellLimit.end() && multicellStatisticsIt != multicellStatistics.end()) {
                auto cellTotalMasterMemory = multicellStatisticsIt->second.ResourceUsage.GetTotalMasterMemory();
                if (cellTotalMasterMemory + totalMasterMemoryDelta > cellLimitsIt->second) {
                    throwOverdraftError(Format("cell %v master memory", cellTag),
                        account,
                        cellTotalMasterMemory,
                        totalMasterMemoryDelta,
                        cellLimitsIt->second);
                }
            }

            if (IsChunkHostCell_) {
                auto chunkHostCellMasterMemory = account->GetChunkHostCellMasterMemoryUsage();
                if (chunkHostCellMasterMemory + totalMasterMemoryDelta > limits.MasterMemory().ChunkHost) {
                    throwOverdraftError(Format("chunk host master memory"),
                        account,
                        chunkHostCellMasterMemory,
                        totalMasterMemoryDelta,
                        limits.MasterMemory().ChunkHost);
                }
            }

            auto masterMemoryUsage = account->ClusterStatistics().ResourceUsage.GetTotalMasterMemory();
            if (masterMemoryUsage + totalMasterMemoryDelta > limits.MasterMemory().Total) {
                throwOverdraftError("master memory",
                    account,
                    masterMemoryUsage,
                    totalMasterMemoryDelta,
                    limits.MasterMemory().Total);
            }
        };

        for (; account; account = account->GetParent()) {
            const auto& usage = account->ClusterStatistics().ResourceUsage;
            const auto& committedUsage = account->ClusterStatistics().CommittedResourceUsage;
            const auto& limits = account->ClusterResourceLimits();

            for (auto [index, deltaSpace] : delta.DiskSpace()) {
                auto usageSpace = usage.DiskSpace().lookup(index);
                auto limitsSpace = limits.DiskSpace().lookup(index);

                if (usageSpace + deltaSpace > limitsSpace) {
                    const auto& chunkManager = Bootstrap_->GetChunkManager();
                    const auto* medium = chunkManager->GetMediumByIndex(index);

                    throwOverdraftError("disk space", account, usageSpace, deltaSpace, limitsSpace, medium);
                }
            }
            // Branched nodes are usually "paid for" by the originating node's
            // account, which is wrong, but can't be easily avoided. To mitigate the
            // issue, only committed node count is checked here. All this does is
            // effectively ignores non-trunk nodes, which constitute the majority of
            // problematic nodes.
            if (delta.GetNodeCount() > 0 && committedUsage.GetNodeCount() + delta.GetNodeCount() > limits.GetNodeCount()) {
                throwOverdraftError("Cypress node count", account, committedUsage.GetNodeCount(), delta.GetNodeCount(), limits.GetNodeCount());
            }
            if (delta.GetChunkCount() > 0 && usage.GetChunkCount() + delta.GetChunkCount() > limits.GetChunkCount()) {
                throwOverdraftError("chunk count", account, usage.GetChunkCount(), delta.GetChunkCount(), limits.GetChunkCount());
            }

            if (dynamicConfig->EnableTabletResourceValidation) {
                if (delta.GetTabletCount() > 0 && usage.GetTabletCount() + delta.GetTabletCount() > limits.GetTabletCount()) {
                    throwOverdraftError("tablet count", account, usage.GetTabletCount(), delta.GetTabletCount(), limits.GetTabletCount());
                }
                if (delta.GetTabletStaticMemory() > 0 && usage.GetTabletStaticMemory() + delta.GetTabletStaticMemory() > limits.GetTabletStaticMemory()) {
                    throwOverdraftError("tablet static memory", account, usage.GetTabletStaticMemory(), delta.GetTabletStaticMemory(), limits.GetTabletStaticMemory());
                }
            }

            validateMasterMemoryIncrease(account);
        }
    }

    void ValidateAttachChildAccount(TAccount* parentAccount, TAccount* childAccount)
    {
        const auto& childUsage = childAccount->ClusterStatistics().ResourceUsage;
        const auto& childLimits = childAccount->ClusterResourceLimits();

        const auto& parentLimits = parentAccount->ClusterResourceLimits();
        if (parentLimits.IsViolatedBy(childLimits)) {
            ThrowWithDetailedViolatedResources(
                parentLimits,
                childLimits,
                "Failed to change account %Qv parent to %Qv: "
                "child resource limit cannot be above that of its parent",
                childAccount->GetName(),
                parentAccount->GetName());
        }

        if (!parentAccount->GetAllowChildrenLimitOvercommit() && IsAccountOvercommitted(parentAccount, parentLimits - childLimits)) {
            ThrowAccountOvercommitted(
                parentAccount,
                parentLimits - childLimits,
                "Failed to change account %Qv parent to %Qv: "
                "the sum of children limits cannot be above parent limits",
                childAccount->GetName(),
                parentAccount->GetName());
        }

        ValidateResourceUsageIncrease(parentAccount, childUsage, true /*allowRootAccount*/);
    }

    void SetAccountAllowChildrenLimitOvercommit(
        TAccount* account,
        bool overcommitAllowed)
    {
        if (!overcommitAllowed && account->GetAllowChildrenLimitOvercommit() &&
            IsAccountOvercommitted(account, account->ClusterResourceLimits()))
        {
            ThrowAccountOvercommitted(
                account,
                account->ClusterResourceLimits(),
                "Failed to disable children limit overcommit for account %Qv because it is currently overcommitted",
                account->GetName());
        }

        account->SetAllowChildrenLimitOvercommit(overcommitAllowed);
    }

    // This is just a glorified for-each, but it's worth giving it a semantic name.
    template <class T>
    void ChargeAccountAncestry(TAccount* account, T&& chargeIndividualAccount)
    {
        for (; account; account = account->GetParent()) {
            chargeIndividualAccount(account);
        }
    }


    void SetUserBanned(TUser* user, bool banned)
    {
        if (banned && user == RootUser_) {
            THROW_ERROR_EXCEPTION("User %Qv cannot be banned",
                user->GetName());
        }

        if (user->GetBanned() != banned) {
            user->SetBanned(banned);
            if (banned) {
                YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "User is banned (User: %v)", user->GetName());
            } else {
                YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "User is no longer banned (User: %v)", user->GetName());
            }
        }
    }

    TError CheckUserAccess(TUser* user)
    {
        if (user->GetBanned()) {
            return TError(
                NSecurityClient::EErrorCode::UserBanned,
                "User %Qv is banned",
                user->GetName());
        }

        if (user == GetOwnerUser()) {
            return TError(
                NSecurityClient::EErrorCode::AuthenticationError,
                "Cannot authenticate as %Qv",
                user->GetName());
        }

        return {};
    }


    void ChargeUser(TUser* user, const TUserWorkload& workload)
    {
        Bootstrap_->VerifyPersistentStateRead();

        if (!IsObjectAlive(user)) {
            return;
        }
        RequestTracker_->ChargeUser(user, workload);
        UserCharged_.Fire(user, workload);
    }

    TFuture<void> ThrottleUser(TUser* user, int requestCount, EUserWorkloadType workloadType)
    {
        return RequestTracker_->ThrottleUserRequest(user, requestCount, workloadType);
    }

    void SetUserRequestRateLimit(TUser* user, int limit, EUserWorkloadType type)
    {
        RequestTracker_->SetUserRequestRateLimit(user, limit, type);
    }

    void SetUserRequestLimits(TUser* user, TUserRequestLimitsConfigPtr config)
    {
        RequestTracker_->SetUserRequestLimits(user, std::move(config));
    }

    void SetUserRequestQueueSizeLimit(TUser* user, int limit)
    {
        RequestTracker_->SetUserRequestQueueSizeLimit(user, limit);
    }


    bool TryIncreaseRequestQueueSize(TUser* user)
    {
        return RequestTracker_->TryIncreaseRequestQueueSize(user);
    }

    void DecreaseRequestQueueSize(TUser* user)
    {
        RequestTracker_->DecreaseRequestQueueSize(user);
    }


    const TSecurityTagsRegistryPtr& GetSecurityTagsRegistry() const
    {
        return SecurityTagsRegistry_;
    }

    void SetSubjectAliases(TSubject* subject, const std::vector<TString>& newAliases)
    {
        THashSet<TString> uniqAliases;
        for (const auto& newAlias : newAliases) {
            if (!subject->Aliases().contains(newAlias)) {
                // Check only if newAlias is not already used as subject alias
                ValidateSubjectName(newAlias);
            }
            if (uniqAliases.contains(newAlias)) {
                THROW_ERROR_EXCEPTION("Alias %Qv listed more than once for subject %Qv",
                    newAlias, subject->GetName());
            }
            uniqAliases.insert(newAlias);
        }
        auto& aliases = subject->Aliases();
        for (const auto& alias : aliases) {
            YT_VERIFY(SubjectAliasMap_.erase(alias) == 1);
        }
        aliases.clear();

        for (const auto& newAlias : newAliases) {
            aliases.insert(newAlias);
            YT_VERIFY(SubjectAliasMap_.emplace(newAlias, subject).second);
        }
    }

    TProfilerTagPtr GetUserCpuProfilerTag(TUser* user)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return *CpuProfilerTags_
            .FindOrInsert(
                user->GetName(),
                [&] { return New<TProfilerTag>("user", user->GetName()); })
            .first;
    }

    DEFINE_SIGNAL(void(TUser*, const TUserWorkload&), UserCharged);

private:
    friend class TAccountTypeHandler;
    friend class TAccountResourceUsageLeaseTypeHandler;
    friend class TUserTypeHandler;
    friend class TGroupTypeHandler;
    friend class TNetworkProjectTypeHandler;
    friend class TProxyRoleTypeHandler;

    const TRequestTrackerPtr RequestTracker_;

    const TSecurityTagsRegistryPtr SecurityTagsRegistry_ = New<TSecurityTagsRegistry>();

    TPeriodicExecutorPtr AccountStatisticsGossipExecutor_;
    TPeriodicExecutorPtr MembershipClosureRecomputeExecutor_;
    TPeriodicExecutorPtr AccountMasterMemoryUsageUpdateExecutor_;

    NHydra::TEntityMap<TAccount> AccountMap_;
    THashMap<TString, TAccount*> AccountNameMap_;

    NHydra::TEntityMap<TAccountResourceUsageLease> AccountResourceUsageLeaseMap_;

    TAccountId RootAccountId_;
    TAccount* RootAccount_ = nullptr;

    TAccountId SysAccountId_;
    TAccount* SysAccount_ = nullptr;

    TAccountId TmpAccountId_;
    TAccount* TmpAccount_ = nullptr;

    TAccountId IntermediateAccountId_;
    TAccount* IntermediateAccount_ = nullptr;

    TAccountId ChunkWiseAccountingMigrationAccountId_;
    TAccount* ChunkWiseAccountingMigrationAccount_ = nullptr;

    NHydra::TEntityMap<TUser> UserMap_;
    THashMap<TString, TUser*> UserNameMap_;

    TUserId RootUserId_;
    TUser* RootUser_ = nullptr;

    TUserId GuestUserId_;
    TUser* GuestUser_ = nullptr;

    TUserId JobUserId_;
    TUser* JobUser_ = nullptr;

    TUserId SchedulerUserId_;
    TUser* SchedulerUser_ = nullptr;

    TUserId ReplicatorUserId_;
    TUser* ReplicatorUser_ = nullptr;

    TUserId OwnerUserId_;
    TUser* OwnerUser_ = nullptr;

    TUserId FileCacheUserId_;
    TUser* FileCacheUser_ = nullptr;

    TUserId OperationsCleanerUserId_;
    TUser* OperationsCleanerUser_ = nullptr;

    TUserId OperationsClientUserId_;
    TUser* OperationsClientUser_ = nullptr;

    TUserId TabletCellChangeloggerUserId_;
    TUser* TabletCellChangeloggerUser_ = nullptr;

    TUserId TabletCellSnapshotterUserId_;
    TUser* TabletCellSnapshotterUser_ = nullptr;

    TUserId TableMountInformerUserId_;
    TUser* TableMountInformerUser_ = nullptr;

    TUserId AlienCellSynchronizerUserId_;
    TUser* AlienCellSynchronizerUser_ = nullptr;

    TUserId QueueAgentUserId_;
    TUser* QueueAgentUser_ = nullptr;

    TUserId TabletBalancerUserId_;
    TUser* TabletBalancerUser_ = nullptr;

    NHydra::TEntityMap<TGroup> GroupMap_;
    THashMap<TString, TGroup*> GroupNameMap_;

    THashMap<TString, TSubject*> SubjectAliasMap_;

    TGroupId EveryoneGroupId_;
    TGroup* EveryoneGroup_ = nullptr;

    TGroupId UsersGroupId_;
    TGroup* UsersGroup_ = nullptr;

    TGroupId SuperusersGroupId_;
    TGroup* SuperusersGroup_ = nullptr;

    TFls<TUser*> AuthenticatedUser_;

    NHydra::TEntityMap<TNetworkProject> NetworkProjectMap_;
    THashMap<TString, TNetworkProject*> NetworkProjectNameMap_;

    NHydra::TEntityMap<TProxyRole> ProxyRoleMap_;
    TEnumIndexedVector<EProxyKind, THashMap<TString, TProxyRole*>> ProxyRoleNameMaps_;

    TSyncMap<TString, TProfilerTagPtr> CpuProfilerTags_;

    bool IsChunkHostCell_ = false;

    // COMPAT(shakurov)
    bool RecomputeAccountResourceUsage_ = false;
    bool ValidateAccountResourceUsage_ = false;

    bool MustRecomputeMembershipClosure_ = false;

    // COMPAT(h0pless)
    bool RecomputeChunkHostCellMasterMemory_ = false;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    static i64 GetDiskSpaceToCharge(i64 diskSpace, NErasure::ECodec erasureCodec, TReplicationPolicy policy)
    {
        auto isErasure = erasureCodec != NErasure::ECodec::None;
        auto replicationFactor = isErasure ? 1 : policy.GetReplicationFactor();
        auto result = diskSpace *  replicationFactor;

        if (policy.GetDataPartsOnly() && isErasure) {
            auto* codec = NErasure::GetCodec(erasureCodec);
            auto dataPartCount = codec->GetDataPartCount();
            auto totalPartCount = codec->GetTotalPartCount();

            // Should only charge for data parts.
            result = result * dataPartCount / totalPartCount;
        }

        return result;
    }

    TClusterResources* GetTransactionAccountUsage(TTransaction* transaction, TAccount* account)
    {
        auto it = transaction->AccountResourceUsage().find(account);
        if (it == transaction->AccountResourceUsage().end()) {
            it = transaction->AccountResourceUsage().emplace(account, TClusterResources()).first;
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->RefObject(account);
        }
        return &it->second;
    }

    template <class T>
    void ComputeChunkResourceDelta(const TChunk* chunk, const TChunkRequisition& requisition, i64 delta, T&& doCharge)
    {
        auto chunkDiskSpace = chunk->GetDiskSpace();
        auto erasureCodec = chunk->GetErasureCodec();

        const TAccount* lastAccount = nullptr;
        auto lastMediumIndex = GenericMediumIndex;
        i64 lastDiskSpace = 0;
        auto masterMemoryUsageDelta = delta * chunk->GetMasterMemoryUsage();

        for (const auto& entry : requisition) {
            auto* account = entry.Account;
            if (!IsObjectAlive(account)) {
                continue;
            }

            auto mediumIndex = entry.MediumIndex;
            YT_ASSERT(mediumIndex != NChunkClient::GenericMediumIndex);

            auto policy = entry.ReplicationPolicy;
            auto diskSpace = delta * GetDiskSpaceToCharge(chunkDiskSpace, erasureCodec, policy);

            auto chunkCount = delta;
            auto masterMemoryUsage = masterMemoryUsageDelta;
            // Сharge once per account.
            if (account == lastAccount) {
                chunkCount = 0;
                masterMemoryUsage = 0;
            }

            if (account == lastAccount && mediumIndex == lastMediumIndex) {
                // TChunkRequisition keeps entries sorted, which means an
                // uncommitted entry for account A and medium M, if any,
                // immediately follows a committed entry for A and M (if any).
                YT_VERIFY(!entry.Committed);

                // Avoid overcharging: if, for example, a chunk has 3 'committed' and
                // 5 'uncommitted' replicas (for the same account and medium), the account
                // has already been charged for 3 and should now be charged for 2 only.
                if (delta > 0) {
                    diskSpace = std::max(i64(0), diskSpace - lastDiskSpace);
                } else {
                    diskSpace = std::min(i64(0), diskSpace - lastDiskSpace);
                }
            }

            ChargeAccountAncestry(
                account,
                [&] (TAccount* account) {
                    doCharge(account, mediumIndex, chunkCount, diskSpace, masterMemoryUsage, entry.Committed);
                });

            lastAccount = account;
            lastMediumIndex = mediumIndex;
            lastDiskSpace = diskSpace;
        }
    }


    TAccount* DoCreateAccount(TAccountId id)
    {
        auto accountHolder = TPoolAllocator::New<TAccount>(id, id == RootAccountId_);

        auto* account = AccountMap_.Insert(id, std::move(accountHolder));

        InitializeAccountStatistics(account);

        // Make the fake reference.
        YT_VERIFY(account->RefObject() == 1);

        return account;
    }

    TGroup* GetBuiltinGroupForUser(TUser* user)
    {
        // "guest" is a member of "everyone" group.
        // "root", "job", "scheduler", "replicator", "file_cache", "operations_cleaner", "operations_client",
        // "tablet_cell_changelogger", "tablet_cell_snapshotter" and "table_mount_informer" are members of "superusers" group.
        // others are members of "users" group.
        const auto& id = user->GetId();
        if (id == GuestUserId_) {
            return EveryoneGroup_;
        } else if (
            id == RootUserId_ ||
            id == JobUserId_ ||
            id == SchedulerUserId_ ||
            id == ReplicatorUserId_ ||
            id == FileCacheUserId_ ||
            id == OperationsCleanerUserId_ ||
            id == OperationsClientUserId_ ||
            id == TabletCellChangeloggerUserId_ ||
            id == TabletCellSnapshotterUserId_ ||
            id == TableMountInformerUserId_ ||
            id == AlienCellSynchronizerUserId_ ||
            id == QueueAgentUserId_ ||
            id == TabletBalancerUserId_)
        {
            return SuperusersGroup_;
        } else {
            return UsersGroup_;
        }
    }

    TUser* DoCreateUser(TUserId id, const TString& name)
    {
        auto userHolder = TPoolAllocator::New<TUser>(id);
        userHolder->SetName(name);

        auto* user = UserMap_.Insert(id, std::move(userHolder));
        YT_VERIFY(UserNameMap_.emplace(user->GetName(), user).second);

        YT_VERIFY(user->RefObject() == 1);
        DoAddMember(GetBuiltinGroupForUser(user), user);
        MaybeRecomputeMembershipClosure();

        if (!IsRecovery()) {
            RequestTracker_->ReconfigureUserRequestRateThrottlers(user);
        }

        return user;
    }

    TGroup* DoCreateGroup(TGroupId id, const TString& name)
    {
        auto groupHolder = TPoolAllocator::New<TGroup>(id);
        groupHolder->SetName(name);

        auto* group = GroupMap_.Insert(id, std::move(groupHolder));
        YT_VERIFY(GroupNameMap_.emplace(group->GetName(), group).second);

        // Make the fake reference.
        YT_VERIFY(group->RefObject() == 1);

        return group;
    }

    TNetworkProject* DoCreateNetworkProject(TNetworkProjectId id, const TString& name)
    {
        auto networkProjectHolder = TPoolAllocator::New<TNetworkProject>(id);
        networkProjectHolder->SetName(name);

        auto* networkProject = NetworkProjectMap_.Insert(id, std::move(networkProjectHolder));
        YT_VERIFY(NetworkProjectNameMap_.emplace(networkProject->GetName(), networkProject).second);

        // Make the fake reference.
        YT_VERIFY(networkProject->RefObject() == 1);

        return networkProject;
    }

    TProxyRole* DoCreateProxyRole(TProxyRoleId id, const TString& name, EProxyKind proxyKind)
    {
        auto proxyRoleHolder = TPoolAllocator::New<TProxyRole>(id);
        proxyRoleHolder->SetName(name);
        proxyRoleHolder->SetProxyKind(proxyKind);

        auto proxyRole = ProxyRoleMap_.Insert(id, std::move(proxyRoleHolder));
        YT_VERIFY(ProxyRoleNameMaps_[proxyKind].emplace(name, proxyRole).second);

        // Make the fake reference.
        YT_VERIFY(proxyRole->RefObject() == 1);

        return proxyRole;
    }

    void PropagateRecursiveMemberOf(TSubject* subject, TGroup* ancestorGroup)
    {
        bool added = subject->RecursiveMemberOf().insert(ancestorGroup).second;
        if (added && subject->GetType() == EObjectType::Group) {
            auto* subjectGroup = subject->AsGroup();
            for (auto* member : subjectGroup->Members()) {
                PropagateRecursiveMemberOf(member, ancestorGroup);
            }
        }
    }

    void MaybeRecomputeMembershipClosure()
    {
        const auto& dynamicConfig = GetDynamicConfig();
        if (dynamicConfig->EnableDelayedMembershipClosureRecomputation) {
            if (!MustRecomputeMembershipClosure_) {
                MustRecomputeMembershipClosure_ = true;
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Will recompute membership closure");
            }
        } else {
            DoRecomputeMembershipClosure();
        }
    }

    void DoRecomputeMembershipClosure()
    {
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Started recomputing membership closure");

        for (auto [userId, user] : UserMap_) {
            user->RecursiveMemberOf().clear();
        }

        for (auto [groupId, group] : GroupMap_) {
            group->RecursiveMemberOf().clear();
        }

        for (auto [groupId, group] : GroupMap_) {
            for (auto* member : group->Members()) {
                PropagateRecursiveMemberOf(member, group);
            }
        }

        MustRecomputeMembershipClosure_ = false;

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Finished recomputing membership closure");
    }

    void OnRecomputeMembershipClosure()
    {
        NProto::TReqRecomputeMembershipClosure request;
        CreateMutation(Bootstrap_->GetHydraFacade()->GetHydraManager(), request)
            ->CommitAndLog(Logger);
    }


    void DoAddMember(TGroup* group, TSubject* member)
    {
        YT_VERIFY(group->Members().insert(member).second);
        YT_VERIFY(member->MemberOf().insert(group).second);
    }

    void DoRemoveMember(TGroup* group, TSubject* member)
    {
        YT_VERIFY(group->Members().erase(member) == 1);
        YT_VERIFY(member->MemberOf().erase(group) == 1);
    }


    void ValidateMembershipUpdate(TGroup* group, TSubject* /*member*/)
    {
        if (group == EveryoneGroup_ || group == UsersGroup_) {
            THROW_ERROR_EXCEPTION("Cannot modify group");
        }

        ValidatePermission(group, EPermission::Write);
    }


    void SaveKeys(NCellMaster::TSaveContext& context) const
    {
        AccountMap_.SaveKeys(context);
        UserMap_.SaveKeys(context);
        GroupMap_.SaveKeys(context);
        NetworkProjectMap_.SaveKeys(context);
        ProxyRoleMap_.SaveKeys(context);
        AccountResourceUsageLeaseMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        AccountMap_.SaveValues(context);
        UserMap_.SaveValues(context);
        GroupMap_.SaveValues(context);
        NetworkProjectMap_.SaveValues(context);
        Save(context, MustRecomputeMembershipClosure_);
        ProxyRoleMap_.SaveValues(context);
        AccountResourceUsageLeaseMap_.SaveValues(context);
        Save(context, IsChunkHostCell_);
    }


    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        AccountMap_.LoadKeys(context);
        UserMap_.LoadKeys(context);
        GroupMap_.LoadKeys(context);
        NetworkProjectMap_.LoadKeys(context);
        ProxyRoleMap_.LoadKeys(context);
        AccountResourceUsageLeaseMap_.LoadKeys(context);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        using NYT::Load;

        AccountMap_.LoadValues(context);
        UserMap_.LoadValues(context);
        GroupMap_.LoadValues(context);
        NetworkProjectMap_.LoadValues(context);
        MustRecomputeMembershipClosure_ = Load<bool>(context);

        // COMPAT(savrus) COMPAT(shakurov)
        ValidateAccountResourceUsage_ = true;

        // COMPAT(h0pless)
        RecomputeChunkHostCellMasterMemory_ = context.GetVersion() < EMasterReign::AccountGossipStatisticsOptimization;

        ProxyRoleMap_.LoadValues(context);
        AccountResourceUsageLeaseMap_.LoadValues(context);

        // COMPAT(h0pless)
        if (context.GetVersion() >= NCellMaster::EMasterReign::AccountGossipStatisticsOptimization) {
            Load(context, IsChunkHostCell_);
        }
    }

    void OnAfterSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        AccountNameMap_.clear();
        for (auto [accountId, account] : AccountMap_) {
            // Initialize statistics for this cell.
            // NB: This also provides the necessary data migration for pre-0.18 versions.
            InitializeAccountStatistics(account);

            if (!IsObjectAlive(account)) {
                continue;
            }

            if (!account->GetParent() && account->GetId() != RootAccountId_) {
                YT_LOG_ALERT("Unattended account found in snapshot (Id: %v)",
                    account->GetId());
            } else {
                // Reconstruct account name map.
                RegisterAccountName(account->GetName(), account);
            }
        }

        UserNameMap_.clear();
        for (auto [userId, user] : UserMap_) {
            if (!IsObjectAlive(user)) {
                continue;
            }

            // Reconstruct user name map.
            YT_VERIFY(UserNameMap_.emplace(user->GetName(), user).second);
            // Add group aliases to SubjectAliasMap
            for (const auto& alias : user->Aliases()) {
                YT_VERIFY(SubjectAliasMap_.emplace(alias, user).second);
            }
        }

        GroupNameMap_.clear();
        for (auto [groupId, group] : GroupMap_) {
            if (!IsObjectAlive(group)) {
                continue;
            }
            // Reconstruct group name map.
            YT_VERIFY(GroupNameMap_.emplace(group->GetName(), group).second);
            // Add group aliases to SubjectAliasMap
            for (const auto& alias : group->Aliases()) {
                YT_VERIFY(SubjectAliasMap_.emplace(alias, group).second);
            }
        }

        NetworkProjectNameMap_.clear();
        for (auto [networkProjectId, networkProject] : NetworkProjectMap_) {
            if (!IsObjectAlive(networkProject)) {
                continue;
            }

            // Reconstruct network project name map.
            YT_VERIFY(NetworkProjectNameMap_.emplace(networkProject->GetName(), networkProject).second);
        }

        for (auto proxyKind : TEnumTraits<EProxyKind>::GetDomainValues()) {
            ProxyRoleNameMaps_[proxyKind].clear();
        }
        for (auto [proxyRoleId, proxyRole] : ProxyRoleMap_) {
            if (!IsObjectAlive(proxyRole)) {
                continue;
            }

            // Reconstruct proxy role name maps.
            auto name = proxyRole->GetName();
            auto proxyKind = proxyRole->GetProxyKind();
            YT_VERIFY(ProxyRoleNameMaps_[proxyKind].emplace(name, proxyRole).second);
        }

        InitBuiltins();

        RecomputeAccountResourceUsage();
        RecomputeAccountMasterMemoryUsage();
        RecomputeSubtreeSize(RootAccount_, /*validateMatch*/ true);

        if (RecomputeChunkHostCellMasterMemory_) {
            RecomputeChunkHostCellMasterMemory();
        }
    }

    void RecomputeChunkHostCellMasterMemory() {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto cellTag = multicellManager->GetCellTag();
        auto roles = multicellManager->GetMasterCellRoles(cellTag);
        IsChunkHostCell_ = Any(roles & EMasterCellRoles::ChunkHost);
        if (!IsChunkHostCell_) {
            return;
        }

        for (auto [accountId, account] : AccountMap_) {
            if (!IsObjectAlive(account)) {
                continue;
            }

            auto& resourceUsage = account->LocalStatistics().ResourceUsage;
            auto& committedResourceUsage = account->LocalStatistics().CommittedResourceUsage;
            resourceUsage.SetChunkHostCellMasterMemory(resourceUsage.DetailedMasterMemory().GetTotal());
            committedResourceUsage.SetChunkHostCellMasterMemory(committedResourceUsage.DetailedMasterMemory().GetTotal());
        }
    }

    void RecomputeAccountResourceUsage()
    {
        if (!ValidateAccountResourceUsage_ && !RecomputeAccountResourceUsage_) {
            return;
        }

        struct TStat
        {
            TClusterResources NodeUsage;
            TClusterResources NodeCommittedUsage;
        };

        THashMap<TAccount*, TStat> statMap;

        const auto& cypressManager = Bootstrap_->GetCypressManager();

        // Recompute everything except:
        //   - chunk count and disk space as they're recomputed below on chunk-by-chunk basis;
        //   - master memory usage as it's always recomputed after loading.
        for (auto [nodeId, node] : cypressManager->Nodes()) {
            // NB: zombie nodes are still accounted.
            if (node->IsGhost()) {
                continue;
            }

            if (node->IsExternal()) {
                continue;
            }

            auto* account = node->GetAccount();

            auto usage = node->GetDeltaResourceUsage();
            auto tabletResourceUsage = node->GetTabletResourceUsage();
            usage.SetTabletCount(tabletResourceUsage.TabletCount);
            usage.SetTabletStaticMemory(tabletResourceUsage.TabletStaticMemory);
            usage.DetailedMasterMemory() = TDetailedMasterMemory();
            usage.SetChunkCount(0);
            usage.ClearDiskSpace();

            auto isTrunkNode = node->IsTrunk();

            ChargeAccountAncestry(
                account,
                [&] (TAccount* account) {
                    auto& stat = statMap[account];
                    stat.NodeUsage += usage;
                    if (isTrunkNode) {
                        stat.NodeCommittedUsage += usage;
                    }
                });
        }

        auto chargeStatMap = [&] (TAccount* account, int mediumIndex, i64 chunkCount, i64 diskSpace, i64 /*masterMemoryUsage*/, bool committed) {
            auto& stat = statMap[account];
            stat.NodeUsage.AddToMediumDiskSpace(mediumIndex, diskSpace);
            stat.NodeUsage.SetChunkCount(stat.NodeUsage.GetChunkCount() + chunkCount);
            if (committed) {
                stat.NodeCommittedUsage.AddToMediumDiskSpace(mediumIndex, diskSpace);
                stat.NodeCommittedUsage.SetChunkCount(stat.NodeCommittedUsage.GetChunkCount() + chunkCount);
            }
        };

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto* requisitionRegistry = chunkManager->GetChunkRequisitionRegistry();

        for (auto [chunkId, chunk] : chunkManager->Chunks()) {
            // NB: zombie chunks are still accounted.
            if (chunk->IsGhost()) {
                continue;
            }

            if (chunk->IsForeign()) {
                continue;
            }

            if (chunk->IsDiskSizeFinal()) {
                auto requisition = chunk->GetAggregatedRequisition(requisitionRegistry);
                ComputeChunkResourceDelta(chunk, requisition, +1, chargeStatMap);
            }  // Else this'll be done later when the chunk is confirmed/sealed.
        }

        auto resourceUsageMatch = [&] (
            TAccount* account,
            const TClusterResources& accountUsage,
            const TClusterResources& expectedUsage)
        {
            auto accountUsageCopy = accountUsage;
            // Ignore master memory as it's always recomputed anyway.
            accountUsageCopy.DetailedMasterMemory() = TDetailedMasterMemory();

            if (accountUsageCopy == expectedUsage) {
                return true;
            }
            if (account != SysAccount_) {
                return false;
            }

            // Root node requires special handling (unless resource usage have previously been recomputed).
            accountUsageCopy.SetNodeCount(accountUsageCopy.GetNodeCount() + 1);
            return accountUsageCopy == expectedUsage;
        };

        for (auto [accountId, account] : Accounts()) {
            if (!IsObjectAlive(account)) {
                continue;
            }

            // NB: statMap may contain no entry for an account if it has no nodes or chunks.
            const auto& stat = statMap[account];
            auto& actualUsage = account->LocalStatistics().ResourceUsage;
            auto& actualCommittedUsage = account->LocalStatistics().CommittedResourceUsage;
            const auto& expectedUsage = stat.NodeUsage;
            const auto& expectedCommittedUsage = stat.NodeCommittedUsage;
            if (ValidateAccountResourceUsage_) {
                if (!resourceUsageMatch(account, actualUsage, expectedUsage)) {
                    YT_LOG_ERROR("%v account usage mismatch, snapshot usage: %v, recomputed usage: %v",
                        account->GetName(),
                        actualUsage,
                        expectedUsage);
                }
                if (!resourceUsageMatch(account, actualCommittedUsage, expectedCommittedUsage)) {
                    YT_LOG_ERROR("%v account committed usage mismatch, snapshot usage: %v, recomputed usage: %v",
                        account->GetName(),
                        actualCommittedUsage,
                        expectedCommittedUsage);
                }
            }
            if (RecomputeAccountResourceUsage_) {;
                actualUsage = expectedUsage;
                actualCommittedUsage = expectedCommittedUsage;

                const auto& multicellManager = Bootstrap_->GetMulticellManager();
                if (multicellManager->IsPrimaryMaster()) {
                    account->RecomputeClusterStatistics();
                }
            }
        }
    }

    void RecomputeAccountMasterMemoryUsage()
    {
        YT_LOG_INFO("Started recomputing account master memory usage");

        for (auto [id, account] : AccountMap_) {
            if (!IsObjectAlive(account)) {
                continue;
            }
            account->DetailedMasterMemoryUsage() = {};
        }

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        for (auto [nodeId, node] : cypressManager->Nodes()) {
            if (node->IsGhost()) {
                continue;
            }

            if (IsTableType(node->GetType()) && node->IsTrunk()) {
                auto* table = node->As<TTableNode>();
                table->RecomputeTabletMasterMemoryUsage();
            }
            UpdateMasterMemoryUsage(node, /*accountChanged*/ true);
        }

        auto chargeAccount = [&] (TAccount* account, int /*mediumIndex*/, i64 /*chunkCount*/, i64 /*diskSpace*/, i64 masterMemoryUsage, bool /*committed*/) {
            account->DetailedMasterMemoryUsage()[EMasterMemoryType::Chunks] += masterMemoryUsage;
        };

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto* requisitionRegistry = chunkManager->GetChunkRequisitionRegistry();

        for (auto [chunkId, chunk] : chunkManager->Chunks()) {
            // NB: zombie chunks are still accounted.
            if (chunk->IsGhost()) {
                continue;
            }

            if (chunk->IsForeign()) {
                continue;
            }

            if (!chunk->IsDiskSizeFinal()) {
                continue;
            }

            auto requisition = chunk->GetAggregatedRequisition(requisitionRegistry);
            ComputeChunkResourceDelta(chunk, requisition, +1, chargeAccount);
        }

        YT_LOG_INFO("Finished recomputing account master memory usage");
    }

    void Clear() override
    {
        TMasterAutomatonPart::Clear();

        AccountMap_.Clear();
        AccountNameMap_.clear();

        AccountResourceUsageLeaseMap_.Clear();

        UserMap_.Clear();
        UserNameMap_.clear();

        GroupMap_.Clear();
        GroupNameMap_.clear();
        SubjectAliasMap_.clear();

        NetworkProjectMap_.Clear();
        NetworkProjectNameMap_.clear();

        ProxyRoleMap_.Clear();
        for (auto proxyKind : TEnumTraits<EProxyKind>::GetDomainValues()) {
            ProxyRoleNameMaps_[proxyKind].clear();
        }

        RootUser_ = nullptr;
        GuestUser_ = nullptr;
        JobUser_ = nullptr;
        SchedulerUser_ = nullptr;
        OperationsCleanerUser_ = nullptr;
        OperationsClientUser_ = nullptr;
        TabletCellChangeloggerUser_ = nullptr;
        TabletCellSnapshotterUser_ = nullptr;
        TableMountInformerUser_ = nullptr;
        AlienCellSynchronizerUser_ = nullptr;
        QueueAgentUser_ = nullptr;
        TabletBalancerUser_ = nullptr;
        ReplicatorUser_ = nullptr;
        OwnerUser_ = nullptr;
        FileCacheUser_ = nullptr;
        EveryoneGroup_ = nullptr;
        UsersGroup_ = nullptr;
        SuperusersGroup_ = nullptr;

        RootAccount_ = nullptr;
        SysAccount_ = nullptr;
        TmpAccount_ = nullptr;
        IntermediateAccount_ = nullptr;
        ChunkWiseAccountingMigrationAccount_ = nullptr;

        MustRecomputeMembershipClosure_ = false;

        ResetAuthenticatedUser();
    }

    void SetZeroState() override
    {
        TMasterAutomatonPart::SetZeroState();

        InitBuiltins();
        InitDefaultSchemaAcds();
    }

    void InitDefaultSchemaAcds()
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        for (auto type : objectManager->GetRegisteredTypes()) {
            if (HasSchema(type)) {
                auto* schema = objectManager->GetSchema(type);
                auto* acd = GetAcd(schema);
                // TODO(renadeen): giving read, write, remove for object schema to all users looks like a bad idea.
                if (!IsVersionedType(type)) {
                    acd->AddEntry(TAccessControlEntry(
                        ESecurityAction::Allow,
                        GetUsersGroup(),
                        EPermission::Remove));
                    acd->AddEntry(TAccessControlEntry(
                        ESecurityAction::Allow,
                        GetUsersGroup(),
                        EPermission::Write));
                    acd->AddEntry(TAccessControlEntry(
                        ESecurityAction::Allow,
                        GetEveryoneGroup(),
                        EPermission::Read));
                }
                if (IsUserType(type)) {
                    acd->AddEntry(TAccessControlEntry(
                        ESecurityAction::Allow,
                        GetUsersGroup(),
                        EPermission::Create));
                }
            }
        }
    }

    template <class T>
    T* GetBuiltin(T*& builtin)
    {
        if (!builtin) {
            InitBuiltins();
        }
        YT_VERIFY(builtin);
        return builtin;
    }

    void InitBuiltins()
    {
        // Groups

        // users
        EnsureBuiltinGroupInitialized(UsersGroup_, UsersGroupId_, UsersGroupName);

        // everyone
        if (EnsureBuiltinGroupInitialized(EveryoneGroup_, EveryoneGroupId_, EveryoneGroupName)) {
            DoAddMember(EveryoneGroup_, UsersGroup_);
        }

        // superusers
        if (EnsureBuiltinGroupInitialized(SuperusersGroup_, SuperusersGroupId_, SuperusersGroupName)) {
            DoAddMember(UsersGroup_, SuperusersGroup_);
        }

        DoRecomputeMembershipClosure();

        // Users

        // root
        if (EnsureBuiltinUserInitialized(RootUser_, RootUserId_, RootUserName)) {
            RootUser_->SetRequestRateLimit(std::nullopt, EUserWorkloadType::Read);
            RootUser_->SetRequestRateLimit(std::nullopt, EUserWorkloadType::Write);
            RootUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // guest
        EnsureBuiltinUserInitialized(GuestUser_, GuestUserId_, GuestUserName);

        if (EnsureBuiltinUserInitialized(JobUser_, JobUserId_, JobUserName)) {
            // job
            JobUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            JobUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            JobUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // scheduler
        if (EnsureBuiltinUserInitialized(SchedulerUser_, SchedulerUserId_, SchedulerUserName)) {
            SchedulerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            SchedulerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            SchedulerUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // replicator
        if (EnsureBuiltinUserInitialized(ReplicatorUser_, ReplicatorUserId_, ReplicatorUserName)) {
            ReplicatorUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            ReplicatorUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            ReplicatorUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // owner
        EnsureBuiltinUserInitialized(OwnerUser_, OwnerUserId_, OwnerUserName);

        // file cache
        if (EnsureBuiltinUserInitialized(FileCacheUser_, FileCacheUserId_, FileCacheUserName)) {
            FileCacheUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            FileCacheUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            FileCacheUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // operations cleaner
        if (EnsureBuiltinUserInitialized(OperationsCleanerUser_, OperationsCleanerUserId_, OperationsCleanerUserName)) {
            OperationsCleanerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            OperationsCleanerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            OperationsCleanerUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // operations client
        if (EnsureBuiltinUserInitialized(OperationsClientUser_, OperationsClientUserId_, OperationsClientUserName)) {
            OperationsClientUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            OperationsClientUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            OperationsClientUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // tablet cell changelogger
        if (EnsureBuiltinUserInitialized(TabletCellChangeloggerUser_, TabletCellChangeloggerUserId_, TabletCellChangeloggerUserName)) {
            TabletCellChangeloggerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            TabletCellChangeloggerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            TabletCellChangeloggerUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // tablet cell snapshotter
        if (EnsureBuiltinUserInitialized(TabletCellSnapshotterUser_, TabletCellSnapshotterUserId_, TabletCellSnapshotterUserName)) {
            TabletCellSnapshotterUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            TabletCellSnapshotterUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            TabletCellSnapshotterUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // table mount informer
        if (EnsureBuiltinUserInitialized(TableMountInformerUser_, TableMountInformerUserId_, TableMountInformerUserName)) {
            TableMountInformerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Read);
            TableMountInformerUser_->SetRequestRateLimit(1'000'000, EUserWorkloadType::Write);
            TableMountInformerUser_->SetRequestQueueSizeLimit(1'000'000);
        }

        // alien cell synchronizer
        EnsureBuiltinUserInitialized(AlienCellSynchronizerUser_, AlienCellSynchronizerUserId_, AlienCellSynchronizerUserName);

        // queue agent
        EnsureBuiltinUserInitialized(QueueAgentUser_, QueueAgentUserId_, QueueAgentUserName);

        // tablet balancer
        EnsureBuiltinUserInitialized(TabletBalancerUser_, TabletBalancerUserId_, TabletBalancerUserName);

        // Accounts
        // root, infinite resources, not meant to be used
        if (EnsureBuiltinAccountInitialized(RootAccount_, RootAccountId_, RootAccountName)) {
            RootAccount_->ClusterResourceLimits() = TClusterResourceLimits::Infinite();
        }
        RootAccount_->SetAllowChildrenLimitOvercommit(true);

        auto defaultResources = TClusterResourceLimits()
            .SetNodeCount(100'000)
            .SetChunkCount(1'000'000'000)
            .SetMediumDiskSpace(NChunkServer::DefaultStoreMediumIndex, 1_TB)
            .SetMasterMemory({100_GB, 100_GB, {}});

        // sys, 1 TB disk space, 100 000 nodes, 1 000 000 000 chunks, 100 000 tablets, 10TB tablet static memory, 100_GB master memory allowed for: root
        if (EnsureBuiltinAccountInitialized(SysAccount_, SysAccountId_, SysAccountName)) {
            auto resourceLimits = defaultResources;
            resourceLimits.SetTabletCount(100'000);
            resourceLimits.SetTabletStaticMemory(10_TB);
            TrySetResourceLimits(SysAccount_, resourceLimits);

            SysAccount_->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                RootUser_,
                EPermission::Use));
        }

        // tmp, 1 TB disk space, 100 000 nodes, 1 000 000 000 chunks, 100_GB master memory, allowed for: users
        if (EnsureBuiltinAccountInitialized(TmpAccount_, TmpAccountId_, TmpAccountName)) {
            TrySetResourceLimits(TmpAccount_, defaultResources);
            TmpAccount_->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                UsersGroup_,
                EPermission::Use));
        }

        // intermediate, 1 TB disk space, 100 000 nodes, 1 000 000 000 chunks, 100_GB master memory allowed for: users
        if (EnsureBuiltinAccountInitialized(IntermediateAccount_, IntermediateAccountId_, IntermediateAccountName)) {
            TrySetResourceLimits(IntermediateAccount_, defaultResources);
            IntermediateAccount_->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                UsersGroup_,
                EPermission::Use));
        }

        // chunk_wise_accounting_migration, maximum disk space, maximum nodes, maximum chunks, 100_GB master memory allowed for: root
        if (EnsureBuiltinAccountInitialized(ChunkWiseAccountingMigrationAccount_, ChunkWiseAccountingMigrationAccountId_, ChunkWiseAccountingMigrationAccountName)) {
            auto resourceLimits = TClusterResourceLimits()
                .SetNodeCount(std::numeric_limits<int>::max())
                .SetChunkCount(std::numeric_limits<int>::max())
                .SetMasterMemory({100_GB, 100_GB, {}})
                .SetMediumDiskSpace(NChunkServer::DefaultStoreMediumIndex, std::numeric_limits<i64>::max() / 4);
            TrySetResourceLimits(ChunkWiseAccountingMigrationAccount_, resourceLimits);
            ChunkWiseAccountingMigrationAccount_->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                RootUser_,
                EPermission::Use));
        }

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* requisitionRegistry = chunkManager->GetChunkRequisitionRegistry();
        requisitionRegistry->EnsureBuiltinRequisitionsInitialized(
            GetChunkWiseAccountingMigrationAccount(),
            Bootstrap_->GetObjectManager());
    }

    bool EnsureBuiltinGroupInitialized(TGroup*& group, TGroupId id, const TString& name)
    {
        if (group) {
            return false;
        }
        group = FindGroup(id);
        if (group) {
            return false;
        }
        group = DoCreateGroup(id, name);
        return true;
    }

    bool EnsureBuiltinUserInitialized(TUser*& user, TUserId id, const TString& name)
    {
        if (user) {
            return false;
        }
        user = FindUser(id);
        if (user) {
            return false;
        }
        user = DoCreateUser(id, name);
        return true;
    }

    bool EnsureBuiltinAccountInitialized(TAccount*& account, TAccountId id, const TString& name)
    {
        if (account) {
            return false;
        }
        account = FindAccount(id);
        if (account) {
            return false;
        }

        account = DoCreateAccount(id);

        if (id != RootAccountId_) {
            YT_VERIFY(RootAccount_);
            RootAccount_->AttachChild(name, account);
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->RefObject(RootAccount_);
        }
        RegisterAccountName(name, account);

        return true;
    }


    void OnRecoveryComplete() override
    {
        TMasterAutomatonPart::OnRecoveryComplete();

        RequestTracker_->Start();
    }

    void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnLeaderActive();

        AccountStatisticsGossipExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::SecurityGossip),
            BIND(&TImpl::OnAccountStatisticsGossip, MakeWeak(this)));
        AccountStatisticsGossipExecutor_->Start();

        MembershipClosureRecomputeExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::Periodic),
            BIND(&TImpl::OnRecomputeMembershipClosure, MakeWeak(this)));
        MembershipClosureRecomputeExecutor_->Start();

        AccountMasterMemoryUsageUpdateExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::SecurityManager),
            BIND(&TImpl::CommitAccountMasterMemoryUsage, MakeWeak(this)));
        AccountMasterMemoryUsageUpdateExecutor_->Start();
    }

    void OnStopLeading() override
    {
        TMasterAutomatonPart::OnStopLeading();

        RequestTracker_->Stop();

        if (AccountStatisticsGossipExecutor_) {
            AccountStatisticsGossipExecutor_->Stop();
            AccountStatisticsGossipExecutor_.Reset();
        }

        if (MembershipClosureRecomputeExecutor_) {
            MembershipClosureRecomputeExecutor_->Stop();
            MembershipClosureRecomputeExecutor_.Reset();
        }

        if (AccountMasterMemoryUsageUpdateExecutor_) {
            AccountMasterMemoryUsageUpdateExecutor_->Stop();
            AccountMasterMemoryUsageUpdateExecutor_.Reset();
        }
    }

    void OnStopFollowing() override
    {
        TMasterAutomatonPart::OnStopFollowing();

        RequestTracker_->Stop();
    }


    void CommitAccountMasterMemoryUsage()
    {
        NProto::TReqUpdateAccountMasterMemoryUsage request;
        for (auto [id, account] : AccountMap_) {
            if (!IsObjectAlive(account)) {
                continue;
            }

            const auto& resources = account->LocalStatistics().ResourceUsage;

            const auto& newMemoryUsage = account->DetailedMasterMemoryUsage();
            if (newMemoryUsage == resources.DetailedMasterMemory()) {
                continue;
            }

            auto* entry = request.add_entries();
            ToProto(entry->mutable_account_id(), account->GetId());
            ToProto(entry->mutable_detailed_master_memory_usage(), newMemoryUsage);
        }

        if (request.entries_size() > 0) {
            CreateMutation(Bootstrap_->GetHydraFacade()->GetHydraManager(), request)
                ->CommitAndLog(Logger);
        }
    }


    void InitializeAccountStatistics(TAccount* account)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto cellTag = multicellManager->GetCellTag();

        auto& multicellStatistics = account->MulticellStatistics();
        if (multicellStatistics.find(cellTag) == multicellStatistics.end()) {
            multicellStatistics[cellTag] = account->ClusterStatistics();
        }

        if (multicellManager->IsPrimaryMaster()) {
            const auto& secondaryCellTags = multicellManager->GetSecondaryCellTags();
            for (auto secondaryCellTag : secondaryCellTags) {
                multicellStatistics[secondaryCellTag];
            }
        }

        account->SetLocalStatisticsPtr(&multicellStatistics[cellTag]);
        account->DetailedMasterMemoryUsage() = multicellStatistics[cellTag].ResourceUsage.DetailedMasterMemory();
    }

    void OnAccountStatisticsGossip()
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (!multicellManager->IsLocalMasterCellRegistered()) {
            return;
        }

        if (multicellManager->IsPrimaryMaster()) {
            YT_LOG_INFO("Sending account statistics gossip message to secondary cells");
            NProto::TReqSetAccountStatistics request;
            // For each secondary cell, account statistics are being combined and sent.
            // Note, however, that every cell receives the sum of all other cells' information with it's own data excluded.
            // This is done because cell statistics on the primary master might be outdated for any particular cell.
            for (auto cellTag : multicellManager->GetRegisteredMasterCellTags()) {
                for (auto [accountId, account] : AccountMap_) {
                    if (!IsObjectAlive(account)) {
                        continue;
                    }

                    const auto& cellStatistics = GetOrCrash(account->MulticellStatistics(), cellTag);
                    const auto& clusterStatistics = account->ClusterStatistics();
                    auto* entry = request.add_entries();
                    ToProto(entry->mutable_account_id(), account->GetId());
                    ToProto(entry->mutable_statistics(), clusterStatistics - cellStatistics);
                }
                multicellManager->PostToMaster(request, cellTag, false);
            }
        } else {
            YT_LOG_INFO("Sending account statistics gossip message to primary cell");
            NProto::TReqSetAccountStatistics request;
            request.set_cell_tag(multicellManager->GetCellTag());
            for (auto [accountId, account] : AccountMap_) {
                if (!IsObjectAlive(account)) {
                    continue;
                }

                auto* entry = request.add_entries();
                ToProto(entry->mutable_account_id(), account->GetId());
                ToProto(entry->mutable_statistics(), account->LocalStatistics());
            }
            multicellManager->PostToPrimaryMaster(request, false);
        }
    }

    void HydraSetAccountStatistics(NProto::TReqSetAccountStatistics* request)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            SetAccountStatisticsAtPrimaryCell(request);
        } else {
            SetAccountStatisticsAtSecondaryCells(request);
        }
    }

    void SetAccountStatisticsAtPrimaryCell(NProto::TReqSetAccountStatistics* request)
    {
        auto cellTag = request->cell_tag();

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster());

        if (!multicellManager->IsRegisteredMasterCell(cellTag)) {
            YT_LOG_ERROR_IF(IsMutationLoggingEnabled(), "Received account statistics gossip message from unknown cell (CellTag: %v)",
                cellTag);
            return;
        }

        YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "Received account statistics gossip message (CellTag: %v)",
            cellTag);

        for (const auto& entry : request->entries()) {
            auto accountId = FromProto<TAccountId>(entry.account_id());
            auto* account = FindAccount(accountId);
            if (!IsObjectAlive(account)) {
                continue;
            }

            auto newStatistics = FromProto<TAccountStatistics>(entry.statistics());
            *account->GetCellStatistics(cellTag) = newStatistics;
            account->RecomputeClusterStatistics();
        }
    }

    void SetAccountStatisticsAtSecondaryCells(NProto::TReqSetAccountStatistics* request)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsSecondaryMaster());

        YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "Received account statistics gossip message");

        for (const auto& entry : request->entries()) {
            auto accountId = FromProto<TAccountId>(entry.account_id());
            auto* account = FindAccount(accountId);
            if (!IsObjectAlive(account)) {
                continue;
            }

            auto& clusterStatistics = account->ClusterStatistics();
            clusterStatistics = FromProto<TAccountStatistics>(entry.statistics()) + account->LocalStatistics();
        }
    }

    void HydraRecomputeMembershipClosure(NProto::TReqRecomputeMembershipClosure* /*request*/)
    {
        if (MustRecomputeMembershipClosure_) {
            DoRecomputeMembershipClosure();
        }
    }

    void HydraUpdateAccountMasterMemoryUsage(NProto::TReqUpdateAccountMasterMemoryUsage* request)
    {
        for (const auto& entry : request->entries()) {
            auto accountId = FromProto<TAccountId>(entry.account_id());
            auto* account = FindAccount(accountId);

            if (!IsObjectAlive(account)) {
                continue;
            }

            auto newDetailedMasterMemory = FromProto<TDetailedMasterMemory>(entry.detailed_master_memory_usage());
            auto masterMemoryUsageDelta = newDetailedMasterMemory - account->LocalStatistics().ResourceUsage.DetailedMasterMemory();

            account->LocalStatistics().ResourceUsage.DetailedMasterMemory() = newDetailedMasterMemory;
            account->ClusterStatistics().ResourceUsage.IncreaseDetailedMasterMemory(masterMemoryUsageDelta);

            account->LocalStatistics().CommittedResourceUsage.DetailedMasterMemory() = newDetailedMasterMemory;
            account->ClusterStatistics().CommittedResourceUsage.IncreaseDetailedMasterMemory(masterMemoryUsageDelta);

            if (IsChunkHostCell_) {
                auto newChunkHostCellMasterMemoryUsage = newDetailedMasterMemory.GetTotal();
                auto chunkHostCellMasterMemoryUsageDelta = masterMemoryUsageDelta.GetTotal();
                auto& clusterStatistics = account->ClusterStatistics();
                auto& localStatistics = account->LocalStatistics();

                localStatistics.ResourceUsage.SetChunkHostCellMasterMemory(newChunkHostCellMasterMemoryUsage);
                localStatistics.CommittedResourceUsage.SetChunkHostCellMasterMemory(newChunkHostCellMasterMemoryUsage);
                clusterStatistics.ResourceUsage.IncreaseChunkHostCellMasterMemory(chunkHostCellMasterMemoryUsageDelta);
                clusterStatistics.CommittedResourceUsage.IncreaseChunkHostCellMasterMemory(chunkHostCellMasterMemoryUsageDelta);
            }
        }
    }

    void DoUpdateAccountResourceUsageLease(
        TAccountResourceUsageLease* accountResourceUsageLease,
        const TClusterResources& resources)
    {
        auto* account = accountResourceUsageLease->GetAccount();
        auto* transaction = accountResourceUsageLease->GetTransaction();

        auto resourcesDelta = resources - accountResourceUsageLease->Resources();

        account->ClusterStatistics().ResourceUsage += resourcesDelta;
        account->LocalStatistics().ResourceUsage += resourcesDelta;

        auto* transactionResources = GetTransactionAccountUsage(transaction, account);
        *transactionResources += resourcesDelta;

        accountResourceUsageLease->Resources() = resources;
    }

    void OnTransactionFinished(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YT_VERIFY(transaction->NestedTransactions().empty());

        if (transaction->AccountResourceUsageLeases().empty()) {
            return;
        }

        // Remove account usage leases that belong to transaction.
        auto accountResourceUsageLeases = transaction->AccountResourceUsageLeases();
        for (auto* accountResourceUsageLease : accountResourceUsageLeases) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            // NB: Unref of account usage lease removes it from transacation that lease belongs to.
            objectManager->UnrefObject(accountResourceUsageLease);
        }

        YT_VERIFY(transaction->AccountResourceUsageLeases().empty());
    }

    void OnReplicateKeysToSecondaryMaster(TCellTag cellTag)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        // NB: media are referenced by accounts, so the former must be replicated
        // before the latter. That's why it's done here and not in the chunk manager.

        // Sort media by index to make sure they have the same indexing in the secondary
        // cell. Also, this makes the code deterministic which is a must.
        std::vector<TMedium*> media;
        media.reserve(chunkManager->Media().size());
        for (auto [mediumId, medium] : chunkManager->Media()) {
            if (IsObjectAlive(medium)) {
                media.push_back(medium);
            }
        }
        auto indexLess = [] (TMedium* lhs, TMedium* rhs) {
            return lhs->GetIndex() < rhs->GetIndex();
        };
        std::sort(media.begin(), media.end(), indexLess);
        for (auto* medium : media) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(medium, cellTag);
        }

        auto accounts = GetValuesSortedByKey(AccountMap_);
        for (auto* account : accounts) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(account, cellTag);
        }

        auto users = GetValuesSortedByKey(UserMap_);
        for (auto* user : users) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(user, cellTag);
        }

        auto groups = GetValuesSortedByKey(GroupMap_);
        for (auto* group : groups) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(group, cellTag);
        }
    }

    void OnReplicateValuesToSecondaryMaster(TCellTag cellTag)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        auto media = GetValuesSortedByKey(chunkManager->Media());
        for (auto* medium : media) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(medium, cellTag);
        }

        auto accounts = GetValuesSortedByKey(AccountMap_);
        for (auto* account : accounts) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(account, cellTag);
        }

        auto users = GetValuesSortedByKey(UserMap_);
        for (auto* user : users) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(user, cellTag);
        }

        auto groups = GetValuesSortedByKey(GroupMap_);
        for (auto* group : groups) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(group, cellTag);
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto replicateMembership = [&] (TSubject* subject) {
            for (auto* group : subject->MemberOf()) {
                auto req = TGroupYPathProxy::AddMember(FromObjectId(group->GetId()));
                req->set_name(subject->GetName());
                req->set_ignore_existing(true);
                multicellManager->PostToMaster(req, cellTag);
            }
        };

        for (auto* user : users) {
            replicateMembership(user);
        }

        for (auto* group : groups) {
            replicateMembership(group);
        }
    }

    void ValidateSubjectName(const TString& name)
    {
        if (name.empty()) {
            THROW_ERROR_EXCEPTION("Subject name cannot be empty");
        }
        auto* subjectByAlias = FindSubjectByAlias(name, false /*activeLifeStageOnly*/);
        if (subjectByAlias) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Alias %Qv already exists and points to %Qv",
                name, subjectByAlias->GetName());
        }
        if (FindUserByName(name, false /*activeLifeStageOnly*/)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "User %Qv already exists",
                name);
        }

        if (FindGroupByName(name)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Group %Qv already exists",
                name);
        }
    }

    class TPermissionChecker
    {
    public:
        TPermissionChecker(
            TImpl* impl,
            TUser* user,
            EPermission permission,
            const TPermissionCheckOptions& options)
            : Impl_(impl)
            , User_(user)
            , FullRead_(CheckPermissionMatch(permission, EPermission::FullRead))
            , Permission_(FullRead_
                ? (permission ^ EPermission::FullRead) | EPermission::Read
                : permission)
            , Options_(options)
        {
            auto fastAction = FastCheckPermission();
            if (fastAction != ESecurityAction::Undefined) {
                Response_ = MakeFastCheckPermissionResponse(fastAction, options);
                Proceed_ = false;
                return;
            }

            Response_.Action = ESecurityAction::Undefined;
            if (Options_.Columns) {
                for (const auto& column : *Options_.Columns) {
                    // NB: Multiple occurrences are possible.
                    Columns_.insert(column);
                }
            }
            Proceed_ = true;
        }

        bool ShouldProceed() const
        {
            return Proceed_;
        }

        void ProcessAce(
            const TAccessControlEntry& ace,
            TSubject* owner,
            TObject* object,
            int depth)
        {
            if (!Proceed_) {
                return;
            }

            if (ace.Columns) {
                for (const auto& column : *ace.Columns) {
                    auto it = Columns_.find(column);
                    if (it == Columns_.end()) {
                        continue;
                    }
                    // NB: Multiple occurrences are possible.
                    ColumnToResult_.emplace(*it, TPermissionCheckResult());
                }
            }

            if (!CheckInheritanceMode(ace.InheritanceMode, depth)) {
                return;
            }

            if (!CheckPermissionMatch(ace.Permissions, Permission_)) {
                return;
            }

            if (Permission_ == EPermission::RegisterQueueConsumer) {
                // RegisterQueueConsumer may only be present in ACE as a single permission;
                // in this case it is ensured that vitality is specified.
                YT_VERIFY(ace.Vital);
                if (!CheckVitalityMatch(*ace.Vital, Options_.Vital.value_or(false))) {
                    return;
                }
            }

            for (auto* subject : ace.Subjects) {
                auto* adjustedSubject = subject == Impl_->GetOwnerUser() && owner
                    ? owner
                    : subject;
                if (!adjustedSubject) {
                    continue;
                }

                if (!CheckSubjectMatch(adjustedSubject, User_)) {
                    continue;
                }

                if (ace.Columns) {
                    for (const auto& column : *ace.Columns) {
                        auto it = ColumnToResult_.find(column);
                        if (it == ColumnToResult_.end()) {
                            continue;
                        }
                        auto& columnResult = it->second;
                        ProcessMatchingAce(
                            &columnResult,
                            ace,
                            adjustedSubject,
                            object);
                        if (FullRead_ && columnResult.Action == ESecurityAction::Deny) {
                            SetDeny(adjustedSubject, object);
                            break;
                        }
                    }
                } else {
                    ProcessMatchingAce(
                        &Response_,
                        ace,
                        adjustedSubject,
                        object);
                    if (Response_.Action == ESecurityAction::Deny) {
                        SetDeny(adjustedSubject, object);
                        break;
                    }
                }

                if (!Proceed_) {
                    break;
                }
            }
        }

        TPermissionCheckResponse GetResponse()
        {
            if (Response_.Action == ESecurityAction::Undefined) {
                SetDeny(nullptr, nullptr);
            }

            if (Response_.Action == ESecurityAction::Allow && Options_.Columns) {
                Response_.Columns = std::vector<TPermissionCheckResult>(Options_.Columns->size());
                std::optional<TPermissionCheckResult> deniedColumnResult;
                for (size_t index = 0; index < Options_.Columns->size(); ++index) {
                    const auto& column = (*Options_.Columns)[index];
                    auto& result = (*Response_.Columns)[index];
                    auto it = ColumnToResult_.find(column);
                    if (it == ColumnToResult_.end()) {
                        result = static_cast<const TPermissionCheckResult>(Response_);
                    } else {
                        result = it->second;
                        if (result.Action == ESecurityAction::Undefined) {
                            result.Action = ESecurityAction::Deny;
                            if (!deniedColumnResult) {
                                deniedColumnResult = result;
                            }
                        }
                    }
                }

                if (FullRead_ && deniedColumnResult) {
                    SetDeny(deniedColumnResult->Subject, deniedColumnResult->Object);
                }
            }

            return std::move(Response_);
        }

    private:
        TImpl* const Impl_;
        TUser* const User_;
        const bool FullRead_;
        const EPermission Permission_;
        const TPermissionCheckOptions& Options_;

        THashSet<TStringBuf> Columns_;
        THashMap<TStringBuf, TPermissionCheckResult> ColumnToResult_;

        bool Proceed_;
        TPermissionCheckResponse Response_;

        ESecurityAction FastCheckPermission()
        {
            // "replicator", though being superuser, can only read in safe mode.
            if (User_ == Impl_->ReplicatorUser_ &&
                Permission_ != EPermission::Read &&
                Impl_->IsSafeMode())
            {
                return ESecurityAction::Deny;
            }

            // Banned users are denied any permission.
            if (User_->GetBanned()) {
                return ESecurityAction::Deny;
            }

            // "root" and "superusers" need no authorization.
            if (Impl_->IsSuperuser(User_)) {
                return ESecurityAction::Allow;
            }

            // Non-reads are forbidden in safe mode.
            if (Permission_ != EPermission::Read &&
                Impl_->Bootstrap_->GetConfigManager()->GetConfig()->EnableSafeMode)
            {
                return ESecurityAction::Deny;
            }

            return ESecurityAction::Undefined;
        }

        static bool CheckSubjectMatch(TSubject* subject, TUser* user)
        {
            switch (subject->GetType()) {
                case EObjectType::User:
                    return subject == user;

                case EObjectType::Group: {
                    auto* subjectGroup = subject->AsGroup();
                    return user->RecursiveMemberOf().find(subjectGroup) != user->RecursiveMemberOf().end();
                }

                default:
                    YT_ABORT();
            }
        }

        static bool CheckInheritanceMode(EAceInheritanceMode mode, int depth)
        {
            return GetInheritedInheritanceMode(mode, depth).has_value();
        }

        static bool CheckPermissionMatch(EPermissionSet permissions, EPermission requestedPermission)
        {
            return (permissions & requestedPermission) != NonePermissions;
        }

        static bool CheckVitalityMatch(bool vital, bool requestedVital)
        {
            return !requestedVital || vital;
        }

        static TPermissionCheckResponse MakeFastCheckPermissionResponse(ESecurityAction action, const TPermissionCheckOptions& options)
        {
            TPermissionCheckResponse response;
            response.Action = action;
            if (options.Columns) {
                response.Columns = std::vector<TPermissionCheckResult>(options.Columns->size());
                for (size_t index = 0; index < options.Columns->size(); ++index) {
                    (*response.Columns)[index].Action = action;
                }
            }
            return response;
        }

        void ProcessMatchingAce(
            TPermissionCheckResult* result,
            const TAccessControlEntry& ace,
            TSubject* subject,
            TObject* object)
        {
            if (result->Action == ESecurityAction::Deny) {
                return;
            }

            result->Action = ace.Action;
            result->Object = object;
            result->Subject = subject;
        }

        static void SetDeny(TPermissionCheckResult* result, TSubject* subject, TObject* object)
        {
            result->Action = ESecurityAction::Deny;
            result->Subject = subject;
            result->Object = object;
        }

        void SetDeny(TSubject* subject, TObject* object)
        {
            SetDeny(&Response_, subject, object);
            if (Response_.Columns) {
                for (auto& result : *Response_.Columns) {
                    SetDeny(&result, subject, object);
                }
            }
            Proceed_ = false;
        }
    };

    static std::optional<EAceInheritanceMode> GetInheritedInheritanceMode(EAceInheritanceMode mode, int depth)
    {
        auto nothing = std::optional<EAceInheritanceMode>();
        switch (mode) {
            case EAceInheritanceMode::ObjectAndDescendants:
                return EAceInheritanceMode::ObjectAndDescendants;
            case EAceInheritanceMode::ObjectOnly:
                return (depth == 0 ? EAceInheritanceMode::ObjectOnly : nothing);
            case EAceInheritanceMode::DescendantsOnly:
                return (depth > 0 ? EAceInheritanceMode::ObjectAndDescendants : nothing);
            case EAceInheritanceMode::ImmediateDescendantsOnly:
                return (depth == 1 ? EAceInheritanceMode::ObjectOnly : nothing);
            default:
                YT_ABORT();
        }
    }

    TString GetPermissionCheckTargetName(const TPermissionCheckTarget& target)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto name = objectManager->GetHandler(target.Object)->GetName(target.Object);
        if (target.Column) {
            return Format("column %Qv of %v",
                *target.Column,
                name);
        } else {
            return name;
        }
    }

    const TDynamicSecurityManagerConfigPtr& GetDynamicConfig() const
    {
        return Bootstrap_->GetConfigManager()->GetConfig()->SecurityManager;
    }

    void OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/ = nullptr)
    {
        if (AccountStatisticsGossipExecutor_) {
            AccountStatisticsGossipExecutor_->SetPeriod(GetDynamicConfig()->AccountStatisticsGossipPeriod);
        }

        if (MembershipClosureRecomputeExecutor_) {
            MembershipClosureRecomputeExecutor_->SetPeriod(GetDynamicConfig()->MembershipClosureRecomputePeriod);
        }

        if (AccountMasterMemoryUsageUpdateExecutor_) {
            AccountMasterMemoryUsageUpdateExecutor_->SetPeriod(GetDynamicConfig()->AccountMasterMemoryUsageUpdatePeriod);
        }

        if (HasMutationContext()) {
            RecomputeMasterMemoryUsageOnConfigChange();
        }
    }

    void RecomputeMasterMemoryUsageOnConfigChange()
    {

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto cellTag = multicellManager->GetCellTag();
        auto roles = multicellManager->GetMasterCellRoles(cellTag);
        auto wasChunkHostCell = IsChunkHostCell_;
        IsChunkHostCell_ = Any(roles & EMasterCellRoles::ChunkHost);
        if (wasChunkHostCell != IsChunkHostCell_) {
            for (auto [accountId, account] : AccountMap_) {
                if (!IsObjectAlive(account)) {
                    continue;
                }

                auto& localStatistics = account->LocalStatistics();
                auto& clusterStatistics = account->ClusterStatistics();

                auto& localResourceUsage = localStatistics.ResourceUsage;
                auto& localCommittedUsage = localStatistics.CommittedResourceUsage;

                auto& clusterResourceUsage = clusterStatistics.ResourceUsage;
                auto& clusterCommittedUsage = clusterStatistics.CommittedResourceUsage;

                if (wasChunkHostCell) {
                    clusterResourceUsage.SetChunkHostCellMasterMemory(clusterResourceUsage.GetChunkHostCellMasterMemory() - localResourceUsage.GetChunkHostCellMasterMemory());
                    clusterCommittedUsage.SetChunkHostCellMasterMemory(clusterCommittedUsage.GetChunkHostCellMasterMemory() - localCommittedUsage.GetChunkHostCellMasterMemory());

                    if (clusterResourceUsage.GetChunkHostCellMasterMemory() < 0) {
                        YT_LOG_ALERT("Chunk host cell memory is negative after removing chunk host role from cell %v", cellTag);
                    }

                    if (clusterCommittedUsage.GetChunkHostCellMasterMemory() < 0) {
                        YT_LOG_ALERT("Committed chunk host cell memory is negative after removing chunk host role from cell %v", cellTag);
                    }

                    localResourceUsage.SetChunkHostCellMasterMemory(0);
                    localCommittedUsage.SetChunkHostCellMasterMemory(0);
                } else {
                    localResourceUsage.SetChunkHostCellMasterMemory(localResourceUsage.DetailedMasterMemory().GetTotal());
                    localCommittedUsage.SetChunkHostCellMasterMemory(localCommittedUsage.DetailedMasterMemory().GetTotal());

                    clusterResourceUsage.SetChunkHostCellMasterMemory(clusterResourceUsage.GetChunkHostCellMasterMemory() + localResourceUsage.GetChunkHostCellMasterMemory());
                    clusterCommittedUsage.SetChunkHostCellMasterMemory(clusterCommittedUsage.GetChunkHostCellMasterMemory() + localCommittedUsage.GetChunkHostCellMasterMemory());
                }
            }
        }
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TSecurityManager::TImpl, Account, TAccount, AccountMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TSecurityManager::TImpl, AccountResourceUsageLease, TAccountResourceUsageLease, AccountResourceUsageLeaseMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TSecurityManager::TImpl, User, TUser, UserMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TSecurityManager::TImpl, Group, TGroup, GroupMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TSecurityManager::TImpl, NetworkProject, TNetworkProject, NetworkProjectMap_)


////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TAccountTypeHandler::TAccountTypeHandler(TImpl* owner)
    : TNonversionedMapObjectTypeHandlerBase<TAccount>(owner->Bootstrap_, &owner->AccountMap_)
    , Owner_(owner)
{ }

TObject* TSecurityManager::TAccountTypeHandler::CreateObject(
    TObjectId hintId,
    IAttributeDictionary* attributes)
{
    auto name = attributes->GetAndRemove<TString>("name");
    auto parentName = attributes->GetAndRemove<TString>("parent_name", NSecurityClient::RootAccountName);
    auto* parent = Owner_->GetAccountByNameOrThrow(parentName, true /*activeLifeStageOnly*/);
    attributes->Set("hint_id", hintId);

    auto* account = CreateObjectImpl(name, parent, attributes);
    return account;
}

std::optional<TObject*> TSecurityManager::TAccountTypeHandler::FindObjectByAttributes(
    const NYTree::IAttributeDictionary* attributes)
{
    auto name = attributes->Get<TString>("name");
    auto parentName = attributes->Get<TString>("parent_name", RootAccountName);
    auto* parent = Owner_->GetAccountByNameOrThrow(parentName, true /*activeLifeStageOnly*/);

    return parent->FindChild(name);
}

TIntrusivePtr<TNonversionedMapObjectProxyBase<TAccount>> TSecurityManager::TAccountTypeHandler::GetMapObjectProxy(
    TAccount* account)
{
    return CreateAccountProxy(Owner_->Bootstrap_, &Metadata_, account);
}

void TSecurityManager::TAccountTypeHandler::DoZombifyObject(TAccount* account)
{
    TNonversionedMapObjectTypeHandlerBase<TAccount>::DoZombifyObject(account);
    Owner_->DestroyAccount(account);
}

void TSecurityManager::TAccountTypeHandler::RegisterName(const TString& name, TAccount* account) noexcept
{
    Owner_->RegisterAccountName(name, account);
}

void TSecurityManager::TAccountTypeHandler::UnregisterName(const TString& name, TAccount* /* account */) noexcept
{
    Owner_->UnregisterAccountName(name);
}

TString TSecurityManager::TAccountTypeHandler::GetRootPath(const TAccount* rootAccount) const
{
    YT_VERIFY(rootAccount == Owner_->GetRootAccount());
    return RootAccountCypressPath;
}

std::optional<int> TSecurityManager::TAccountTypeHandler::GetSubtreeSizeLimit() const {
    return Owner_->GetDynamicConfig()->MaxAccountSubtreeSize;
}

////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TAccountResourceUsageLeaseTypeHandler::TAccountResourceUsageLeaseTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase<TAccountResourceUsageLease>(owner->Bootstrap_, &owner->AccountResourceUsageLeaseMap_)
    , Owner_(owner)
{ }

TObject* TSecurityManager::TAccountResourceUsageLeaseTypeHandler::CreateObject(
    TObjectId hintId,
    IAttributeDictionary* attributes)
{
    auto transactionId = attributes->GetAndRemove<TTransactionId>("transaction_id");
    auto accountName = attributes->GetAndRemove<TString>("account");

    return Owner_->CreateAccountResourceUsageLease(accountName, transactionId, hintId);
}

void TSecurityManager::TAccountResourceUsageLeaseTypeHandler::DoZombifyObject(TAccountResourceUsageLease* accountResourceUsageLease)
{
    TObjectTypeHandlerWithMapBase<TAccountResourceUsageLease>::DoZombifyObject(accountResourceUsageLease);
    Owner_->DestroyAccountResourceUsageLease(accountResourceUsageLease);
}

////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TUserTypeHandler::TUserTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->UserMap_)
    , Owner_(owner)
{ }

TObject* TSecurityManager::TUserTypeHandler::CreateObject(
    TObjectId hintId,
    IAttributeDictionary* attributes)
{
    auto name = attributes->GetAndRemove<TString>("name");

    return Owner_->CreateUser(name, hintId);
}

std::optional<TObject*> TSecurityManager::TUserTypeHandler::FindObjectByAttributes(
    const NYTree::IAttributeDictionary* attributes)
{
    auto name = attributes->Get<TString>("name");

    return Owner_->FindUserByNameOrAlias(name, /* activeLifeStageOnly */ true);
}

IObjectProxyPtr TSecurityManager::TUserTypeHandler::DoGetProxy(
    TUser* user,
    TTransaction* /*transaction*/)
{
    return CreateUserProxy(Owner_->Bootstrap_, &Metadata_, user);
}

void TSecurityManager::TUserTypeHandler::DoZombifyObject(TUser* user)
{
    TObjectTypeHandlerWithMapBase::DoZombifyObject(user);
    Owner_->DestroyUser(user);
}

////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TGroupTypeHandler::TGroupTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->GroupMap_)
    , Owner_(owner)
{ }

TObject* TSecurityManager::TGroupTypeHandler::CreateObject(
    TObjectId hintId,
    IAttributeDictionary* attributes)
{
    auto name = attributes->GetAndRemove<TString>("name");

    return Owner_->CreateGroup(name, hintId);
}

IObjectProxyPtr TSecurityManager::TGroupTypeHandler::DoGetProxy(
    TGroup* group,
    TTransaction* /*transaction*/)
{
    return CreateGroupProxy(Owner_->Bootstrap_, &Metadata_, group);
}

void TSecurityManager::TGroupTypeHandler::DoZombifyObject(TGroup* group)
{
    TObjectTypeHandlerWithMapBase::DoZombifyObject(group);
    Owner_->DestroyGroup(group);
}

std::optional<TObject*> TSecurityManager::TGroupTypeHandler::FindObjectByAttributes(
    const NYTree::IAttributeDictionary* attributes)
{
    auto name = attributes->Get<TString>("name");

    return Owner_->FindGroupByNameOrAlias(name);
}

////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TNetworkProjectTypeHandler::TNetworkProjectTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->NetworkProjectMap_)
    , Owner_(owner)
{ }

TObject* TSecurityManager::TNetworkProjectTypeHandler::CreateObject(
    TObjectId hintId,
    IAttributeDictionary* attributes)
{
    auto name = attributes->GetAndRemove<TString>("name");

    return Owner_->CreateNetworkProject(name, hintId);
}

IObjectProxyPtr TSecurityManager::TNetworkProjectTypeHandler::DoGetProxy(
    TNetworkProject* networkProject,
    TTransaction* /*transaction*/)
{
    return CreateNetworkProjectProxy(Owner_->Bootstrap_, &Metadata_, networkProject);
}

void TSecurityManager::TNetworkProjectTypeHandler::DoZombifyObject(TNetworkProject* networkProject)
{
    TObjectTypeHandlerWithMapBase::DoZombifyObject(networkProject);
    Owner_->DestroyNetworkProject(networkProject);
}

////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TProxyRoleTypeHandler::TProxyRoleTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->ProxyRoleMap_)
    , Owner_(owner)
{ }

TObject* TSecurityManager::TProxyRoleTypeHandler::CreateObject(
    TObjectId hintId,
    IAttributeDictionary* attributes)
{
    auto name = attributes->GetAndRemove<TString>("name");
    auto proxyKind = attributes->GetAndRemove<EProxyKind>("proxy_kind");

    return Owner_->CreateProxyRole(name, proxyKind, hintId);
}

IObjectProxyPtr TSecurityManager::TProxyRoleTypeHandler::DoGetProxy(
    TProxyRole* proxyRole,
    TTransaction* /* transaction */)
{
    return CreateProxyRoleProxy(Owner_->Bootstrap_, &Metadata_, proxyRole);
}

void TSecurityManager::TProxyRoleTypeHandler::DoZombifyObject(TProxyRole* proxyRole)
{
    TObjectTypeHandlerWithMapBase::DoZombifyObject(proxyRole);
    Owner_->DestroyProxyRole(proxyRole);
}

////////////////////////////////////////////////////////////////////////////////

TSecurityManager::TSecurityManager(
    const TSecurityManagerConfigPtr& config,
    NCellMaster::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        config,
        bootstrap))
{ }

TSecurityManager::~TSecurityManager() = default;

void TSecurityManager::Initialize()
{
    return Impl_->Initialize();
}

TAccount* TSecurityManager::CreateAccount(TObjectId hintId)
{
    return Impl_->CreateAccount(hintId);
}

TAccount* TSecurityManager::GetAccountOrThrow(TAccountId id)
{
    return Impl_->GetAccountOrThrow(id);
}

TAccount* TSecurityManager::FindAccountByName(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->FindAccountByName(name, activeLifeStageOnly);
}

TAccount* TSecurityManager::GetAccountByNameOrThrow(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->GetAccountByNameOrThrow(name, activeLifeStageOnly);
}

TAccount* TSecurityManager::GetRootAccount()
{
    return Impl_->GetRootAccount();
}

TAccount* TSecurityManager::GetSysAccount()
{
    return Impl_->GetSysAccount();
}

TAccount* TSecurityManager::GetTmpAccount()
{
    return Impl_->GetTmpAccount();
}

TAccount* TSecurityManager::GetIntermediateAccount()
{
    return Impl_->GetIntermediateAccount();
}

TAccount* TSecurityManager::GetChunkWiseAccountingMigrationAccount()
{
    return Impl_->GetChunkWiseAccountingMigrationAccount();
}

void TSecurityManager::TrySetResourceLimits(TAccount* account, const TClusterResourceLimits& resourceLimits)
{
    Impl_->TrySetResourceLimits(account, resourceLimits);
}

void TSecurityManager::TransferAccountResources(
    TAccount* srcAccount,
    TAccount* dstAccount,
    const TClusterResourceLimits& resourceDelta)
{
    Impl_->TransferAccountResources(srcAccount, dstAccount, resourceDelta);
}

void TSecurityManager::UpdateResourceUsage(const TChunk* chunk, const TChunkRequisition& requisition, i64 delta)
{
    Impl_->UpdateResourceUsage(chunk, requisition, delta);
}

void TSecurityManager::UpdateAccountResourceUsageLease(
    TAccountResourceUsageLease* accountResourceUsageLease,
    const TClusterResources& resources)
{
    Impl_->UpdateAccountResourceUsageLease(accountResourceUsageLease, resources);
}

void TSecurityManager::UpdateTabletResourceUsage(TCypressNode* node, const TClusterResources& resourceUsageDelta)
{
    Impl_->UpdateTabletResourceUsage(node, resourceUsageDelta);
}

void TSecurityManager::UpdateTransactionResourceUsage(const TChunk* chunk, const TChunkRequisition& requisition, i64 delta)
{
    Impl_->UpdateTransactionResourceUsage(chunk, requisition, delta);
}

void TSecurityManager::UpdateMasterMemoryUsage(TCypressNode* node)
{
    Impl_->UpdateMasterMemoryUsage(node);
}

void TSecurityManager::UpdateMasterMemoryUsage(TMasterTableSchema* schema, TAccount* account)
{
    Impl_->UpdateMasterMemoryUsage(schema, account);
}

void TSecurityManager::ResetMasterMemoryUsage(TMasterTableSchema* schema, TAccount* account)
{
    Impl_->ResetMasterMemoryUsage(schema, account);
}

void TSecurityManager::ResetTransactionAccountResourceUsage(TTransaction* transaction)
{
    Impl_->ResetTransactionAccountResourceUsage(transaction);
}

void TSecurityManager::RecomputeTransactionAccountResourceUsage(TTransaction* transaction)
{
    Impl_->RecomputeTransactionResourceUsage(transaction);
}

void TSecurityManager::SetAccount(
    TCypressNode* node,
    TAccount* newAccount,
    TTransaction* transaction) noexcept
{
    Impl_->SetAccount(node, newAccount, transaction);
}

void TSecurityManager::ResetAccount(TCypressNode* node)
{
    Impl_->ResetAccount(node);
}

TUser* TSecurityManager::FindUserByName(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->FindUserByName(name, activeLifeStageOnly);
}

TUser* TSecurityManager::FindUserByNameOrAlias(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->FindUserByNameOrAlias(name, activeLifeStageOnly);
}

TUser* TSecurityManager::GetUserByNameOrThrow(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->GetUserByNameOrThrow(name, activeLifeStageOnly);
}

TUser* TSecurityManager::GetUserOrThrow(TUserId id)
{
    return Impl_->GetUserOrThrow(id);
}

TUser* TSecurityManager::GetRootUser()
{
    return Impl_->GetRootUser();
}

TUser* TSecurityManager::GetGuestUser()
{
    return Impl_->GetGuestUser();
}

TUser* TSecurityManager::GetOwnerUser()
{
    return Impl_->GetOwnerUser();
}

TGroup* TSecurityManager::FindGroupByNameOrAlias(const TString& name)
{
    return Impl_->FindGroupByNameOrAlias(name);
}

TGroup* TSecurityManager::GetEveryoneGroup()
{
    return Impl_->GetEveryoneGroup();
}

TGroup* TSecurityManager::GetUsersGroup()
{
    return Impl_->GetUsersGroup();
}

TGroup* TSecurityManager::GetSuperusersGroup()
{
    return Impl_->GetSuperusersGroup();
}

TSubject* TSecurityManager::FindSubject(TSubjectId id)
{
    return Impl_->FindSubject(id);
}

TSubject* TSecurityManager::GetSubjectOrThrow(TSubjectId id)
{
    return Impl_->GetSubjectOrThrow(id);
}

TSubject* TSecurityManager::FindSubjectByNameOrAlias(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->FindSubjectByNameOrAlias(name, activeLifeStageOnly);
}

TSubject* TSecurityManager::GetSubjectByNameOrAliasOrThrow(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->GetSubjectByNameOrAliasOrThrow(name, activeLifeStageOnly);
}

TNetworkProject* TSecurityManager::FindNetworkProjectByName(const TString& name)
{
    return Impl_->FindNetworkProjectByName(name);
}

void TSecurityManager::RenameNetworkProject(NYT::NSecurityServer::TNetworkProject* networkProject, const TString& newName)
{
    Impl_->RenameNetworkProject(networkProject, newName);
}

const THashMap<TString, TProxyRole*>& TSecurityManager::GetProxyRolesWithProxyKind(EProxyKind proxyKind) const
{
    return Impl_->GetProxyRolesWithProxyKind(proxyKind);
}

void TSecurityManager::AddMember(TGroup* group, TSubject* member, bool ignoreExisting)
{
    Impl_->AddMember(group, member, ignoreExisting);
}

void TSecurityManager::RemoveMember(TGroup* group, TSubject* member, bool ignoreMissing)
{
    Impl_->RemoveMember(group, member, ignoreMissing);
}

void TSecurityManager::RenameSubject(TSubject* subject, const TString& newName)
{
    Impl_->RenameSubject(subject, newName);
}

TAccessControlDescriptor* TSecurityManager::FindAcd(TObject* object)
{
    return Impl_->FindAcd(object);
}

TAccessControlDescriptor* TSecurityManager::GetAcd(TObject* object)
{
    return Impl_->GetAcd(object);
}

TAccessControlList TSecurityManager::GetEffectiveAcl(TObject* object)
{
    return Impl_->GetEffectiveAcl(object);
}

std::optional<TString> TSecurityManager::GetEffectiveAnnotation(TCypressNode* node)
{
    return Impl_->GetEffectiveAnnotation(node);
}

void TSecurityManager::SetAuthenticatedUser(TUser* user)
{
    Impl_->SetAuthenticatedUser(user);
}

void TSecurityManager::ResetAuthenticatedUser()
{
    Impl_->ResetAuthenticatedUser();
}

TUser* TSecurityManager::GetAuthenticatedUser()
{
    return Impl_->GetAuthenticatedUser();
}

bool TSecurityManager::IsSafeMode()
{
    return Impl_->IsSafeMode();
}

TPermissionCheckResponse TSecurityManager::CheckPermission(
    TObject* object,
    TUser* user,
    EPermission permission,
    TPermissionCheckOptions options)
{
    return Impl_->CheckPermission(
        object,
        user,
        permission,
        std::move(options));
}

TPermissionCheckResponse TSecurityManager::CheckPermission(
    TUser* user,
    EPermission permission,
    const TAccessControlList& acl,
    TPermissionCheckOptions options)
{
    return Impl_->CheckPermission(
        user,
        permission,
        acl,
        std::move(options));
}

bool TSecurityManager::IsSuperuser(const TUser* user) const
{
    return Impl_->IsSuperuser(user);
}

void TSecurityManager::ValidatePermission(
    TObject* object,
    TUser* user,
    EPermission permission,
    TPermissionCheckOptions options)
{
    Impl_->ValidatePermission(
        object,
        user,
        permission,
        std::move(options));
}

void TSecurityManager::ValidatePermission(
    TObject* object,
    EPermission permission,
    TPermissionCheckOptions options)
{
    Impl_->ValidatePermission(
        object,
        permission,
        std::move(options));
}

void TSecurityManager::LogAndThrowAuthorizationError(
    const TPermissionCheckTarget& target,
    TUser* user,
    EPermission permission,
    const TPermissionCheckResult& result)
{
    Impl_->LogAndThrowAuthorizationError(
        target,
        user,
        permission,
        result);
}

void TSecurityManager::ValidateResourceUsageIncrease(
    TAccount* account,
    const TClusterResources& delta,
    bool allowRootAccount)
{
    Impl_->ValidateResourceUsageIncrease(
        account,
        delta,
        allowRootAccount);
}

void TSecurityManager::ValidateAttachChildAccount(TAccount* parentAccount, TAccount* childAccount)
{
    Impl_->ValidateAttachChildAccount(parentAccount, childAccount);
}

void TSecurityManager::SetAccountAllowChildrenLimitOvercommit(
    TAccount* account,
    bool overcommitAllowed)
{
    Impl_->SetAccountAllowChildrenLimitOvercommit(account, overcommitAllowed);
}

void TSecurityManager::SetUserBanned(TUser* user, bool banned)
{
    Impl_->SetUserBanned(user, banned);
}

TError TSecurityManager::CheckUserAccess(TUser* user)
{
    return Impl_->CheckUserAccess(user);
}

void TSecurityManager::ChargeUser(TUser* user, const TUserWorkload& workload)
{
    return Impl_->ChargeUser(user, workload);
}

TFuture<void> TSecurityManager::ThrottleUser(TUser* user, int requestCount, EUserWorkloadType workloadType)
{
    return Impl_->ThrottleUser(user, requestCount, workloadType);
}

void TSecurityManager::SetUserRequestRateLimit(TUser* user, int limit, EUserWorkloadType type)
{
    Impl_->SetUserRequestRateLimit(user, limit, type);
}

void TSecurityManager::SetUserRequestLimits(TUser* user, TUserRequestLimitsConfigPtr config)
{
    Impl_->SetUserRequestLimits(user, std::move(config));
}

void TSecurityManager::SetUserRequestQueueSizeLimit(TUser* user, int limit)
{
    Impl_->SetUserRequestQueueSizeLimit(user, limit);
}

bool TSecurityManager::TryIncreaseRequestQueueSize(TUser* user)
{
    return Impl_->TryIncreaseRequestQueueSize(user);
}

void TSecurityManager::DecreaseRequestQueueSize(TUser* user)
{
    Impl_->DecreaseRequestQueueSize(user);
}

const TSecurityTagsRegistryPtr& TSecurityManager::GetSecurityTagsRegistry() const
{
    return Impl_->GetSecurityTagsRegistry();
}

void TSecurityManager::SetSubjectAliases(TSubject* subject, const std::vector<TString>& aliases)
{
    Impl_->SetSubjectAliases(subject, aliases);
}

TProfilerTagPtr TSecurityManager::GetUserCpuProfilerTag(TUser* user)
{
    return Impl_->GetUserCpuProfilerTag(user);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TSecurityManager, Account, TAccount, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TSecurityManager, AccountResourceUsageLease, TAccountResourceUsageLease, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TSecurityManager, User, TUser, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TSecurityManager, Group, TGroup, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TSecurityManager, NetworkProject, TNetworkProject, *Impl_)

DELEGATE_SIGNAL(TSecurityManager, void(TUser*, const TUserWorkload&), UserCharged, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityServer
