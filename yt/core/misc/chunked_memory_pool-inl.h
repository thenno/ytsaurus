#ifndef CHUNKED_MEMORY_POOL_INL_H_
#error "Direct inclusion of this file is not allowed, include chunked_memory_pool.h"
#endif
#undef CHUNKED_MEMORY_POOL_INL_H_

#include "serialize.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

inline char* TChunkedMemoryPool::AllocateUnaligned(i64 size)
{
    // Fast path.
    if (FreeZoneEnd_ >= FreeZoneBegin_ + size) {
        FreeZoneEnd_ -= size;
        Size_ += size;
        return FreeZoneEnd_;
    }

    // Slow path.
    return AllocateUnalignedSlow(size);
}

inline char* TChunkedMemoryPool::AllocateAligned(i64 size, int align)
{
    // NB: This can lead to FreeZoneBegin_ >= FreeZoneEnd_ in which case the chunk is full.
    FreeZoneBegin_ = AlignUp(FreeZoneBegin_, align);

    // Fast path.
    if (FreeZoneBegin_ + size <= FreeZoneEnd_) {
        char* result = FreeZoneBegin_;
        Size_ += size;
        FreeZoneBegin_ += size;
        return result;
    }

    // Slow path.
    return AllocateAlignedSlow(size, align);
}

template <class T>
inline T* TChunkedMemoryPool::AllocateUninitialized(int n, int align)
{
    return reinterpret_cast<T*>(AllocateAligned(sizeof(T) * n, align));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
