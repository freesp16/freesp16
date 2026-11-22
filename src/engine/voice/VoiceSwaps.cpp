#include "engine/voice/VoiceSwaps.h"

#include "engine/Engine.h"

namespace engine::voice
{
void releaseSlotBuffer(const void* object) noexcept { core::audio::release(asSampleBuffer(object)); }

void acceptVoiceSwaps(Engine& engine) noexcept
{
    engine.acceptSwaps(SwapSlot::TrackSample, &releaseSlotBuffer);
    engine.acceptSwaps(SwapSlot::Preview, &releaseSlotBuffer);
}
} // namespace engine::voice
