// Parameter catalogue of the Vintage Pitch Shifter (ours; engine side engine/fx/effects/VintagePitch.cpp, descriptor
// "vpitch", knobs fx.vpitch.<key> in page order = internal index k). Every slot kind; no SYNC form, no options.
//   k0 PITCH     −12 … +12 st (25 steps: one detent = one semitone)
//   k1 FINE      −50 … +50 cents (the doubler's detune)
//   k2 DELAY     0 … 120 ms added read delay
//   k3 FEEDBACK  −100 … +100 % (negative = the FB phase switch inverted)
//   k4 AGE       0 … 100 % (one macro over every service trim of a worn unit)
//   k5 MIX       0 … 100 % (the shell's CrossfadeK5)
// Defaults: a fifth up, dry, a third of the way to a worn unit, half wet — an obvious harmonizer on first selection.
#include "core/params/catalogue/fx.h"

#include <cstdio>

namespace core::fx
{
namespace
{
constexpr Grid kPitch{-12, 13}, kFine{-50, 51}, kDelay{0, 121}, kFeedback{-100, 101}, kPercent{0, 101};

int put(int n, int len) noexcept
{
    return len < 0 ? 0 : (len < n ? len : n - 1);
}
int fmtPitch(float v, char* b, int n) noexcept
{
    const int st = stepIndex(v, kPitch);
    if (st == 0)
        return put(n, std::snprintf(b, static_cast<std::size_t>(n), "0 st"));
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%+d st", st));
}
int fmtFine(float v, char* b, int n) noexcept
{
    const int ct = stepIndex(v, kFine);
    if (ct == 0)
        return put(n, std::snprintf(b, static_cast<std::size_t>(n), "0 ct"));
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%+d ct", ct));
}
int fmtDelay(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d ms", stepIndex(v, kDelay)));
}
int fmtFeedback(float v, char* b, int n) noexcept
{
    const int pct = stepIndex(v, kFeedback);
    if (pct == 0)
        return put(n, std::snprintf(b, static_cast<std::size_t>(n), "0 %%"));
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%+d %%", pct));
}
int fmtPercent(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kPercent)));
}

constexpr float kPitchDefault = 19.0f / 24.0f; // +7 st
constexpr float kAgeDefault = 0.35f;

constexpr EffectParamDef kParams[] = {
    {.key = "pitch",
     .label = "PITCH",
     .shortLabel = "PITCH",
     .k = 0,
     .free = kPitch,
     .defInsert = kPitchDefault,
     .defSend = kPitchDefault,
     .defMaster = kPitchDefault,
     .bipolar = true,
     .format = &fmtPitch},
    {.key = "fine",
     .label = "FINE",
     .shortLabel = "FINE",
     .k = 1,
     .free = kFine,
     .defInsert = 0.5f,
     .defSend = 0.5f,
     .defMaster = 0.5f,
     .bipolar = true,
     .format = &fmtFine},
    {.key = "delay", .label = "DELAY", .shortLabel = "DELAY", .k = 2, .free = kDelay, .format = &fmtDelay},
    {.key = "feedback",
     .label = "FEEDBACK",
     .shortLabel = "FEEDBACK",
     .k = 3,
     .free = kFeedback,
     .defInsert = 0.5f,
     .defSend = 0.5f,
     .defMaster = 0.5f,
     .bipolar = true,
     .format = &fmtFeedback},
    {.key = "age",
     .label = "AGE",
     .shortLabel = "AGE",
     .k = 4,
     .free = kPercent,
     .defInsert = kAgeDefault,
     .defSend = kAgeDefault,
     .defMaster = kAgeDefault,
     .format = &fmtPercent},
    {.key = "mix",
     .label = "MIX RATIO",
     .shortLabel = "MIX",
     .k = 5,
     .free = kPercent,
     .defInsert = 0.5f,
     .defSend = 1.0f,
     .defMaster = 0.5f,
     .format = &fmtPercent},
};

void addVintagePitchParams(ParamRegistry& r)
{
    registerEffect(r, "vpitch", kParams, kSlotInsert | kSlotSend | kSlotMaster);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_vpitch", 713, &core::fx::addVintagePitchParams)
