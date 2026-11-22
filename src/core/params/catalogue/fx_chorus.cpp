// Parameter catalogue of the Chorus (spec 02-features/effects/chorus.md "Parameters", FX-100, FX-163:
// the same defaults in every slot type, Mix 0). ParamSpecs fx1|fx2|send|master.chorus.<key> through
// core::fx::registerEffect; the engine side is engine/fx/effects/Chorus.cpp (descriptor "chorus", knobs fx.chorus.<key>,
// k = internal index; k4 unused). RATE has the Chorus family's SYNC form (`<id>.div`, 16 … 1/16 beats per cycle).
// Our units (FX-100): WIDTH in %, DELAY in ms (s = the maximum sweep in ms at WIDTH 100), PHASE in degrees.
#include "core/params/catalogue/fx.h"

#include <cstdio>

namespace core::fx
{
namespace
{
constexpr Grid kWidth{0, 101}, kPhase{0, 181}, kDelay{10, 101}, kMix{0, 101};

int put(int n, int len) noexcept
{
    return len < 0 ? 0 : (len < n ? len : n - 1);
}
int fmtWidth(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kWidth)));
}
int fmtPhase(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d\xc2\xb0", stepIndex(v, kPhase)));
}
int fmtDelay(float v, char* b, int n) noexcept // s = 0.1·rint(10 + 90v): 1.0 … 10.0 ms
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%.1f ms", 0.1 * stepIndex(v, kDelay)));
}
int fmtMix(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kMix)));
}

constexpr float kRateDefault = 19.0f / 99.0f; // 2.0 Hz

constexpr EffectParamDef kParams[] = {
    {.key = "rate",
     .label = "RATE",
     .shortLabel = "RATE",
     .k = 0,
     .free = {1, 101},
     .sync = {0, 10},
     .family = Family::Chorus,
     .defInsert = kRateDefault,
     .defSend = kRateDefault,
     .defMaster = kRateDefault},
    {.key = "width",
     .label = "WIDTH",
     .shortLabel = "WIDTH",
     .k = 1,
     .free = kWidth,
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtWidth},
    {.key = "phase", .label = "PHASE", .shortLabel = "PHASE", .k = 2, .free = kPhase, .format = &fmtPhase},
    {.key = "delay", .label = "DELAY", .shortLabel = "DELAY", .k = 3, .free = kDelay, .format = &fmtDelay},
    {.key = "mix", .label = "MIX RATIO", .shortLabel = "MIX", .k = 5, .free = kMix, .format = &fmtMix},
};
constexpr EffectOptionDef kOptions[] = {
    {.key = "sync", .label = "SYNC", .shortLabel = "SYNC"},
};

void addChorusParams(ParamRegistry& r)
{
    registerEffect(r, "chorus", kParams, kSlotInsert | kSlotSend | kSlotMaster, {}, kOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_chorus", 701, &core::fx::addChorusParams)
