// SLICE pad mode through the trigger router (SLC-003/005; an adapter). The frozen TriggerEvent has no
// pad field, so a pad hit in SLICE mode carries its slice the way a sequencer step does: as a slice-number lock
// (ParamIndex 22, the lock slot of "slice.number") in a LockSet this object owns. One path for both sources: the
// TrackSource plays a Full trigger with a slice lock as that slice's region, and the recorder (LiveHitSink) sees the
// lock on the live hit it records.
//
//   step 1   installSwaps → setMap(track, table)            the current scene's tables (null = default 16 grid)
//   step 1   router.pushLive (SLICE mode) → slice(track, pad), lock(slice)
//   step 6   router.process() → endBlock()                  flips the two lock banks
// Lifetime (LockSet contract: owned by the sender for the whole block): the locks of block N's live hits live in bank
// N mod 2, which block N + 1's hits do not touch, so blockEvents() of block N stay valid until block N + 2 starts.
// Real-time: fixed arrays, no allocation; ≤ 20 cycles per pad hit.
#pragma once

#include "engine/EngineApi.h"
#include "engine/trigger/TriggerEvent.h"
#include "engine/voice/SliceMap.h"

#include <array>
#include <cstdint>

namespace engine::voice
{
class SlicePadLocks
{
public:
    static constexpr int kPerBlock = 64; // ≥ TriggerRouter::kMaxLiveEvents
    static constexpr int kSlots = 35;    // LockSet slots 0–34 (readers index values by slot)

    void setMap(int track, const SliceMap* map) noexcept { maps_[static_cast<std::size_t>(track & 15)] = map; }
    const SliceMap* map(int track) const noexcept { return maps_[static_cast<std::size_t>(track & 15)]; }

    // The slice pad `pad` (0 = bottom-left) plays on `track`: 16·windowOffset + pad of the installed table, or pad
    // itself on the default 16-slice grid; −1 = the grid has no such slice (the hit is silent).
    int slice(int track, int pad) const noexcept
    {
        const SliceMap* m = maps_[static_cast<std::size_t>(track & 15)];
        if (m == nullptr)
            return pad >= 0 && pad < 16 ? pad : -1;
        return m->sliceForPad(pad, m->windowOffset());
    }
    // A LockSet with only the slice-number slot set (change Revert); null when this block's bank is full.
    const trigger::LockSet* lock(int slice) noexcept
    {
        if (used_ >= kPerBlock)
            return nullptr;
        Entry& e = bank_[bank_index_][static_cast<std::size_t>(used_++)];
        e.values[kLockSlotSlice] = static_cast<float>(slice);
        e.set.mask = std::uint64_t {1} << kLockSlotSlice;
        e.set.values = e.values.data();
        e.set.change = nullptr;
        return &e.set;
    }
    void endBlock() noexcept
    {
        bank_index_ ^= 1;
        used_ = 0;
    }

private:
    struct Entry
    {
        trigger::LockSet set;
        std::array<float, kSlots> values;
    };
    std::array<const SliceMap*, kTracks> maps_ {};
    Entry bank_[2][kPerBlock]; // written before read (no braces: GCC 11 build time)
    int bank_index_ = 0, used_ = 0;
};
} // namespace engine::voice
