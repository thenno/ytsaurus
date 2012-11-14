#include "stdafx.h"
#include "cypress_integration.h"
#include "node.h"
#include "node_statistics.h"
#include "private.h"

#include <ytlib/misc/string.h>

#include <ytlib/actions/bind.h>

#include <ytlib/ytree/virtual.h>
#include <ytlib/ytree/fluent.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <server/cypress_server/virtual.h>
#include <server/cypress_server/node_proxy_detail.h>
#include <server/chunk_server/chunk_manager.h>
#include <server/chunk_server/node_authority.h>

#include <server/orchid/cypress_integration.h>

#include <server/cell_master/bootstrap.h>

namespace NYT {
namespace NChunkServer {

using namespace NYTree;
using namespace NYPath;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NMetaState;
using namespace NOrchid;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NCellMaster;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EChunkFilter,
    (All)
    (Lost)
    (Overreplicated)
    (Underreplicated)
);

class TVirtualChunkMap
    : public TVirtualMapBase
{
public:
    TVirtualChunkMap(TBootstrap* bootstrap, EChunkFilter filter)
        : Bootstrap(bootstrap)
        , Filter(filter)
    { }

private:
    TBootstrap* Bootstrap;
    EChunkFilter Filter;

    const yhash_set<TChunkId>& GetFilteredChunkIds() const
    {
        auto chunkManager = Bootstrap->GetChunkManager();
        switch (Filter) {
            case EChunkFilter::Lost:
                return chunkManager->LostChunkIds();
            case EChunkFilter::Overreplicated:
                return chunkManager->OverreplicatedChunkIds();
            case EChunkFilter::Underreplicated:
                return chunkManager->UnderreplicatedChunkIds();
            default:
                YUNREACHABLE();
        }
    }

    bool CheckFilter(const TChunkId& chunkId) const
    {
        if (Filter == EChunkFilter::All) {
            return true;
        }

        const auto& chunkIds = GetFilteredChunkIds();
        return chunkIds.find(chunkId) != chunkIds.end();
    }

    virtual std::vector<Stroka> GetKeys(size_t sizeLimit) const override
    {
        if (Filter == EChunkFilter::All) {
            const auto& chunkIds = Bootstrap->GetChunkManager()->GetChunkIds(sizeLimit);
            return ConvertToStrings(chunkIds.begin(), chunkIds.end(), sizeLimit);
        } else {
            const auto& chunkIds = GetFilteredChunkIds();
            return ConvertToStrings(chunkIds.begin(), chunkIds.end(), sizeLimit);
        }
    }

    virtual size_t GetSize() const override
    {
        if (Filter == EChunkFilter::All) {
            return Bootstrap->GetChunkManager()->GetChunkCount();
        } else {
            return GetFilteredChunkIds().size();
        }
    }

    virtual IYPathServicePtr GetItemService(const TStringBuf& key) const override
    {
        auto id = TChunkId::FromString(key);

        if (TypeFromId(id) != EObjectType::Chunk) {
            return NULL;
        }

        if (!CheckFilter(id)) {
            return NULL;
        }

        return Bootstrap->GetObjectManager()->FindProxy(id);
    }
};

INodeTypeHandlerPtr CreateChunkMapTypeHandler(
    TBootstrap* bootstrap,
    EObjectType objectType,
    EChunkFilter filter)
{
    YCHECK(bootstrap);

    auto service = New<TVirtualChunkMap>(bootstrap, filter);
    return CreateVirtualTypeHandler(bootstrap, objectType, service, true);
}

INodeTypeHandlerPtr CreateChunkMapTypeHandler(TBootstrap* bootstrap)
{
    return CreateChunkMapTypeHandler(bootstrap, EObjectType::ChunkMap, EChunkFilter::All);
}

INodeTypeHandlerPtr CreateLostChunkMapTypeHandler(TBootstrap* bootstrap)
{
    return CreateChunkMapTypeHandler(bootstrap, EObjectType::LostChunkMap, EChunkFilter::Lost);
}

INodeTypeHandlerPtr CreateOverreplicatedChunkMapTypeHandler(TBootstrap* bootstrap)
{
    return CreateChunkMapTypeHandler(bootstrap, EObjectType::OverreplicatedChunkMap, EChunkFilter::Overreplicated);
}

INodeTypeHandlerPtr CreateUnderreplicatedChunkMapTypeHandler(TBootstrap* bootstrap)
{
    return CreateChunkMapTypeHandler(bootstrap, EObjectType::UnderreplicatedChunkMap, EChunkFilter::Underreplicated);
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualChunkListMap
    : public TVirtualMapBase
{
public:
    explicit TVirtualChunkListMap(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    { }

private:
    TBootstrap* Bootstrap;

    virtual std::vector<Stroka> GetKeys(size_t sizeLimit) const override
    {
        const auto& chunkListIds = Bootstrap->GetChunkManager()->GetChunkListIds(sizeLimit);
        return ConvertToStrings(chunkListIds.begin(), chunkListIds.end(), sizeLimit);
    }

    virtual size_t GetSize() const override
    {
        return Bootstrap->GetChunkManager()->GetChunkListCount();
    }

    virtual IYPathServicePtr GetItemService(const TStringBuf& key) const override
    {
        auto id = TChunkListId::FromString(key);
        if (TypeFromId(id) != EObjectType::ChunkList) {
            return NULL;
        }
        return Bootstrap->GetObjectManager()->FindProxy(id);
    }
};

INodeTypeHandlerPtr CreateChunkListMapTypeHandler(TBootstrap* bootstrap)
{
    YCHECK(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::ChunkListMap,
        New<TVirtualChunkListMap>(bootstrap));
}

////////////////////////////////////////////////////////////////////////////////

class TNodeAuthority
    : public INodeAuthority
{
public:
    explicit TNodeAuthority(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    { }

    virtual bool IsAuthorized(const Stroka& address) override
    {
        auto cypressManager = Bootstrap->GetCypressManager();
        auto resolver = cypressManager->CreateResolver();
        auto nodesNode = resolver->ResolvePath("//sys/nodes");
        if (!nodesNode) {
            LOG_ERROR("Missing //sys/nodes");
            return false;
        }

        auto nodesMap = nodesNode->AsMap();
        auto nodeNode = nodesMap->FindChild(address);

        if (!nodeNode) {
            // New node.
            return true;
        }

        bool banned = nodeNode->Attributes().Get<bool>("banned", false);
        return !banned;
    }
    
private:
    TBootstrap* Bootstrap;

};

INodeAuthorityPtr CreateNodeAuthority(TBootstrap* bootstrap)
{
    return New<TNodeAuthority>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

class TNodeProxy
    : public TMapNodeProxy
{
public:
    TNodeProxy(
        INodeTypeHandlerPtr typeHandler,
        TBootstrap* bootstrap,
        NTransactionServer::TTransaction* transaction,
        ICypressNode* trunkNode)
        : TMapNodeProxy(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
    { }

private:
    TDataNode* GetNode() const
    {
        auto address = GetParent()->AsMap()->GetChildKey(this);
        return Bootstrap->GetChunkManager()->FindNodeByAddress(address);
    }

    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) const override
    {
        const auto* node = GetNode();
        attributes->push_back(TAttributeInfo("state"));
        attributes->push_back(TAttributeInfo("confirmed", node));
        attributes->push_back(TAttributeInfo("incarnation_id", node));
        attributes->push_back(TAttributeInfo("statistics", node));
        TMapNodeProxy::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer) const override
    {
        const auto* node = GetNode();

        if (key == "state") {
            auto state = node ? node->GetState() : ENodeState(ENodeState::Offline);
            BuildYsonFluently(consumer)
                .Scalar(FormatEnum(state));
            return true;
        }

        if (node) {
            if (key == "confirmed") {
                ValidateActiveLeader();
                BuildYsonFluently(consumer)
                    .Scalar(FormatBool(Bootstrap->GetChunkManager()->IsNodeConfirmed(node)));
                return true;
            }

            if (key == "incarnation_id") {
                BuildYsonFluently(consumer)
                    .Scalar(node->GetIncarnationId());
                return true;
            }

            if (key == "statistics") {
                const auto& nodeStatistics = node->Statistics();
                BuildYsonFluently(consumer)
                    .BeginMap()
                        .Item("total_available_space").Scalar(nodeStatistics.total_available_space())
                        .Item("total_used_space").Scalar(nodeStatistics.total_used_space())
                        .Item("total_chunk_count").Scalar(nodeStatistics.total_chunk_count())
                        .Item("total_session_count").Scalar(node->GetTotalSessionCount())
                        .Item("full").Scalar(nodeStatistics.full())
                        .Item("locations").DoListFor(nodeStatistics.locations(), [] (TFluentList fluent, const NProto::TLocationStatistics& locationStatistics) {
                            fluent
                                .Item().BeginMap()
                                    .Item("available_space").Scalar(locationStatistics.available_space())
                                    .Item("used_space").Scalar(locationStatistics.used_space())
                                    .Item("chunk_count").Scalar(locationStatistics.chunk_count())
                                    .Item("session_count").Scalar(locationStatistics.session_count())
                                    .Item("full").Scalar(locationStatistics.full())
                                    .Item("enabled").Scalar(locationStatistics.enabled())
                                .EndMap();
                        })
                    .EndMap();
                return true;
            }
        }

        return TMapNodeProxy::GetSystemAttribute(key, consumer);
    }

    virtual void ValidateUserAttributeUpdate(
        const Stroka& key,
        const TNullable<TYsonString>& oldValue,
        const TNullable<TYsonString>& newValue) override
    {
        UNUSED(oldValue);

        if (key == "banned") {
            if (newValue) {
                ConvertTo<bool>(*newValue);
            }
        }
    }
};

class TNodeTypeHandler
    : public TMapNodeTypeHandler
{
public:
    explicit TNodeTypeHandler(TBootstrap* bootstrap)
        : TMapNodeTypeHandler(bootstrap)
    { }

    virtual EObjectType GetObjectType() override
    {
        return EObjectType::Node;
    }

    virtual ICypressNodeProxyPtr GetProxy(
        ICypressNode* trunkNode,
        TTransaction* transaction) override
    {
        return New<TNodeProxy>(
            this,
            Bootstrap,
            transaction,
            trunkNode);
    }
};

INodeTypeHandlerPtr CreateNodeTypeHandler(TBootstrap* bootstrap)
{
    YASSERT(bootstrap);

    return New<TNodeTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

class TNodeMapBehavior
    : public TNodeBehaviorBase<TMapNode, TMapNodeProxy>
{
public:
    TNodeMapBehavior(TBootstrap* bootstrap, const NCypressServer::TNodeId& nodeId)
        : TNodeBehaviorBase<TMapNode, TMapNodeProxy>(bootstrap, nodeId)
    {
        bootstrap->GetChunkManager()->SubscribeNodeRegistered(BIND(
            &TNodeMapBehavior::OnRegistered,
            MakeWeak(this)));
    }

private:
    void OnRegistered(const TDataNode* node)
    {
        Stroka address = node->GetAddress();

        auto metaStateFacade = Bootstrap->GetMetaStateFacade();

        // We're already in the state thread but need to postpone the planned changes and enqueue a callback.
        // Doing otherwise will turn node registration and Cypress update into a single
        // logged change, which is undesirable.
        BIND(&TNodeMapBehavior::CreateNodeIfNeeded, MakeStrong(this), address)
            .Via(metaStateFacade->GetEpochInvoker())
            .Run();
    }

    void CreateNodeIfNeeded(const Stroka& address)
    {
        auto proxy = GetProxy();

        if (proxy->FindChild(address))
            return;

        auto cypressManager = Bootstrap->GetCypressManager();
        auto service = cypressManager->GetVersionedNodeProxy(NodeId);

        // TODO(babenko): make a single transaction
        // TODO(babenko): check for errors and retry

        {
            auto req = TCypressYPathProxy::Create("/" + ToYPathLiteral(address));
            req->set_type(EObjectType::Node);
            ExecuteVerb(service, req);
        }

        {
            auto req = TCypressYPathProxy::Create("/" + ToYPathLiteral(address) + "/orchid");
            req->set_type(EObjectType::Orchid);

            auto attributes = CreateEphemeralAttributes();
            attributes->Set("remote_address", address);
            ToProto(req->mutable_node_attributes(), *attributes);

            ExecuteVerb(service, req);
        }
    }

};

class TNodeMapProxy
    : public TMapNodeProxy
{
public:
    TNodeMapProxy(
        INodeTypeHandlerPtr typeHandler,
        TBootstrap* bootstrap,
        NTransactionServer::TTransaction* transaction,
        ICypressNode* trunkNode)
        : TMapNodeProxy(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
    { }

private:
    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) const override
    {
        attributes->push_back("offline");
        attributes->push_back("registered");
        attributes->push_back("online");
        attributes->push_back("unconfirmed");
        attributes->push_back("confirmed");
        attributes->push_back("available_space");
        attributes->push_back("used_space");
        attributes->push_back("chunk_count");
        attributes->push_back("session_count");
        attributes->push_back("online_holder_count");
        attributes->push_back("chunk_replicator_enabled");
        TMapNodeProxy::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer) const override
    {
        auto chunkManager = Bootstrap->GetChunkManager();

        if (key == "offline") {
            BuildYsonFluently(consumer)
                .DoListFor(GetKeys(), [=] (TFluentList fluent, Stroka address) {
                    if (!chunkManager->FindNodeByAddress(address)) {
                        fluent.Item().Scalar(address);
                    }
            });
            return true;
        }

        if (key == "registered" || key == "online") {
            auto state = key == "registered" ? ENodeState::Registered : ENodeState::Online;
            BuildYsonFluently(consumer)
                .DoListFor(chunkManager->GetNodes(), [=] (TFluentList fluent, TDataNode* node) {
                    if (node->GetState() == state) {
                        fluent.Item().Scalar(node->GetAddress());
                    }
                });
            return true;
        }

        if (key == "unconfirmed" || key == "confirmed") {
            ValidateActiveLeader();
            bool state = key == "confirmed";
            BuildYsonFluently(consumer)
                .DoListFor(chunkManager->GetNodes(), [=] (TFluentList fluent, TDataNode* node) {
                    if (chunkManager->IsNodeConfirmed(node) == state) {
                        fluent.Item().Scalar(node->GetAddress());
                    }
                });
            return true;
        }

        auto statistics = chunkManager->GetTotalNodeStatistics();
        if (key == "available_space") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.AvailbaleSpace);
            return true;
        }

        if (key == "used_space") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.UsedSpace);
            return true;
        }

        if (key == "chunk_count") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.ChunkCount);
            return true;
        }

        if (key == "session_count") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.SessionCount);
            return true;
        }

        if (key == "online_holder_count") {
            BuildYsonFluently(consumer)
                .Scalar(statistics.OnlineNodeCount);
            return true;
        }

        if (key == "chunk_replicator_enabled") {
            ValidateActiveLeader();
            BuildYsonFluently(consumer)
                .Scalar(chunkManager->IsReplicatorEnabled());
            return true;
        }

        return TMapNodeProxy::GetSystemAttribute(key, consumer);
    }
};

class TNodeMapTypeHandler
    : public TMapNodeTypeHandler
{
public:
    explicit TNodeMapTypeHandler(TBootstrap* bootstrap)
        : TMapNodeTypeHandler(bootstrap)
    { }

    virtual EObjectType GetObjectType() override
    {
        return EObjectType::NodeMap;
    }
    
    virtual ICypressNodeProxyPtr GetProxy(
        ICypressNode* trunkNode,
        TTransaction* transaction) override
    {
        return New<TNodeMapProxy>(
            this,
            Bootstrap,
            transaction,
            trunkNode);
    }

    virtual INodeBehaviorPtr CreateBehavior(
        const NCypressServer::TNodeId& nodeId) override
    {
        return New<TNodeMapBehavior>(Bootstrap, nodeId);
    }
};

INodeTypeHandlerPtr CreateNodeMapTypeHandler(TBootstrap* bootstrap)
{
    YCHECK(bootstrap);

    return New<TNodeMapTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
