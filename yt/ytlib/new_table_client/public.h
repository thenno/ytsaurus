#pragma once

#include <core/misc/common.h>
#include <core/misc/enum.h>
#include <core/misc/small_vector.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/table_client/public.h>

#include <ytlib/chunk_client/public.h>

#include <initializer_list>

// TODO(babenko): remove after migration
namespace NYT { namespace NTableClient { namespace NProto {
    class TKeyColumnsExt;
} } }

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TNameTableExt;
class TColumnSchema;
class TTableSchemaExt;
using NTableClient::NProto::TKeyColumnsExt; // TODO(babenko): remove after migration
class TBoundaryKeysExt;
class TBlockIndexesExt;
class TBlockMetaExt;
class TBlockMeta;
class TSimpleVersionedBlockMeta;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

using NTransactionClient::TTimestamp;
using NTransactionClient::NullTimestamp;
using NTransactionClient::MinTimestamp;
using NTransactionClient::MaxTimestamp;
using NTransactionClient::LastCommittedTimestamp;
using NTransactionClient::AllCommittedTimestamp;

using NTableClient::TKeyColumns; // TODO(babenko): remove after migration

////////////////////////////////////////////////////////////////////////////////

const int TypicalColumnCount = 64;
const int MaxKeyColumnCount = 64;
const int MaxValuesPerRow = 1024;
const int MaxRowsPerRowset = 1024 * 1024;
const i64 MaxStringValueLength = (i64) 1024 * 1024; // 1 MB

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EValueType,
    ((Min)         (0x00))

    ((TheBottom)   (0x01))
    ((Null)        (0x02))

    ((Int64)       (0x03))
    ((Uint64)      (0x04))
    ((Double)      (0x05))
    ((Boolean)     (0x06))

    ((String)      (0x10))
    ((Any)         (0x11))

    ((Max)         (0xef))
);

DECLARE_ENUM(ETableChunkFormat,
    ((Old)                  (1))
    ((VersionedSimple)      (2))
    ((Schemaful)            (3))
    ((SchemalessHorizontal) (4))
);

struct TColumnIdMapping
{
    int ChunkSchemaIndex;
    int ReaderSchemaIndex;
};

struct TColumnFilter
{
    TColumnFilter()
        : All(true)
    { }

    TColumnFilter(const std::initializer_list<int>& indexes)
        : All(false)
        , Indexes(indexes.begin(), indexes.end())
    { }

    bool All;
    SmallVector<int, TypicalColumnCount> Indexes;
};

typedef SmallVector<int, TypicalColumnCount> TNameTableToSchemaIdMapping;

union TUnversionedValueData;

struct TUnversionedValue;
struct TVersionedValue;

struct TUnversionedRowHeader;
struct TVersionedRowHeader;

class TUnversionedRow;
class TUnversionedOwningRow;

class TVersionedRow;
class TVersionedOwningRow;

typedef TUnversionedRow TKey;
typedef TUnversionedOwningRow TOwningKey;

class TUnversionedRowBuilder;
class TUnversionedOwningRowBuilder;

class TKeyComparer;

struct TColumnSchema;
class TTableSchema;

DECLARE_REFCOUNTED_CLASS(TNameTable)

class TRowBuffer;

struct  IBlockWriter;

class TBlockWriter;

DECLARE_REFCOUNTED_CLASS(TSamplesFetcher)
DECLARE_REFCOUNTED_CLASS(TChunkSplitsFetcher)

DECLARE_REFCOUNTED_STRUCT(ISchemafulReader)
DECLARE_REFCOUNTED_STRUCT(ISchemafulWriter)
DECLARE_REFCOUNTED_CLASS(TSchemafulPipe)

DECLARE_REFCOUNTED_STRUCT(ISchemalessReader)
DECLARE_REFCOUNTED_STRUCT(ISchemalessWriter)

//DECLARE_REFCOUNTED_STRUCT(ISchemalessChunkReader)
DECLARE_REFCOUNTED_STRUCT(ISchemalessChunkWriter)

//DECLARE_REFCOUNTED_STRUCT(ISchemalessChunkReader)
DECLARE_REFCOUNTED_STRUCT(ISchemalessMultiChunkWriter)

DECLARE_REFCOUNTED_STRUCT(IVersionedReader)
DECLARE_REFCOUNTED_STRUCT(IVersionedWriter)

DECLARE_REFCOUNTED_STRUCT(IVersionedChunkWriter)
DECLARE_REFCOUNTED_STRUCT(IVersionedMultiChunkWriter)

DECLARE_REFCOUNTED_CLASS(TCachedVersionedChunkMeta)

DECLARE_REFCOUNTED_STRUCT(IVersionedReader)

typedef NChunkClient::TEncodingWriterOptions TChunkWriterOptions;
typedef NChunkClient::TEncodingWriterOptionsPtr TChunkWriterOptionsPtr;

typedef NChunkClient::TMultiChunkWriterOptions TTableWriterOptions;
typedef NChunkClient::TMultiChunkWriterOptionsPtr TTableWriterOptionsPtr;

DECLARE_REFCOUNTED_CLASS(TChunkWriterConfig)
DECLARE_REFCOUNTED_CLASS(TChunkReaderConfig)

DECLARE_REFCOUNTED_CLASS(TTableWriterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
