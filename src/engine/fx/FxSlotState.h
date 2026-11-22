// Plain data of the FX slot bank: the scene snapshot the bridge prepares for a scene switch, the step
// locks the sequencer's lock applier passes, and the meter feed the effect panels read. Trivially copyable, fixed
// size, no engine state: prepared off the audio thread and installed (ObjectSwap / copy) at a block start.
//
// Slot index: 0…31 = track t's FX1 at 2t and FX2 at 2t + 1; 32 = Send; 33 = Master (kSendSlot, kMasterSlot).
// Type index: the position in the slot kind's list (engine/fx/EffectRegistryOrder.h listFor), −1 = Off.
#pragma once

#include "engine/EngineApi.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace engine::fx
{
constexpr int kInsertSlots = 2 * kTracks;
constexpr int kSendSlot = kInsertSlots;
constexpr int kMasterSlot = kInsertSlots + 1;
constexpr int kFxSlots = kInsertSlots + 2;
constexpr int kSlotOptions = 8;

constexpr int insertSlot(int track, int unit) noexcept
{
    return 2 * track + unit;
}

struct FxSlotSettings
{
    std::int8_t type = -1;                // −1 = Off
    std::uint8_t bypass = 0;              // FX-044/045 inserts, FX-046 Send/Master
    std::uint8_t optionMask = 0;          // which of options[] are set
    float params[6]{};                    // stored step fractions, internal index k (FX-060)
    std::int16_t options[kSlotOptions]{}; // EffectBase::setOption(i, options[i]) for each set bit i
};

// One scene's FX (FX-004/045, OQ-12): applied at the switch block (FxSlotBank::applyScene).
struct FxSlotState
{
    std::uint8_t scene = 0;
    std::uint8_t fxMode = 0;     // FxMode
    std::uint32_t quirkMask = 0; // FxShell kQuirkProfile* (project-wide; repeated here so one install is complete)
    std::array<FxSlotSettings, kFxSlots> slot{};
};

// The FX p-locks of one step of one track (FX-078…080): FX1 = unit 0, FX2 = unit 1; bit k of mask = k locked; the
// lock applies only while the slot holds `type` (FX-080).
struct FxStepLocks
{
    std::uint8_t mask[2]{};
    float value[2][6]{};
    std::int8_t type[2]{-1, -1};
};

// Effect panel feed (UID-834), published once per period with the engine's other feeds.
struct FxMeterFeed
{
    std::uint64_t blockIndex = 0;
    std::int8_t meteredSlot = -1; // the slot whose IN/OUT peaks are measured (one panel at a time), −1 = none
    float inPeak = 0.0f, outPeak = 0.0f;
    float telemetry[2]{};                        // the metered slot's current effect: telemetry(0), telemetry(1)
    std::uint64_t runningMask = 0;               // bit s = slot s has an effect running (not Off, not settled-disabled)
    std::uint32_t triggerFlash = 0;              // tracks triggered in the last block (Ducker SOURCE flash)
    std::array<std::int8_t, kFxSlots> current{}; // current type index per slot, −1 = Off
};

static_assert(std::is_trivially_copyable_v<FxSlotState> && std::is_trivially_copyable_v<FxStepLocks> &&
                  std::is_trivially_copyable_v<FxMeterFeed>,
              "FX bank data crosses threads by copy");
} // namespace engine::fx
