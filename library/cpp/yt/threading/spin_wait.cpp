#include "spin_wait.h"
#include "spin_wait_hook.h"

#include <util/datetime/base.h>

#include <util/system/compiler.h>

#include <atomic>

namespace NYT::NThreading {

////////////////////////////////////////////////////////////////////////////////

static constexpr int SpinIterationCount = 1000;

namespace {

TDuration SuggestSleepDelay(int iteration)
{
    static std::atomic<ui64> Rand;
    auto rand = Rand.load(std::memory_order_relaxed);
    rand = 0x5deece66dLL * rand + 0xb;   // numbers from nrand48()
    Rand.store(rand, std::memory_order_relaxed);

    constexpr ui64 MinDelayUs = 128;

    // Double delay every 8 iterations, up to 16x (2ms).
    if (iteration > 32) {
        iteration = 32;
    }
    ui64 delayUs = MinDelayUs << (iteration / 8);

    // Randomize in delay..2*delay range, for resulting 128us..4ms range.
    delayUs = delayUs | ((delayUs - 1) & rand);
    return TDuration::MicroSeconds(delayUs);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TSpinWait::~TSpinWait()
{
    if (SlowPathStartInstant_ >= 0) {
        if (auto* hook = GetSpinWaitSlowPathHook()) {
            auto cpuDelay = GetCpuInstant() - SlowPathStartInstant_;
            hook(cpuDelay);
        }
    }
}

void TSpinWait::Wait()
{
    if (Y_LIKELY(SpinIteration_++ < SpinIterationCount)) {
        return;
    }

    SpinIteration_ = 0;

    if (SlowPathStartInstant_ < 0) {
        SlowPathStartInstant_ = GetCpuInstant();
    }

    Sleep(SuggestSleepDelay(SleepIteration_++));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NThreading
