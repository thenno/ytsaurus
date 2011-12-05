#include "stdafx.h"
#include "chunk.h"

namespace NYT {
namespace NChunkServer {

using namespace NProto;
using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

TChunk::TChunk(const TChunkId& id)
    : Id_(id)
    , MetaChecksum_(UnknownChecksum)
    , RefCounter(0)
{ }

TChunk::TChunk(const TChunk& other)
    : Id_(other.Id_)
    , ChunkListId_(other.ChunkListId_)
    , MetaChecksum_(other.MetaChecksum_)
    , ChunkInfo_(ChunkInfo_)
    , Locations_(other.Locations_)
    , RefCounter(other.RefCounter)
{ }

TAutoPtr<TChunk> TChunk::Clone() const
{
    return new TChunk(*this);
}

void TChunk::Save(TOutputStream* output) const
{
    ::Save(output, ChunkListId_);
    ::Save(output, MetaChecksum_);
    ::Save(output, ChunkInfo_);
    ::Save(output, Locations_);
    ::Save(output, RefCounter);

}

TAutoPtr<TChunk> TChunk::Load(const TChunkId& id, TInputStream* input)
{
    TAutoPtr<TChunk> chunk = new TChunk(id);
    ::Load(input, chunk->ChunkListId_);
    ::Load(input, chunk->MetaChecksum_);
    ::Load(input, chunk->ChunkInfo_);
    ::Load(input, chunk->Locations_);
    ::Load(input, chunk->RefCounter);
    return chunk;
}


void TChunk::AddLocation(THolderId holderId)
{
    Locations_.push_back(holderId);
}

void TChunk::RemoveLocation(THolderId holderId)
{
    auto it = std::find(Locations_.begin(), Locations_.end(), holderId);
    YASSERT(it != Locations_.end());
    Locations_.erase(it);
}

i32 TChunk::Ref()
{
    return ++RefCounter;
}

i32 TChunk::Unref()
{
    return --RefCounter;
}

i32 TChunk::GetRefCounter() const
{
    return RefCounter;
}

TChunkInfo TChunk::DeserializeChunkInfo() const
{
    TChunkInfo chunkInfo;
    if (ChunkInfo_ == TSharedRef()) {
        // Did not receive metainfo from the holders, let's make a fake one.
        chunkInfo.set_id(Id_.ToProto());
        chunkInfo.set_size(-1);
        chunkInfo.mutable_attributes()->set_type(EChunkType::Unknown);
    } else {
        // Deserialize the blob received from the holders.
        if (!DeserializeProtobuf(&chunkInfo, ChunkInfo_)) {
            LOG_FATAL("Error deserializing chunk info (ChunkId: %s)", ~Id_.ToString());
        }
    }
    return chunkInfo;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
