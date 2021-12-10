#pragma once

#include "thread.h"

#include <library/cpp/yt/threading/event_count.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

class TSchedulerThreadBase
    : public TThread
{
public:
    ~TSchedulerThreadBase();

    void Stop(bool graceful = false);

protected:
    const TIntrusivePtr<NThreading::TEventCount> CallbackEventCount_;
    const TString ThreadGroupName_;
    const TString ThreadName_;

    std::atomic<bool> GracefulStop_ = false;

    TSchedulerThreadBase(
        TIntrusivePtr<NThreading::TEventCount> callbackEventCount,
        const TString& threadGroupName,
        const TString& threadName,
        int shutdownPriority = 0);

    virtual void OnStart();
    virtual void OnStop();

    virtual bool OnLoop(NThreading::TEventCount::TCookie* cookie) = 0;

private:
    void StartEpilogue() override;
    void StopPrologue() override;
    void StopEpilogue() override;

    void ThreadMain() override;
};

DEFINE_REFCOUNTED_TYPE(TSchedulerThreadBase)

////////////////////////////////////////////////////////////////////////////////

} //namespace NYT::NConcurrency
