// Patterns and steps (data-model.md "Pattern and steps"; SEQ-001…, SEQ-010/011/021). A step matrix always has
// 64 × 16 cells (invariant); `length` only limits playback. Cells are 8 bytes so the 256 patterns of a project stay
// ≈ 2 MB; p-locks are sparse, stored per locked cell in `locks` and referenced by index.
#pragma once

#include "core/model/Extras.h"
#include "core/model/enums/Enums.h"

#include <array>
#include <cstdint>
#include <vector>

namespace core::model
{
constexpr int kSteps = 64, kTracks = 16, kScenes = 16, kPatterns = 16;
// 23 track lock slots (ParamIndex, sequencer.md "Lockable parameters") + 6 FX1 + 6 FX2 (OQ-07): 35.
constexpr int kLockSlots = 35;
using LockSlot = std::uint8_t;

struct Step // one track at one step: 8 bytes
{
    static constexpr std::int16_t kEmpty = -127;     // reference sentinel: no trigger
    static constexpr std::int16_t kNoValue2 = -126;  // reference sentinel: no second value
    static constexpr std::uint16_t kNoLocks = 0xFFFF;

    std::int16_t trigger = kEmpty;   // trigger value; kEmpty = empty step
    std::int16_t value2 = kNoValue2; // Sixteen: velocity 1–127
    TriggerType triggerType = TriggerType::Full;
    std::uint8_t reserved = 0;
    std::uint16_t lockIndex = kNoLocks; // index into Pattern::locks

    bool empty() const noexcept { return trigger == kEmpty && lockIndex == kNoLocks; }
    friend bool operator== (const Step&, const Step&) = default;
};
static_assert(sizeof(Step) == 8, "a step cell is 8 bytes");

struct StepLocks // the p-locks of one cell
{
    std::uint64_t mask = 0;                         // bit s = slot s locked
    std::array<float, kLockSlots> values {};        // native units of the slot's ParamSpec
    std::array<LockChange, kLockSlots> change {};   // value-change type to the next step (SEQ-021)
    friend bool operator== (const StepLocks&, const StepLocks&) = default;
};

struct Pattern
{
    int length = 16; // 8–64 in the UI, files may hold 1–64 (SEQ-001)
    std::array<bool, kTracks> trackMutes {}, trackSolos {};
    std::array<std::array<Step, kTracks>, kSteps> steps {}; // steps[step][track]
    std::vector<StepLocks> locks;                           // referenced by Step::lockIndex
    Extras extras {};
    friend bool operator== (const Pattern&, const Pattern&) = default;
};
} // namespace core::model
