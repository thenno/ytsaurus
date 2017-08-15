#pragma once

#include "private.h"
#include "chunk_pool.h"
#include "input_stream.h"

#include <yt/ytlib/chunk_pools/public.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/job_tracker_client/public.h>

namespace NYT {
namespace NChunkPools {

////////////////////////////////////////////////////////////////////////////////

struct TSortedJobOptions
{
    bool EnableKeyGuarantee;
    int PrimaryPrefixLength;
    int ForeignPrefixLength;
    bool EnablePeriodicYielder = true;

    std::vector<NTableClient::TKey> PivotKeys;

    //! An upper bound for a total number of slices that is allowed. If this value
    //! is exceeded, an exception is thrown.
    i64 MaxTotalSliceCount;

    //! An upper bound for a total data weight in a job. If this value
    //! is exceeded, an exception is thrown.
    i64 MaxDataWeightPerJob = std::numeric_limits<i64>::max();

    //! Experimental workaround for YTADMINREQ-5836.
    bool UseNewEndpointKeys = false;

    bool LogEndpoints = false;

    void Persist(const TPersistenceContext& context);
};

struct TSortedChunkPoolOptions
{
    EStripeListExtractionOrder ExtractionOrder = EStripeListExtractionOrder::DataSizeDescending;
    TSortedJobOptions SortedJobOptions;
    i64 MinTeleportChunkSize = 0;
    bool SupportLocality = false;
    NControllerAgent::IJobSizeConstraintsPtr JobSizeConstraints;
    NScheduler::TOperationId OperationId;

    void Persist(const TPersistenceContext& context);
};

////////////////////////////////////////////////////////////////////////////////

struct IChunkSliceFetcherFactory
    : public IPersistent
    , public virtual TRefCounted
{
    virtual NTableClient::IChunkSliceFetcherPtr CreateChunkSliceFetcher() = 0;

    virtual void Persist(const TPersistenceContext& context) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkSliceFetcherFactory);

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IChunkPool> CreateSortedChunkPool(
    const TSortedChunkPoolOptions& options,
    IChunkSliceFetcherFactoryPtr chunkSliceFetcherFactory,
    TInputStreamDirectory dataSourceDirectory);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkPools
} // namespace NYT
