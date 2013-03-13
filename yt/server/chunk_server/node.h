#pragma once

#include "public.h"
#include "chunk_replica.h"

#include <ytlib/misc/property.h>

#include <ytlib/chunk_client/node_directory.h>

#include <server/cell_master/public.h>

#include <server/chunk_server/chunk_service.pb.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(ENodeState,
    // Not registered.
    (Offline)
    // Registered but did not report the full heartbeat yet.
    (Registered)
    // Registered and reported the full heartbeat.
    (Online)
);

class TDataNode
{
    DEFINE_BYVAL_RO_PROPERTY(TNodeId, Id);
    DEFINE_BYVAL_RW_PROPERTY(ENodeState, State);
    DEFINE_BYREF_RW_PROPERTY(NProto::TNodeStatistics, Statistics);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkPtrWithIndex>, StoredReplicas);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkPtrWithIndex>, CachedReplicas);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TChunkPtrWithIndex>, UnapprovedReplicas);
    DEFINE_BYREF_RO_PROPERTY(std::vector<TJob*>, Jobs);
    DEFINE_BYVAL_RW_PROPERTY(int, HintedSessionCount);

    //! Indexed by priority.
    typedef std::vector< yhash_set<TChunkId> > TChunksToReplicate;
    DEFINE_BYREF_RW_PROPERTY(TChunksToReplicate, ChunksToReplicate);

    //! NB: Ids are used instead of raw pointers since these chunks are typically already dead.
    typedef yhash_set<TChunkId> TChunksToRemove;
    DEFINE_BYREF_RW_PROPERTY(TChunksToRemove, ChunksToRemove);

public:
    TDataNode(
        TNodeId id,
        const NChunkClient::TNodeDescriptor& descriptor);

    explicit TDataNode(TNodeId id);

    const NChunkClient::TNodeDescriptor& GetDescriptor() const;
    const Stroka& GetAddress() const;

    void Save(const NCellMaster::TSaveContext& context) const;
    void Load(const NCellMaster::TLoadContext& context);

    void AddReplica(TChunkPtrWithIndex replica, bool cached);
    void RemoveReplica(TChunkPtrWithIndex replica, bool cached);
    bool HasReplica(TChunkPtrWithIndex, bool cached) const;

    void MarkReplicaUnapproved(TChunkPtrWithIndex replica);
    bool HasUnapprovedReplica(TChunkPtrWithIndex replica) const;
    void ApproveReplica(TChunkPtrWithIndex replica);

    void AddJob(TJob* job);
    void RemoveJob(TJob* id);

    int GetTotalSessionCount() const;

private:
    NChunkClient::TNodeDescriptor Descriptor_;

    void Init();

};

TNodeId GetObjectId(const TDataNode* node);
bool CompareObjectsForSerialization(const TDataNode* lhs, const TDataNode* rhs);

////////////////////////////////////////////////////////////////////////////////

class TReplicationSink
{
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Address);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJob*>, Jobs);

public:
    explicit TReplicationSink(const Stroka& address)
        : Address_(address)
    { }

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
