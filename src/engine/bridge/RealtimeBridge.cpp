#include "engine/bridge/RealtimeBridge.h"

#include <limits>
#include <variant>

namespace engine::bridge
{
RealtimeBridge::~RealtimeBridge() { drainAll(); }

bool RealtimeBridge::post(const EngineMessage& message)
{
    if (const auto* c = std::get_if<ParamChange>(&message))
    {
        if (params_.find(c->param) == ParamTarget::None)
            return false; // the engine does not implement this parameter (yet)
    }
    else if (const auto* s = std::get_if<ObjectSwap>(&message))
    {
        if (! swaps_.valid(s->slot) || (s->slot.kind == SwapSlot::TrackSwapBatch && s->object == nullptr))
            return false; // no consumer for this slot kind or no batch to apply
    }
    else if (const auto* t = std::get_if<TriggerNow>(&message))
    {
        if (t->track >= kTracks)
            return false;
    }
    return queue_.push(message); // full: the caller coalesces and retries next frame
}

void RealtimeBridge::drainAll() noexcept
{
    EngineMessage m;
    while (queue_.pop(m))
        if (const auto* s = std::get_if<ObjectSwap>(&m); s != nullptr && s->object != nullptr)
            if (const DestroyFn destroy = swaps_.destroyer(s->slot.kind))
                destroy(s->object);
    retire_.collect(std::numeric_limits<std::uint64_t>::max() / 2, 0);
}
} // namespace engine::bridge
