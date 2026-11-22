// One effect slot (data-model.md "EffectSlot": InsertFx[2] per track, SendFx and MasterFx per scene; effect-shell.md).
// Parameters are stored as step fractions 0–1 (FX-060, FMT-013); type1/type2 are the effect's own enums; every type
// the slot has held keeps its last settings so switching back restores them (FX-007, FMT-014).
#pragma once

#include "core/model/Extras.h"
#include "core/model/enums/Enums.h"

#include <array>
#include <vector>

namespace core::model
{
struct EffectSettings
{
    EffectType type = EffectType::Off;
    int type1 = 0, type2 = 0;           // per-effect enums (effects/parameters.md "Enums")
    std::array<double, 6> params {};    // step fraction step/(steps-1), page order (knob 1..6)
    bool sync = false;                  // tempo-synced times
    bool lookAhead = false;             // Send/Master compressor only (split from sync, FX quirk)
    friend bool operator== (const EffectSettings&, const EffectSettings&) = default;
};

struct EffectSlot
{
    EffectType type = EffectType::Off;
    bool sync = false;
    bool lookAhead = false;
    bool bypass = false;
    int type1 = 0, type2 = 0;
    std::array<double, 6> params {};
    std::vector<EffectSettings> typeMemory; // FX-007: last settings of every other type this slot has held
    Extras extras {};
    friend bool operator== (const EffectSlot&, const EffectSlot&) = default;
};
} // namespace core::model
