#pragma once

#include "public.h"
#include "invoker_queue.h"
#include "event_count.h"
#include "scheduler.h"
#include "execution_context.h"
#include "thread_affinity.h"

#include <core/actions/callback.h>
#include <core/actions/future.h>
#include <core/actions/invoker.h>
#include <core/actions/signal.h>

#include <core/misc/shutdownable.h>

#include <core/profiling/profiler.h>

#include <util/system/thread.h>

#include <util/thread/lfqueue.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

class TSchedulerThread;
typedef TIntrusivePtr<TSchedulerThread> TSchedulerThreadPtr;

////////////////////////////////////////////////////////////////////////////////

class TSchedulerThread
    : public TRefCounted
    , public IScheduler
    , public IShutdownable
{
public:
    virtual ~TSchedulerThread();

    void Start();
    virtual void Shutdown() override;

    TThreadId GetId() const;
    bool IsRunning() const;

    virtual TFiber* GetCurrentFiber() override;
    virtual void Return() override;
    virtual void Yield() override;
    virtual void YieldTo(TFiberPtr&& other) override;
    virtual void SwitchTo(IInvokerPtr invoker) override;
    virtual void SubscribeContextSwitched(TClosure callback) override;
    virtual void UnsubscribeContextSwitched(TClosure callback) override;
    virtual void WaitFor(TFuture<void> future, IInvokerPtr invoker) override;
    virtual void UninterruptableWaitFor(TFuture<void> future, IInvokerPtr invoker) override;

protected:
    TSchedulerThread(
        TEventCount* callbackEventCount,
        const Stroka& threadName,
        const NProfiling::TTagIdList& tagIds,
        bool enableLogging,
        bool enableProfiling);

    virtual EBeginExecuteResult BeginExecute() = 0;
    virtual void EndExecute() = 0;

    virtual void OnStart();
    virtual void OnShutdown();

    virtual void OnThreadStart();
    virtual void OnThreadShutdown();

    static void* ThreadMain(void* opaque);
    void ThreadMain();
    void ThreadMainStep();

    void FiberMain(unsigned int spawnedEpoch);
    bool FiberMainStep(unsigned int spawnedEpoch);

    void Reschedule(TFiberPtr fiber, TFuture<void> future, IInvokerPtr invoker);

    void OnContextSwitch();

    TEventCount* CallbackEventCount;
    Stroka ThreadName;
    bool EnableLogging;

    NProfiling::TProfiler Profiler;

    // If (Epoch & 0x1) == 0x1 then the thread is stopping.
    std::atomic<ui32> Epoch;

    TEvent ThreadStartedEvent;

    TThreadId ThreadId = InvalidThreadId;
    TThread Thread;

    TExecutionContext SchedulerContext;

    std::list<TFiberPtr> RunQueue;
    int FibersCreated = 0;
    int FibersAlive = 0;

    TFiberPtr IdleFiber;
    TFiberPtr CurrentFiber;

    TFuture<void> WaitForFuture;
    IInvokerPtr SwitchToInvoker;

    TCallbackList<void()> ContextSwitchCallbacks;

    DECLARE_THREAD_AFFINITY_SLOT(HomeThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
