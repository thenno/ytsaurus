#pragma once

#include <yt/ytlib/tablet_client/table_replica_ypath.pb.h>

#include <yt/core/ytree/ypath_proxy.h>

namespace NYT {
namespace NTabletClient {

////////////////////////////////////////////////////////////////////////////////

struct TTableReplicaYPathProxy
    : public NYTree::TYPathProxy
{
    static Stroka GetServiceName()
    {
        return "TableReplica";
    }

    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Enable);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, Disable);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletClient
} // namespace NYT
