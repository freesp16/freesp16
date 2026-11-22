// The FX parameter model (spec 02-features/effects/parameters.md FX-060…096; research docs/effects/parameters.md
// "SYNC conversion", "UI step law and SHIFT", CC BY 4.0; arithmetic operation for operation as the MIT model
// tools/fx/sync_convert.py). Message thread: the UI, the commands (core/commands/fx), the CC table
// builder and the catalogue files of the effects use it; the engine never does.
//
// Stored form (FX-060/061): every continuous FX parameter is a step fraction v = (i − lo)/(hi − 1 − lo) of its grid
// [lo, hi), by internal index k = 0…5. Time and rate parameters have a free and a sync grid (Family, FX-072).
//
// Registration: a file core/params/catalogue/fx_<name>.cpp calls
//     core::fx::registerEffect(r, "<effect>", defs, kSlotInsert | ..., "<flag>", options);
// from its APP_REGISTER_PARAMS(…, 700 + n, …) function. That registers one ParamSpec per def and allowed slot:
//     fx1.<effect>.<key>, fx2.<effect>.<key>   (Insert: lockable, LFO and touch-strip targets, ParamScope::Track)
//     send.<effect>.<key>, master.<effect>.<key> (Send/Master: not lockable, FX-094; ParamScope::Scene)
// with the slot's default (defInsert / defSend / defMaster, FX-092/093), and records the defs for the commands
// (defaults, SYNC conversion) and for internalIndexTable(). The EffectDescriptor names the same parameters as
// "fx.<effect>.<key>" (engine/fx/EffectRegistryOrder.h slotParamId).
#pragma once

#include "core/model/enums/Enums.h"
#include "core/params/ParamRegistry.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace core::fx
{
// Slot mask bits (the same values as engine::fx::SlotKind, which core may not include).
constexpr std::uint8_t kSlotInsert = 1, kSlotSend = 2, kSlotMaster = 4;

struct Grid // [lo, hi): steps lo…hi − 1
{
    std::int32_t lo = 0, hi = 2;
    constexpr std::int32_t span() const noexcept { return hi - 1 - lo; }
    friend constexpr bool operator==(Grid, Grid) = default;
};

// Time and rate families (FX-072); Plain = no SYNC form.
enum class Family : std::uint8_t
{
    Plain,
    Chorus,
    Flanger,
    Phaser,
    InsertDelay,
    SendDelay,
    DuckerAttack,
    DuckerRelease
};
Grid freeGrid(Family) noexcept; // Plain: {0, 2}
Grid syncGrid(Family) noexcept; // Plain: {0, 2}
bool hasSync(Family f) noexcept;

struct EffectParamDef
{
    std::string_view key, label, shortLabel; // ParamSpec id suffix, caption, ≤ 8 chars
    std::int8_t k = 0;                       // internal index (FX-060)
    Grid free{0, 101};                       // the free grid (Plain: the only grid)
    Grid sync{};                             // ignored unless the family has SYNC
    Family family = Family::Plain;
    float defInsert = 0.0f, defSend = 0.0f, defMaster = 0.0f; // step fractions (FX-092/093)
    bool bipolar = false;
    ParamFormatter format = nullptr; // null: the family's free-mode text (format())
};

struct EffectOptionDef // an enum/bool option row entry (type1/type2/sync/lookAhead or the effect's own)
{
    std::string_view key, label, shortLabel;
    std::span<const std::string_view> enumLabels{}; // empty = Bool
    float defInsert = 0.0f, defSend = 0.0f, defMaster = 0.0f;
};

struct EffectParamSet // what registerEffect recorded for one effect
{
    std::string_view effect;
    std::span<const EffectParamDef> params;
    std::span<const EffectOptionDef> options;
    std::uint8_t slotMask = 0;
    std::string_view featureFlag;
};

// Registers the ParamSpecs (see the header comment). `defs` and `options` must outlive the registry (static arrays).
void registerEffect(ParamRegistry&, std::string_view effect, std::span<const EffectParamDef> defs,
                    std::uint8_t slotMask, std::string_view featureFlag = {},
                    std::span<const EffectOptionDef> options = {});
const EffectParamSet* effectParams(std::string_view effect) noexcept; // after builtinParams() (or registerEffect)
const EffectParamDef* paramByK(const EffectParamSet&, int k) noexcept;
// ParamId.value → internal index k of an FX knob ParamSpec, −1 elsewhere (engine::fx::FxSlotBank::build).
std::vector<std::int8_t> internalIndexTable(const ParamRegistry&);
// The slot's default of def (kSlotInsert / kSlotSend / kSlotMaster).
float defaultFor(const EffectParamDef&, std::uint8_t slot) noexcept;

// model::EffectType ↔ the registry id ("chorus", "flanger", "eq2", "filter", "delay", "delayBus", "reverb", "lofi",
// "distortion", "compressor", "ducker", "phaser"); Off ↔ "".
std::string_view effectIdOf(model::EffectType) noexcept;
model::EffectType effectTypeOf(std::string_view id) noexcept;

// Step law (FX-063): i = roundToInt(f32(lo + f32(v·(hi − 1 − lo)))) (half to even); v = f32(f32(i − lo)/f32(hi − 1 −
// lo)).
int stepIndex(float v, Grid) noexcept;
float valueOfStep(int i, Grid) noexcept;
float step(float v, Grid, int delta, int multiplier) noexcept;               // FX-063
int coarseMultiplier(Grid, Family, float v, bool sync, bool shift) noexcept; // FX-064 (our law)
float convertSync(float v, Family, bool toSync, double bpm) noexcept;        // FX-073 (float32 arithmetic)
float ccToValue(int cc, Grid) noexcept;                                      // FX-086
// FX-068 value text into buf (≤ n − 1 chars + NUL); returns the length.
int format(Family, float v, bool sync, char* buf, int n) noexcept;
// FX-1210 Ducker sync tables (Sixteen's own, by formula): sixteenths at step k = 0…127.
float duckerSixteenths(Family attackOrRelease, int k) noexcept;
// Beats (rates: per LFO cycle; delays: of delay) of sync step i (FX-072 divisions), and its "num/den" text.
float syncBeats(Family, int i) noexcept;
} // namespace core::fx
