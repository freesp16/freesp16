// engine::sampling::CaptureRing — the engine's name for the lock-free capture ring the audio thread writes and the
// Recorder / take builder read by absolute frame index. The type lives in core/util/sampling/CaptureHub.h so services
// can include it (tools/check-deps.py: services and app see only engine/EngineApi.h of the engine). API:
//   allocate(frames, channels)  message thread        write(l, r, frames)  audio thread
//   head()                      atomic, frames written read(from, n, l, r)  false if not written or overwritten
#pragma once

#include "core/util/sampling/CaptureHub.h"

#include <cstdint>

namespace engine::sampling
{
using CaptureRing = core::sampling::CaptureRing;
using CaptureHub = core::sampling::CaptureHub;
using CaptureStatus = core::sampling::CaptureStatus;
using RecState = core::sampling::RecState;
using LiveState = core::sampling::LiveState;

// The finished (or running) live take: ring window [start, start + length), the nudge applied on
// save / loop, loop-after-rec, the target track.
struct LiveTake
{
    std::int64_t start = -1, length = 0;
    std::int32_t nudge = 0;
    bool loopAfterRec = false;
    std::uint8_t track = 0;
};
} // namespace engine::sampling
