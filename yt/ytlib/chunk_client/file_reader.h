#pragma once

#include "chunk_reader.h"
#include "io_engine.h"

#include <yt/ytlib/chunk_client/chunk_meta.pb.h>

#include <util/system/file.h>
#include <util/system/mutex.h>

#include <atomic>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

//! Provides a local and synchronous implementation of IReader.
class TFileReader
    : public IChunkReader
{
public:
    //! Creates a new reader.
    /*!
     *  For chunk meta version 2+, #chunkId is validated against that stored
     *  in the meta file. Passing #NullChunkId in #chunkId suppresses this check.
     */
    TFileReader(
        const IIOEnginePtr& ioEngine,
        const TChunkId& chunkId,
        const TString& fileName,
        bool validateBlocksChecksums = true);

    // IReader implementation.
    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        const std::vector<int>& blockIndexes) override;

    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        int firstBlockIndex,
        int blockCount) override;

    virtual TFuture<NProto::TChunkMeta> GetMeta(
        const TWorkloadDescriptor& workloadDescriptor,
        const TNullable<int>& partitionTag = Null,
        const TNullable<std::vector<int>>& extensionTags = Null) override;

    virtual TChunkId GetChunkId() const override;

    virtual bool IsValid() const override;

private:
    const IIOEnginePtr IOEngine_;
    const TChunkId ChunkId_;
    const TString FileName_;
    const bool ValidateBlockChecksums_;

    TMutex Mutex_;
    std::atomic<bool> HasCachedDataFile_ = {false};
    std::shared_ptr<TFileHandle> CachedDataFile_;
    std::atomic<bool> HasCachedBlocksExt_ = {false};
    TFuture<NProto::TBlocksExt> CachedBlocksExt_;

    TFuture<std::vector<TBlock>> DoReadBlocks(int firstBlockIndex, int blockCount);
    std::vector<TBlock> OnDataBlock(
        int firstBlockIndex,
        int blockCount,
        const TSharedMutableRef& data);
    TFuture<NProto::TChunkMeta> DoGetMeta(
        const TNullable<int>& partitionTag,
        const TNullable<std::vector<int>>& extensionTags);
    NProto::TChunkMeta OnMetaDataBlock(const TString& metaFileName, const TSharedMutableRef& data);

    const NProto::TBlocksExt& GetBlockExts();
    const std::shared_ptr<TFileHandle>& GetDataFile();
};

DEFINE_REFCOUNTED_TYPE(TFileReader)

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
