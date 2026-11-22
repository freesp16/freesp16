// The slice view of a Sample track (spec 02-features/slicing.md SLC-003/004/010,
// audio-engine.md "Sample memory"): the immutable table of ≤ 65 frame offsets the message thread builds per edit and
// installs with ObjectSwap{SwapSlot::SliceMap, scene, track} (the type is core/util/slice/SliceTable.h, so ui / app can
// build it without including engine internals). No audio is ever copied: a slice is a region of the track's buffer.
//
// The engine reads it in three places, all O(1) and allocation-free:
//   installSwaps (step 1)    the current scene's 16 tables → TrackSource::setSliceMap and SlicePadLocks::setMap
//   router (step 6)          SLICE pad mode: pad → slice 16·windowOffset + pad as a slice-number lock (ParamIndex 22)
//                            on the hit (SlicePadLocks, an adapter: TriggerEvent has no pad field)
//   TrackSource (step 6/8a)  a Full trigger with a slice lock plays [start, end) of that slice (SLC-004)
#pragma once

#include "core/util/slice/SliceTable.h"
#include "engine/EngineApi.h"

namespace engine
{
class Engine;
}

namespace engine::voice
{
using SliceMap = core::slice::SliceTable;
using SliceRegion = core::slice::SliceRegion;

constexpr int kLockSlotSlice = 22; // core::ParamIndex 22, "slice.number" (sequencer.md "Lockable parameters")

inline SwapSlot sliceMapSlot(int scene, int track) noexcept
{
    return {SwapSlot::SliceMap, static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(track & 15)};
}
inline const SliceMap* asSliceMap(const void* installed) noexcept { return static_cast<const SliceMap*>(installed); }

// engine.acceptSwaps(SwapSlot::SliceMap, core::slice::destroySliceTable) (engine set-up, message thread).
void acceptSliceSwaps(Engine& engine) noexcept;

// The region a slice lock selects on a buffer of `frames` frames: the installed table when it was built for this
// buffer, else the default grid of 16 equal slices (a table not yet rebuilt for a new sample, or none).
// False when the slice does not exist (no sound).
bool sliceRegion(const SliceMap* map, std::int64_t frames, int slice, SliceRegion& out) noexcept;
} // namespace engine::voice
