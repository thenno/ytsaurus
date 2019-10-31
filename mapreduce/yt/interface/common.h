#pragma once

#include "fwd.h"

#include <library/yson/node/node.h>

#include <util/generic/guid.h>
#include <util/generic/map.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/type_name.h>
#include <util/generic/vector.h>

#include <contrib/libs/protobuf/message.h>

#include <statbox/ti/ti.h>

#include <initializer_list>
#include <type_traits>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

#define FLUENT_FIELD(type, name) \
    type name##_; \
    TSelf& name(const type& value) \
    { \
        name##_ = value; \
        return static_cast<TSelf&>(*this); \
    }

#define FLUENT_FIELD_OPTION(type, name) \
    TMaybe<type> name##_; \
    TSelf& name(const type& value) \
    { \
        name##_ = value; \
        return static_cast<TSelf&>(*this); \
    }

#define FLUENT_FIELD_DEFAULT(type, name, defaultValue) \
    type name##_ = defaultValue; \
    TSelf& name(const type& value) \
    { \
        name##_ = value; \
        return static_cast<TSelf&>(*this); \
    }

#define FLUENT_VECTOR_FIELD(type, name) \
    TVector<type> name##s_; \
    TSelf& Add##name(const type& value) \
    { \
        name##s_.push_back(value); \
        return static_cast<TSelf&>(*this);\
    }

#define FLUENT_MAP_FIELD(keytype, valuetype, name) \
    TMap<keytype,valuetype> name##_; \
    TSelf& Add##name(const keytype& key, const valuetype& value) \
    { \
        name##_.emplace(key, value); \
        return static_cast<TSelf&>(*this);\
    }

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TOneOrMany
{
    TOneOrMany()
    { }

    TOneOrMany(const TOneOrMany& rhs)
    {
        Parts_ = rhs.Parts_;
    }

    TOneOrMany& operator=(const TOneOrMany& rhs)
    {
        Parts_ = rhs.Parts_;
        return *this;
    }

    TOneOrMany(TOneOrMany&& rhs)
    {
        Parts_ = std::move(rhs.Parts_);
    }

    TOneOrMany& operator=(TOneOrMany&& rhs)
    {
        Parts_ = std::move(rhs.Parts_);
        return *this;
    }

    template<class U>
    TOneOrMany(std::initializer_list<U> il)
    {
        Parts_.assign(il.begin(), il.end());
    }

    template <class U, class... TArgs, std::enable_if_t<std::is_convertible<U, T>::value, int> = 0>
    TOneOrMany(U&& arg, TArgs&&... args)
    {
        Add(arg, std::forward<TArgs>(args)...);
    }

    TOneOrMany(TVector<T> args)
        : Parts_(std::move(args))
    { }

    bool operator==(const TOneOrMany& rhs) const {
        return Parts_ == rhs.Parts_;
    }

    template <class U, class... TArgs>
    TOneOrMany& Add(U&& part, TArgs&&... args) &
    {
        Parts_.push_back(std::forward<U>(part));
        return Add(std::forward<TArgs>(args)...);
    }

    template <class... TArgs>
    TOneOrMany Add(TArgs&&... args) &&
    {
        return std::move(Add(std::forward<TArgs>(args)...));
    }

    TOneOrMany& Add() &
    {
        return *this;
    }

    TOneOrMany Add() &&
    {
        return std::move(*this);
    }

    TVector<T> Parts_;
};

////////////////////////////////////////////////////////////////////////////////

enum EValueType : int
{
    VT_INT64,

    VT_UINT64,

    VT_DOUBLE,
    VT_BOOLEAN,

    VT_STRING,

    VT_ANY,

    VT_INT8,
    VT_INT16,
    VT_INT32,

    VT_UINT8,
    VT_UINT16,
    VT_UINT32,

    VT_UTF8,
};

enum ESortOrder : int
{
    SO_ASCENDING    /* "ascending" */,
    SO_DESCENDING   /* "descending" */,
};

