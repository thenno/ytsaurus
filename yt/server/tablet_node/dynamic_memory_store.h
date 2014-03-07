#pragma once

#include "public.h"
#include "store_detail.h"
#include "dynamic_memory_store_bits.h"

#include <core/misc/public.h>

#include <ytlib/transaction_client/public.h>

#include <ytlib/new_table_client/row_buffer.h>

#include <ytlib/chunk_client/chunk.pb.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TDynamicMemoryStore
    : public TStoreBase
{
public:
    TDynamicMemoryStore(
        TTabletManagerConfigPtr config,
        const TStoreId& id,
        TTablet* tablet);

    ~TDynamicMemoryStore();

    int GetLockCount() const;
    int Lock();
    int Unlock();

    TDynamicRow WriteRow(
        TTransaction* transaction,
        NVersionedTableClient::TUnversionedRow row,
        bool prewrite);

    TDynamicRow DeleteRow(
        TTransaction* transaction,
        NVersionedTableClient::TKey key,
        bool prewrite);

    TDynamicRow MigrateRow(
        TDynamicRow row,
        const TDynamicMemoryStorePtr& migrateTo);
    TDynamicRow FindRowAndCheckLocks(
        NVersionedTableClient::TKey key,
        TTransaction* transaction,
        ERowLockMode mode);

    void ConfirmRow(TDynamicRow row);
    void PrepareRow(TDynamicRow row);
    void CommitRow(TDynamicRow row);
    void AbortRow(TDynamicRow row);

    int GetValueCount() const;
    int GetKeyCount() const;
    
    i64 GetAlignedPoolSize() const;
    i64 GetAlignedPoolCapacity() const;

    i64 GetUnalignedPoolSize() const;
    i64 GetUnalignedPoolCapacity() const;

    // IStore implementation.
    virtual i64 GetDataSize() const override;

    virtual NVersionedTableClient::TOwningKey GetMinKey() const override;
    virtual NVersionedTableClient::TOwningKey GetMaxKey() const override;

    virtual NVersionedTableClient::IVersionedReaderPtr CreateReader(
        NVersionedTableClient::TOwningKey lowerKey,
        NVersionedTableClient::TOwningKey upperKey,
        TTimestamp timestamp,
        const NVersionedTableClient::TColumnFilter& columnFilter) override;

    virtual void Save(TSaveContext& context) const override;
    virtual void Load(TLoadContext& context) override;

    virtual void BuildOrchidYson(NYson::IYsonConsumer* consumer) override;

private:
    class TReader;

    TTabletManagerConfigPtr Config_;

    int LockCount_;

    int KeyColumnCount_;
    int SchemaColumnCount_;

    int ValueCount_;

    NVersionedTableClient::TRowBuffer RowBuffer_;
    std::unique_ptr<TSkipList<TDynamicRow, NVersionedTableClient::TKeyComparer>> Rows_;


    TDynamicRow AllocateRow();
    
    void CheckRowLock(
        TDynamicRow row,
        TTransaction* transaction,
        ERowLockMode mode);
    bool LockRow(
        TDynamicRow row,
        TTransaction* transaction,
        ERowLockMode mode,
        bool prewrite);

    void DropUncommittedValues(TDynamicRow row);

    void AddFixedValue(
        TDynamicRow row,
        int listIndex,
        const NVersionedTableClient::TVersionedValue& value);
    void AddUncommittedFixedValue(
        TDynamicRow row,
        int listIndex,
        const NVersionedTableClient::TUnversionedValue& value);

    void AddTimestamp(TDynamicRow row, TTimestamp timestamp);
    void AddUncommittedTimestamp(TDynamicRow row, TTimestamp timestamp);

    void CaptureValue(
        NVersionedTableClient::TUnversionedValue* dst,
        const NVersionedTableClient::TUnversionedValue& src);
    void CaptureValue(
        NVersionedTableClient::TVersionedValue* dst,
        const NVersionedTableClient::TVersionedValue& src);
    void CaptureValueData(
        NVersionedTableClient::TUnversionedValue* dst,
        const NVersionedTableClient::TUnversionedValue& src);

};

DEFINE_REFCOUNTED_TYPE(TDynamicMemoryStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
