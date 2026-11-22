// Parameter catalogue of the Flanger (spec 02-features/effects/flanger.md "Parameters", FX-200, FX-263:
// the same defaults in every slot type, Mix 0). ParamSpecs fx1|fx2|send|master.flanger.<key> through
// core::fx::registerEffect; the engine side is engine/fx/effects/Flanger.cpp (descriptor "flanger", knobs
// fx.flanger.<key>, k = internal index 0…5). RATE has the Flanger family's SYNC form (`<id>.div`, 64 … 1 beats).
// Our units (FX-200): WIDTH in %, DELAY in ms (s = the maximum sweep in ms at WIDTH 100), FEEDBACK in % (0–99).
#include "core/params/catalogue/fx.h"

#include <cstdio>

namespace core::fx
{
namespace
{
constexpr Grid kWidth{0, 101}, kPhase{0, 181}, kDelay{1, 71}, kFeedback{0, 100}, kMix{0, 101};

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
int fmtDelay(float v, char* b, int n) noexcept // s = 0.1·rint(1 + 69v): 0.1 … 7.0 ms
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%.1f ms", 0.1 * stepIndex(v, kDelay)));
}
int fmtFeedback(float v, char* b, int n) noexcept // fb = rint(99v)/100
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kFeedback)));
}
int fmtMix(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kMix)));
}

constexpr float kRateDefault = 19.0f / 99.0f; // 0.20 Hz

constexpr EffectParamDef kParams[] = {
    {.key = "rate",
     .label = "RATE",
     .shortLabel = "RATE",
     .k = 0,
     .free = {1, 101},
     .sync = {0, 7},
     .family = Family::Flanger,
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
    {.key = "feedback", .label = "FEEDBACK", .shortLabel = "FDBK", .k = 4, .free = kFeedback, .format = &fmtFeedback},
    {.key = "mix", .label = "MIX RATIO", .shortLabel = "MIX", .k = 5, .free = kMix, .format = &fmtMix},
};
constexpr EffectOptionDef kOptions[] = {
    {.key = "sync", .label = "SYNC", .shortLabel = "SYNC"},
};

void addFlangerParams(ParamRegistry& r)
{
    registerEffect(r, "flanger", kParams, kSlotInsert | kSlotSend | kSlotMaster, {}, kOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_flanger", 702, &core::fx::addFlangerParams)
