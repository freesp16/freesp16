#include "io/midi/InputRouter.h"

#include <atomic>

namespace io::midi
{
namespace
{
std::atomic<InputRouter*>& slot() noexcept
{
    static std::atomic<InputRouter*> router {nullptr};
    return router;
}
} // namespace

void setInputRouter(InputRouter* r) noexcept { slot().store(r, std::memory_order_release); }
InputRouter* inputRouter() noexcept { return slot().load(std::memory_order_acquire); }
} // namespace io::midi
