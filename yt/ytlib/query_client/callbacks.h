#pragma once

#include "public.h"

#include <ytlib/ypath/rich.h>

#include <core/rpc/public.h>

#include <core/actions/future.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

typedef std::function<TQueryStatistics(
    const TQueryPtr& query,
    TGuid dataId,
    ISchemafulWriterPtr writer)> TExecuteQuery;

////////////////////////////////////////////////////////////////////////////////

struct IExecutor
    : public virtual TRefCounted
{
    virtual TFuture<TQueryStatistics> Execute(
        TPlanFragmentPtr fragment,
        ISchemafulWriterPtr writer) = 0;

};

DEFINE_REFCOUNTED_TYPE(IExecutor)

////////////////////////////////////////////////////////////////////////////////

struct IPrepareCallbacks
{
    virtual ~IPrepareCallbacks()
    { }

    //! Returns an initial split for a given path.
    virtual TFuture<TDataSplit> GetInitialSplit(
        const NYPath::TRichYPath& path,
        TTimestamp timestamp) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

