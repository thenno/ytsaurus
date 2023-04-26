#ifndef SERVICE_INL_H_
#error "Direct inclusion of this file is not allowed, include service.h"
// For the sake of sane code completion.
#include "service.h"
#endif

#include "helpers.h"

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

template <class... TArgs>
void IServiceContext::SetRequestInfo(const char* format, TArgs&&... args)
{
    if (IsLoggingEnabled()) {
        SetRawRequestInfo(Format(format, std::forward<TArgs>(args)...), /*incremental*/ false);
    } else {
        // Still need to set the info to make AlertOnMissingRequestInfo check happy.
        SetRawRequestInfo(TString(), /*incremental*/ false);
    }
}

template <class... TArgs>
void IServiceContext::SetIncrementalRequestInfo(const char* format, TArgs&&... args)
{
    if (IsLoggingEnabled()) {
        SetRawRequestInfo(Format(format, std::forward<TArgs>(args)...), /*incremental*/ true);
    } else {
        // Still need to set the info to make AlertOnMissingRequestInfo check happy.
        SetRawRequestInfo(TString(), /*incremental*/ true);
    }
}

template <class... TArgs>
void IServiceContext::SetResponseInfo(const char* format, TArgs&&... args)
{
    if (IsLoggingEnabled()) {
        SetRawResponseInfo(Format(format, std::forward<TArgs>(args)...), /*incremental*/ false);
    }
}

template <class... TArgs>
void IServiceContext::SetIncrementalResponseInfo(const char* format, TArgs&&... args)
{
    if (IsLoggingEnabled()) {
        SetRawResponseInfo(Format(format, std::forward<TArgs>(args)...), /*incremental*/ true);
    }
}

namespace NDetail {

bool IsClientFeatureSupported(const IServiceContext* context, int featureId);
void ThrowUnsupportedClientFeature(int featureId, TStringBuf featureName);

} // namespace NDetail

template <class E>
bool IServiceContext::IsClientFeatureSupported(E featureId) const
{
    return NDetail::IsClientFeatureSupported(this, FeatureIdToInt(featureId));
}

template <class E>
void IServiceContext::ValidateClientFeature(E featureId) const
{
    auto intFeatureId = FeatureIdToInt(featureId);
    if (!NDetail::IsClientFeatureSupported(this, intFeatureId)) {
        NDetail::ThrowUnsupportedClientFeature(intFeatureId, TEnumTraits<E>::ToString(featureId));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
