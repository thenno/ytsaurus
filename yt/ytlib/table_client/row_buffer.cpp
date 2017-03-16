#include "row_buffer.h"
#include "unversioned_row.h"
#include "versioned_row.h"

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

TRowBuffer::TRowBuffer(
    i64 chunkSize,
    double maxSmallBlockRatio,
    TRefCountedTypeCookie tagCookie)
    : Pool_(
        chunkSize,
        maxSmallBlockRatio,
        tagCookie)
{ }

TChunkedMemoryPool* TRowBuffer::GetPool()
{
    return &Pool_;
}

TMutableUnversionedRow TRowBuffer::Allocate(int count)
{
    return TMutableUnversionedRow::Allocate(&Pool_, count);
}

void TRowBuffer::Capture(TUnversionedValue* value)
{
    if (IsStringLikeType(value->Type)) {
        char* dst = Pool_.AllocateUnaligned(value->Length);
        memcpy(dst, value->Data.String, value->Length);
        value->Data.String = dst;
    }
}

TVersionedValue TRowBuffer::Capture(const TVersionedValue& value)
{
    auto capturedValue = value;
    Capture(&capturedValue);
    return capturedValue;
}

TUnversionedValue TRowBuffer::Capture(const TUnversionedValue& value)
{
    auto capturedValue = value;
    Capture(&capturedValue);
    return capturedValue;
}

TMutableUnversionedRow TRowBuffer::Capture(TUnversionedRow row, bool deep)
{
    if (!row) {
        return TMutableUnversionedRow();
    }

    return Capture(row.Begin(), row.GetCount(), deep);
}

TMutableUnversionedRow TRowBuffer::Capture(const TUnversionedValue* begin, int count, bool deep)
{
    auto capturedRow = TMutableUnversionedRow::Allocate(&Pool_, count);
    auto* capturedBegin = capturedRow.Begin();

    ::memcpy(capturedBegin, begin, count * sizeof (TUnversionedValue));

    if (deep) {
        for (int index = 0; index < count; ++index) {
            Capture(&capturedBegin[index]);
        }
    }

    return capturedRow;
}

std::vector<TMutableUnversionedRow> TRowBuffer::Capture(const TRange<TUnversionedRow>& rows, bool deep)
{
    int rowCount = static_cast<int>(rows.Size());
    std::vector<TMutableUnversionedRow> capturedRows(rowCount);
    for (int index = 0; index < rowCount; ++index) {
        capturedRows[index] = Capture(rows[index], deep);
    }
    return capturedRows;
}

TMutableUnversionedRow TRowBuffer::CaptureAndPermuteRow(
    TUnversionedRow row,
    const TTableSchema& tableSchema,
    const TNameTableToSchemaIdMapping& idMapping)
{
    int keyColumnCount = tableSchema.GetKeyColumnCount();
    int columnCount = keyColumnCount;

    for (const auto& value : row) {
        ui16 originalId = value.Id;
        YCHECK(originalId < idMapping.size());
        int mappedId = idMapping[originalId];
        if (mappedId < 0) {
            continue;
        }
        YCHECK(mappedId < tableSchema.Columns().size());
        if (mappedId >= keyColumnCount) {
            ++columnCount;
        }
    }

    auto capturedRow = TMutableUnversionedRow::Allocate(&Pool_, columnCount);
    columnCount = keyColumnCount;

    for (const auto& value : row) {
        ui16 originalId = value.Id;
        int mappedId = idMapping[originalId];
        if (mappedId < 0) {
            continue;
        }
        int pos = mappedId < keyColumnCount ? mappedId : columnCount++;
        capturedRow[pos] = value;
        capturedRow[pos].Id = mappedId;
    }

    return capturedRow;
}

i64 TRowBuffer::GetSize() const
{
    return Pool_.GetSize();
}

i64 TRowBuffer::GetCapacity() const
{
    return Pool_.GetCapacity();
}

void TRowBuffer::Clear()
{
    Pool_.Clear();
}

void TRowBuffer::Purge()
{
    Pool_.Purge();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
