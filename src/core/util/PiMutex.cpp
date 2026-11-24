#include "core/util/PiMutex.h"

#if defined(__linux__)
 #include <cerrno>
 #include <linux/futex.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

namespace core
{
#if defined(__linux__)
namespace
{
// FUTEX_UNLOCK_PI on a word nobody owns: EPERM where PI futexes work, ENOSYS where they do not (an old kernel without
// the atomic futex compare-and-exchange, or a user-mode emulator that does not forward the operation).
bool probe() noexcept
{
    int word = 0;
    const long r = ::syscall(SYS_futex, &word, FUTEX_UNLOCK_PI, 0, nullptr, nullptr, 0);
    return ! (r != 0 && errno == ENOSYS);
}
} // namespace

bool PiMutex::inheritanceAvailable() noexcept
{
    static const bool available = probe();
    return available;
}

PiMutex::PiMutex() noexcept
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (inheritanceAvailable())
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if (pthread_mutex_init(&mutex_, &attr) != 0)
    {
        pthread_mutexattr_destroy(&attr);
        pthread_mutex_init(&mutex_, nullptr); // a plain mutex rather than none
        return;
    }
    pthread_mutexattr_destroy(&attr);
}

PiMutex::~PiMutex() { pthread_mutex_destroy(&mutex_); }
void PiMutex::lock() noexcept { pthread_mutex_lock(&mutex_); }
bool PiMutex::try_lock() noexcept { return pthread_mutex_trylock(&mutex_) == 0; }
void PiMutex::unlock() noexcept { pthread_mutex_unlock(&mutex_); }
#else
bool PiMutex::inheritanceAvailable() noexcept { return false; }
PiMutex::PiMutex() noexcept = default;
PiMutex::~PiMutex() = default;
void PiMutex::lock() noexcept { mutex_.lock(); }
bool PiMutex::try_lock() noexcept { return mutex_.try_lock(); }
void PiMutex::unlock() noexcept { mutex_.unlock(); }
#endif
} // namespace core
