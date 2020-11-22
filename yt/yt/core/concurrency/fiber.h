#pragma once

#include "private.h"
#include "execution_stack.h"

#include <yt/core/misc/small_vector.h>

#include <util/system/context.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

struct TContextSwitchHandlers
{
    std::function<void()> Out;
    std::function<void()> In;
};

DECLARE_REFCOUNTED_CLASS(TFiber)

class TFiberRegistry;

using TFiberCanceler = TCallback<void(const TError&)>;

class TFiber
    : public TRefCounted
    , public ITrampoLine
{
public:
    explicit TFiber(EExecutionStackKind stackKind = EExecutionStackKind::Small);
    ~TFiber();

    TFiberId GetId();

    bool CheckFreeStackSpace(size_t space) const;

    TExceptionSafeContext* GetContext();

private:
    // Base fiber fields.
    TFiberId Id_;

    std::shared_ptr<TExecutionStack> Stack_;
    TExceptionSafeContext Context_;

    // No way to select static/thread_local variable in GDB from particular shared library.
    TFiberRegistry* const Registry_;
    const std::list<TFiber*>::iterator Iterator_;

    void RegenerateId();

public:
    // User-defined context switch handlers (executed only during WaitFor).
    friend void PushContextHandler(std::function<void()> out, std::function<void()> in);
    friend void PopContextHandler();

    void InvokeContextOutHandlers();
    void InvokeContextInHandlers();

private:
    SmallVector<TContextSwitchHandlers, 16> SwitchHandlers_;

public:
    // FLS, memory and tracing.
    void OnSwitchIn();
    void OnSwitchOut();

private:
    std::atomic<bool> Running_ = {false};

protected:
    void OnStartRunning();
    void OnFinishRunning();

public:
    void ResetForReuse();

    bool IsCanceled() const;
    TError GetCancelationError() const;
    const TFiberCanceler& GetCanceler();

    void SetFuture(TFuture<void> future);
    void ResetFuture();

private:
    std::atomic<bool> Canceled_ = {false};
    std::atomic<size_t> Epoch_ = {0};

    YT_DECLARE_SPINLOCK(TAdaptiveLock, SpinLock_);
    TError CancelationError_;
    TFiberCanceler Canceler_;
    TFuture<void> Future_;

    void CancelEpoch(size_t epoch, const TError& error);
};

DEFINE_REFCOUNTED_TYPE(TFiber)

////////////////////////////////////////////////////////////////////////////////

} //namespace NYT::NConcurrency
