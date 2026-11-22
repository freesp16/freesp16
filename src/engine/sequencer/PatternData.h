// engine/sequencer/PatternData.h — the engine's immutable pattern object (a frozen contract).
// Immutable, built on the message thread from the model (engine/sequencer/PatternBuilder.h, through the model-neutral
// core/util/sequencer/PatternImage.h), installed with ObjectSwap{SwapSlot::Pattern, scene, pattern}, retired after
// ≥ 2 blocks (threading.md). Readers: the sequencer (step 6), the scene switcher (triggerMask: "has content",
// SCN-026), the ratchets (retrigger fields).
//
// Cell notes: `offset24` is the step's OFF SET lock (lock slot 15) when the cell's LockSet has that slot, else 0 and
// the track's offset applies (SEQ-005); `retrigCount` / `retrigSpeed` are the slot 16 / 17 locks (speed stored + 1),
// 0 = the track value. `kind` is never Release / Stop.
#pragma once

#include "engine/trigger/TriggerEvent.h"

#include <cstdint>
#include <span>

// clang-format off: the block below is the frozen contract text
namespace engine::sequencer {
struct StepCell {                     // 8 bytes; velocity 0 = empty step (SEQ-010)
    std::uint8_t velocity;  trigger::TriggerKind kind;     // Full / Half / LockOnly (SEQ-011)
    std::int8_t offset24;             // −24…+24 = ±1 step in 1/24 (SEQ-005); a lock may override it
    std::uint8_t retrigCount, retrigSpeed;                // consumed by the ratchets (perf/)
    std::uint16_t lockIndex;          // into locks, 0xFFFF = none
};
struct PatternData {
    std::uint8_t length;              // 1–64 steps (UI enforces 8–64, SEQ-001)
    std::uint16_t trackMutes, trackSolos;                 // per-pattern (SCN-011/012)
    std::uint64_t triggerMask[16];    // bit s = step s holds a trigger ("has content", SCN-026)
    StepCell cells[64][16];           // step-major
    std::span<const trigger::LockSet> locks;              // values in native units (SEQ-024)
};
}
// clang-format on

namespace engine::sequencer
{
static_assert(sizeof(StepCell) == 8, "a step cell is 8 bytes");
constexpr std::uint16_t kNoLockIndex = 0xFFFF;
constexpr int kLockSlotOffset = 15; // ParamIndex Offset (sequencer.md "Lockable parameters")

inline bool hasOffsetLock(const PatternData& p, const StepCell& c) noexcept
{
    return c.lockIndex != kNoLockIndex && c.lockIndex < p.locks.size() &&
           (p.locks[c.lockIndex].mask >> kLockSlotOffset & 1u) != 0;
}
} // namespace engine::sequencer
