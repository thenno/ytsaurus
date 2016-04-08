#pragma once

#include "public.h"

#include <yt/core/misc/nullable.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! A thread-safe id-to-name mapping.
class TNameTable
    : public virtual TRefCounted
{
public:
    static TNameTablePtr FromSchema(const TTableSchema& schema);
    static TNameTablePtr FromKeyColumns(const TKeyColumns& keyColumns);

    int GetSize() const;
    i64 GetByteSize() const;

    TNullable<int> FindId(const TStringBuf& name) const;

    int GetId(const TStringBuf& name) const;
    int RegisterName(const TStringBuf& name);
    int GetIdOrRegisterName(const TStringBuf& name);

    TStringBuf GetName(int id) const;

private:
    TSpinLock SpinLock_;

    std::vector<Stroka> IdToName_;
    yhash_map<TStringBuf, int> NameToId_; // String values are owned by IdToName.
    i64 ByteSize_ = 0;

    int DoRegisterName(const TStringBuf& name);

};

DEFINE_REFCOUNTED_TYPE(TNameTable)

////////////////////////////////////////////////////////////////////////////////

//! A non thread-safe read-only wrapper for TNameTable.
class TNameTableReader
    : private TNonCopyable
{
public:
    explicit TNameTableReader(TNameTablePtr nameTable);

    TStringBuf GetName(int id) const;
    int GetSize() const;

private:
    const TNameTablePtr NameTable_;

    mutable std::vector<Stroka> IdToNameCache_;

    void Fill() const;

};

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TNameTableExt* protoNameTable, const TNameTablePtr& nameTable);
void FromProto(TNameTablePtr* nameTable, const NProto::TNameTableExt& protoNameTable);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
