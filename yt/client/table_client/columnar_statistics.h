#pragma once

#include "public.h"

#include <yt/core/misc/optional.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TLightweightColumnarStatistics
{
    //! Sum of per-column data weight for chunks whose meta contains columnar statistics.
    i64 ColumnDataWeightsSum = 0;
    //! Total weight of all write and delete timestamps.
    std::optional<i64> TimestampTotalWeight;
    //! Total data weight of legacy chunks whose meta misses columnar statistics.
    i64 LegacyChunkDataWeight = 0;
};

struct TColumnarStatistics
{
    //! Per-column total data weight for chunks whose meta contains columnar statistics.
    std::vector<i64> ColumnDataWeights;
    //! Total weight of all write and delete timestamps.
    std::optional<i64> TimestampTotalWeight;
    //! Total data weight of legacy chunks whose meta misses columnar statistics.
    i64 LegacyChunkDataWeight = 0;

    TColumnarStatistics& operator +=(const TColumnarStatistics& other);

    static TColumnarStatistics MakeEmpty(int columnCount);

    TLightweightColumnarStatistics MakeLightweightStatistics() const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
