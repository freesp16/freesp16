// How the engine accepts the sample buffers of the voices through ObjectSwap (threading.md "Object swap protocol";
// SwapSlot::TrackSample(scene, track) and SwapSlot::Preview, from the sample library to the voices). Called by the
// engine's set-up (message thread, before the device starts).
//
// Ownership: the sender (the sample library's pool) retains a SampleBuffer once for the slot before it posts the swap;
// when the slot's object is replaced, the engine retires it and, after its grace period, the message thread calls
// the destroy function below, which drops the slot's reference (core::audio::release). The voices hold references of
// their own while they play, so a buffer outlives its slot until its last tail has faded.
#pragma once

#include "core/audio/SampleBuffer.h"
#include "engine/EngineApi.h"

namespace engine
{
class Engine;
}

namespace engine::voice
{
// Destroy function of TrackSample / Preview slots: drops the slot's reference (message thread).
void releaseSlotBuffer(const void* object) noexcept;

// engine.acceptSwaps(TrackSample) and (Preview) with releaseSlotBuffer.
void acceptVoiceSwaps(Engine& engine) noexcept;

inline SwapSlot trackSampleSlot(int scene, int track) noexcept
{
    return {SwapSlot::TrackSample, static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(track & 15)};
}
inline SwapSlot previewSlot() noexcept { return {SwapSlot::Preview, 0, 0}; }

// The SampleBuffer behind an installed slot pointer (Engine::installed / SwapTable::get), null when none.
inline const core::audio::SampleBuffer* asSampleBuffer(const void* installed) noexcept
{
    return static_cast<const core::audio::SampleBuffer*>(installed);
}
} // namespace engine::voice
