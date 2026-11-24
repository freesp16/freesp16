// A mutex with priority inheritance, for a lock a real-time thread shares with a normal one (the panel link and the
// lamp model, the MIDI sender and the port hot-plug): while a SCHED_FIFO thread waits for it, the normal thread that
// holds it runs at the waiter's priority, so a busy UI core cannot keep the lock — and the waiting thread — stalled.
//
// PTHREAD_PRIO_INHERIT needs the kernel's PI futexes; where they are missing (an emulator that does not forward them)
// the mutex is a plain one: probed once, inheritanceAvailable() says which. Lockable, so std::lock_guard works.
// No condition variable may wait on it (none does).
#pragma once

#if defined(__linux__)
 #include <pthread.h>
#else
 #include <mutex>
#endif

namespace core
{
class PiMutex
{
public:
    PiMutex() noexcept;
    ~PiMutex();
    PiMutex(const PiMutex&) = delete;
    PiMutex& operator= (const PiMutex&) = delete;

    void lock() noexcept;
    bool try_lock() noexcept;
    void unlock() noexcept;

    static bool inheritanceAvailable() noexcept;

private:
#if defined(__linux__)
    pthread_mutex_t mutex_;
#else
    std::mutex mutex_;
#endif
};
} // namespace core
