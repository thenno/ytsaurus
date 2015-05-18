#pragma once

#include "public.h"

#include <core/ypath/public.h>

#include <ytlib/api/public.h>

#include <unordered_map>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

struct IFunctionRegistry
    : public virtual TRefCounted
{
    virtual IFunctionDescriptorPtr FindFunction(const Stroka& functionName) = 0;

    IFunctionDescriptorPtr GetFunction(const Stroka& functionName);

    virtual IAggregateFunctionDescriptorPtr FindAggregateFunction(const Stroka& aggregateName) = 0;

    IAggregateFunctionDescriptorPtr GetAggregateFunction(const Stroka& aggregateName);
};

DEFINE_REFCOUNTED_TYPE(IFunctionRegistry)

////////////////////////////////////////////////////////////////////////////////

class TFunctionRegistry
    : public IFunctionRegistry
{
public:
    void RegisterFunction(IFunctionDescriptorPtr descriptor);

    virtual IFunctionDescriptorPtr FindFunction(const Stroka& functionName) override;

    void RegisterAggregateFunction(IAggregateFunctionDescriptorPtr descriptor);

    virtual IAggregateFunctionDescriptorPtr FindAggregateFunction(const Stroka& aggregateName) override;

private:
    std::unordered_map<Stroka, IFunctionDescriptorPtr> RegisteredFunctions_;
    std::unordered_map<Stroka, IAggregateFunctionDescriptorPtr> RegisteredAggregateFunctions_;
};

DEFINE_REFCOUNTED_TYPE(TFunctionRegistry)

////////////////////////////////////////////////////////////////////////////////

class TCypressFunctionRegistry
    : public IFunctionRegistry
{
public:
    TCypressFunctionRegistry(
        NApi::IClientPtr client,
        const NYPath::TYPath& registryPath,
        TFunctionRegistryPtr builtinRegistry);

    virtual IFunctionDescriptorPtr FindFunction(const Stroka& functionName) override;

    virtual IAggregateFunctionDescriptorPtr FindAggregateFunction(const Stroka& aggregateName) override;

private:
    const NApi::IClientPtr Client_;
    const NYPath::TYPath RegistryPath_;
    const TFunctionRegistryPtr BuiltinRegistry_;
    const TFunctionRegistryPtr UdfRegistry_;

    void LookupAndRegisterFunction(const Stroka& functionName);
    void LookupAndRegisterAggregate(const Stroka& aggregateName);
};

////////////////////////////////////////////////////////////////////////////////

IFunctionRegistryPtr CreateBuiltinFunctionRegistry();
IFunctionRegistryPtr CreateFunctionRegistry(NApi::IClientPtr client);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