enum EOptimizeForAttr : i8
{
    OF_SCAN_ATTR    /* "scan" */,
    OF_LOOKUP_ATTR  /* "lookup" */,
};

enum EErasureCodecAttr : i8
{
    EC_NONE_ATTR                /* "none" */,
    EC_REED_SOLOMON_6_3_ATTR    /* "reed_solomon_6_3" */,
    EC_LRC_12_2_2_ATTR          /* "lrc_12_2_2" */,
};

struct TColumnSchema
{
    using TSelf = TColumnSchema;

    FLUENT_FIELD(TString, Name);
    FLUENT_FIELD_DEFAULT(EValueType, Type, VT_INT64);

    // If Required is set to true "null" values are not allowed in this column.
    // Column of type "any" cannot have required=true attribute.
    // Dynamic tables cannot have columns with required=true attribute.
    FLUENT_FIELD_DEFAULT(bool, Required, false);

    // Experimental feature
    FLUENT_FIELD_OPTION(TNode, RawTypeV2);

    FLUENT_FIELD_OPTION(ESortOrder, SortOrder);
    FLUENT_FIELD_OPTION(TString, Lock);
    FLUENT_FIELD_OPTION(TString, Expression);
    FLUENT_FIELD_OPTION(TString, Aggregate);
    FLUENT_FIELD_OPTION(TString, Group);
};

struct TTableSchema
{
public:
    using TSelf = TTableSchema;

    FLUENT_VECTOR_FIELD(TColumnSchema, Column);
    FLUENT_FIELD_DEFAULT(bool, Strict, true);
    FLUENT_FIELD_DEFAULT(bool, UniqueKeys, false);

    bool Empty() const;

public:
    // Some helper methods

    TTableSchema& AddColumn(const TString& name, EValueType type) &;
    TTableSchema AddColumn(const TString& name, EValueType type) &&;

    TTableSchema& AddColumn(const TString& name, EValueType type, ESortOrder sortOrder) &;
    TTableSchema AddColumn(const TString& name, EValueType type, ESortOrder sortOrder) &&;

    TTableSchema& SortBy(const TVector<TString>& columns) &;
    TTableSchema SortBy(const TVector<TString>& columns) &&;

    TNode ToNode() const;
};

TTableSchema CreateTableSchema(
    const ::google::protobuf::Descriptor& messageDescriptor,
    const TKeyColumns& keyColumns = TKeyColumns(),
    bool keepFieldsWithoutExtension = true);

template<class TProtoType>
inline TTableSchema CreateTableSchema(
    const TKeyColumns& keyColumns = TKeyColumns(),
    bool keepFieldsWithoutExtension = true)
{
    static_assert(
        std::is_base_of_v<::google::protobuf::Message, TProtoType>,
        "Should be base of google::protobuf::Message");

    return CreateTableSchema(
        *TProtoType::descriptor(),
        keyColumns,
        keepFieldsWithoutExtension);
}

TTableSchema CreateTableSchema(NTi::TType::TPtr type);

////////////////////////////////////////////////////////////////////////////////

struct TReadLimit
{
    using TSelf = TReadLimit;

    FLUENT_FIELD_OPTION(TKey, Key);
    FLUENT_FIELD_OPTION(i64, RowIndex);
    FLUENT_FIELD_OPTION(i64, Offset);
};

struct TReadRange
{
    using TSelf = TReadRange;

    FLUENT_FIELD(TReadLimit, LowerLimit);
    FLUENT_FIELD(TReadLimit, UpperLimit);
    FLUENT_FIELD(TReadLimit, Exact);

    static TReadRange FromRowIndices(i64 lowerLimit, i64 upperLimit)
    {
        return TReadRange()
            .LowerLimit(TReadLimit().RowIndex(lowerLimit))
            .UpperLimit(TReadLimit().RowIndex(upperLimit));
    }
};

