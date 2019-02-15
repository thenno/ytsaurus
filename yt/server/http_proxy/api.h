#pragma once

#include "public.h"

#include <yt/core/http/http.h>
#include <yt/core/profiling/profiler.h>

#include <yt/core/concurrency/rw_spinlock.h>

#include <yt/ytlib/driver/driver.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

using TUserCommandPair =std::pair<std::optional<TString>, std::optional<TString>>;

class TSemaphoreGuard
{
public:
    TSemaphoreGuard(TSemaphoreGuard&&) = default;
    TSemaphoreGuard& operator = (TSemaphoreGuard&&) = default;

    TSemaphoreGuard(TApi* api, const TUserCommandPair& key);
    ~TSemaphoreGuard();

private:
    struct TEmptyDeleter
    {
        void operator () (TApi* api)
        { }
    };

    std::unique_ptr<TApi, TEmptyDeleter> Api_;
    TUserCommandPair Key_;
};

////////////////////////////////////////////////////////////////////////////////

class TApi
    : public NHttp::IHttpHandler
{
public:
    explicit TApi(TBootstrap* bootstrap);

    virtual void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

    const NDriver::IDriverPtr& GetDriverV3() const;
    const NDriver::IDriverPtr& GetDriverV4() const;
    const THttpAuthenticatorPtr& GetHttpAuthenticator() const;
    const TCoordinatorPtr& GetCoordinator() const;

    bool IsUserBannedInCache(const TString& user);
    void PutUserIntoBanCache(const TString& user);

    std::optional<TSemaphoreGuard> AcquireSemaphore(const TString& user, const TString& command);
    void ReleaseSemaphore(const TUserCommandPair& key);

    void IncrementProfilingCounters(
        const TString& user,
        const TString& command,
        std::optional<NHttp::EStatusCode> httpStatusCode,
        TErrorCode apiErrorCode,
        TDuration duration,
        i64 bytesIn,
        i64 bytesOut);

    void IncrementHttpCode(NHttp::EStatusCode httpStatusCode);

private:
    const TApiConfigPtr Config_;

    const NDriver::IDriverPtr DriverV3_;
    const NDriver::IDriverPtr DriverV4_;

    const THttpAuthenticatorPtr HttpAuthenticator_;
    const TCoordinatorPtr Coordinator_;

    NConcurrency::TReaderWriterSpinLock BanCacheLock_;
    THashMap<TString, TInstant> BanCache_;

    struct TProfilingCounters
    {
        std::atomic<int> LocalSemaphore;

        NProfiling::TTagIdList Tags;

        NProfiling::TAggregateGauge ConcurrencySemaphore;
        NProfiling::TMonotonicCounter RequestCount;
        NProfiling::TMonotonicCounter BytesIn;
        NProfiling::TMonotonicCounter BytesOut;
        NProfiling::TAggregateGauge RequestDuration;

        TSpinLock Lock;
        THashMap<NHttp::EStatusCode, NProfiling::TMonotonicCounter> HttpCodes;
        THashMap<TErrorCode, NProfiling::TMonotonicCounter> ApiErrors;
    };

    std::atomic<int> GlobalSemaphore_{0};

    NConcurrency::TReaderWriterSpinLock CountersLock_;
    THashMap<TUserCommandPair, std::unique_ptr<TProfilingCounters>> Counters_;

    TSpinLock HttpCodesLock_;
    THashMap<NHttp::EStatusCode, NProfiling::TMonotonicCounter> HttpCodes_;

    TProfilingCounters* GetProfilingCounters(const TUserCommandPair& key);

    NProfiling::TMonotonicCounter PrepareErrorCount_{"/request_prepare_error_count"};

    void DoIncrementHttpCode(
        THashMap<NHttp::EStatusCode, NProfiling::TMonotonicCounter>* counters,
        NHttp::EStatusCode httpStatusCode,
        NProfiling::TTagIdList tags);
};

DEFINE_REFCOUNTED_TYPE(TApi)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
