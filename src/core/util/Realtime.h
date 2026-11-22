// Real-time contract enforcement (threading.md: the audio thread never allocates, locks, logs or does I/O).
//
// - ScopedRealtimeThread marks the current thread as real-time for its lifetime (the audio callback opens one).
// - With APP_RT_CHECKS (host Debug preset, device debug builds), the global operator new/delete are replaced
//   (Realtime.cpp) and abort with a message when called on a marked thread. Unit tests run the engine under it.
// - tools/rt-lint.py flags allocation/lock/I/O calls in files compiled with APP_REALTIME_TU (the engine).
// - ScopedAllowAllocation is the explicit, greppable escape hatch (e.g. for a test that checks the hook itself).
#pragma once

#include <cstddef>

namespace core::rt
{
bool isRealtimeThread() noexcept;
std::size_t violationCount() noexcept; // allocations seen on real-time threads (when not aborting)
void setAbortOnViolation(bool) noexcept;

class ScopedRealtimeThread
{
public:
    ScopedRealtimeThread() noexcept;
    ~ScopedRealtimeThread();
    ScopedRealtimeThread(const ScopedRealtimeThread&) = delete;
    ScopedRealtimeThread& operator= (const ScopedRealtimeThread&) = delete;

private:
    bool previous_;
};

class ScopedAllowAllocation
{
public:
    ScopedAllowAllocation() noexcept;
    ~ScopedAllowAllocation();
    ScopedAllowAllocation(const ScopedAllowAllocation&) = delete;
    ScopedAllowAllocation& operator= (const ScopedAllowAllocation&) = delete;

private:
    bool previous_;
};

// Flush denormals to zero on this thread (tech-stack.md numerics policy). On ARMv7 this sets FPSCR.FZ, which makes
// VFP behave like NEON (always flush-to-zero) and avoids the VFP's slow denormal traps on the Cortex-A9.
void enableFlushToZero() noexcept;
} // namespace core::rt