struct TRichYPath
{
    using TSelf = TRichYPath;

    FLUENT_FIELD(TYPath, Path);

    FLUENT_FIELD_OPTION(bool, Append);
    FLUENT_FIELD(TKeyColumns, SortedBy);

    FLUENT_VECTOR_FIELD(TReadRange, Range);


    // Specifies columns that should be read.
    // If it's set to Nothing then all columns will be read.
    // If empty TKeyColumns is specified then each read row will be empty.
    FLUENT_FIELD_OPTION(TKeyColumns, Columns);

    FLUENT_FIELD_OPTION(bool, Teleport);
    FLUENT_FIELD_OPTION(bool, Primary);
    FLUENT_FIELD_OPTION(bool, Foreign);
    FLUENT_FIELD_OPTION(i64, RowCountLimit);

    FLUENT_FIELD_OPTION(TString, FileName);
    FLUENT_FIELD_OPTION(TYPath, OriginalPath);
    FLUENT_FIELD_OPTION(bool, Executable);
    FLUENT_FIELD_OPTION(TNode, Format);
    FLUENT_FIELD_OPTION(TTableSchema, Schema);

    FLUENT_FIELD_OPTION(TString, CompressionCodec);
    FLUENT_FIELD_OPTION(EErasureCodecAttr, ErasureCodec);
    FLUENT_FIELD_OPTION(EOptimizeForAttr, OptimizeFor);

    // Timestamp of dynamic table.
    // NOTE: it is _not_ unix timestamp
    // (instead it's transaction timestamp, that is more complex structure).
    FLUENT_FIELD_OPTION(i64, Timestamp);

    // Specifiy transaction that should be used to access this path.
    // Allows to start cross-transactional operations.
    FLUENT_FIELD_OPTION(TTransactionId, TransactionId);

    // Specifies columnar mapping which will be applied to columns before transfer to job.
    using TRenameColumnsDescriptor = THashMap<TString, TString>;
    FLUENT_FIELD_OPTION(TRenameColumnsDescriptor, RenameColumns);

    TRichYPath()
    { }

    TRichYPath(const char* path)
        : Path_(path)
    { }

    TRichYPath(const TYPath& path)
        : Path_(path)
    { }
};

template <typename TProtoType>
TRichYPath WithSchema(const TRichYPath& path, const TKeyColumns& sortBy = TKeyColumns())
{
    auto schemedPath = path;
    if (!schemedPath.Schema_) {
        schemedPath.Schema(CreateTableSchema<TProtoType>(sortBy));
    }
    return schemedPath;
}

template <typename TRowType>
TRichYPath MaybeWithSchema(const TRichYPath& path, const TKeyColumns& sortBy = TKeyColumns())
{
    if constexpr (std::is_base_of_v<::google::protobuf::Message, TRowType>) {
        return WithSchema<TRowType>(path, sortBy);
    } else {
        return path;
    }
}

////////////////////////////////////////////////////////////////////////////////

// Statistics about table columns.
struct TTableColumnarStatistics
{
    // Total data weight for all chunks for each of requested columns.
    THashMap<TString, i64> ColumnDataWeight;

    // Total weight of all old chunks that don't keep columnar statitics.
    i64 LegacyChunksDataWeight = 0;

    // Timestamps total weight (only for dynamic tables).
    TMaybe<i64> TimestampTotalWeight;
};

////////////////////////////////////////////////////////////////////////////////

struct TAttributeFilter
{
    using TSelf = TAttributeFilter;

    FLUENT_VECTOR_FIELD(TString, Attribute);
};

////////////////////////////////////////////////////////////////////////////////

bool IsTrivial(const TReadLimit& readLimit);

EValueType NodeTypeToValueType(TNode::EType nodeType);

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

// MUST NOT BE USED BY CLIENTS
// TODO: we should use default GENERATE_ENUM_SERIALIZATION
TString ToString(EValueType type);

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
