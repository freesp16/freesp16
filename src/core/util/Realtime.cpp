#include "core/util/Realtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(__x86_64__) || defined(_M_X64)
 #include <xmmintrin.h>
#endif

namespace core::rt
{
namespace
{
thread_local bool realtimeThread = false;
thread_local bool allocationAllowed = false;
std::atomic<std::size_t> violations {0};
std::atomic<bool> abortOnViolation {true};
} // namespace

bool isRealtimeThread() noexcept { return realtimeThread && ! allocationAllowed; }
std::size_t violationCount() noexcept { return violations.load(); }
void setAbortOnViolation(bool on) noexcept { abortOnViolation.store(on); }

ScopedRealtimeThread::ScopedRealtimeThread() noexcept : previous_(realtimeThread) { realtimeThread = true; }
ScopedRealtimeThread::~ScopedRealtimeThread() { realtimeThread = previous_; }
ScopedAllowAllocation::ScopedAllowAllocation() noexcept : previous_(allocationAllowed) { allocationAllowed = true; }
ScopedAllowAllocation::~ScopedAllowAllocation() { allocationAllowed = previous_; }

void enableFlushToZero() noexcept
{
#if defined(__arm__)
    unsigned fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24); // FZ
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#elif defined(__aarch64__)
    unsigned long fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ul << 24); // FZ
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040); // FTZ | DAZ
#endif
}

#if APP_RT_CHECKS
void reportViolation(const char* what) noexcept
{
    violations.fetch_add(1);
    if (abortOnViolation.load())
    {
        // fputs does not allocate; this is the failure path anyway.
        std::fputs("REALTIME VIOLATION: ", stderr);
        std::fputs(what, stderr);
        std::fputs(" on a real-time thread\n", stderr);
        std::abort();
    }
}
#endif
} // namespace core::rt

#if APP_RT_CHECKS
// Debug allocator hook: every global allocation checks the thread's real-time flag.
void* operator new(std::size_t size)
{
    if (core::rt::isRealtimeThread())
        core::rt::reportViolation("operator new");
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (core::rt::isRealtimeThread())
        core::rt::reportViolation("operator new(nothrow)");
    return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t& t) noexcept { return ::operator new(size, t); }
void operator delete(void* p) noexcept
{
    if (p && core::rt::isRealtimeThread())
        core::rt::reportViolation("operator delete");
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif
