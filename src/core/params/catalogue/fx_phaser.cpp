// Parameter catalogue of the Phaser (spec 02-features/effects/phaser.md "Parameters", FX-362: the same
// defaults in every slot type, Mix 0). ParamSpecs fx1|fx2|send|master.phaser.<key> through core::fx::registerEffect;
// the engine side is engine/fx/effects/Phaser.cpp (descriptor "phaser", knobs fx.phaser.<key>, k = internal index
// 0…5). RATE has the Phaser family's SYNC form (`<id>.div`, 64 … 1/16 beats per cycle).
// Our units (F170, effects.md): WIDTH and FEEDBACK in %, PHASE in degrees, FREQUENCY in Hz/kHz.
#include "core/params/catalogue/fx.h"

#include <cmath>
#include <cstdio>

namespace core::fx
{
namespace
{
constexpr Grid kWidth{0, 101}, kPhase{0, 181}, kFrequency{0, 257}, kFeedback{0, 100}, kMix{0, 101};

int put(int n, int len) noexcept
{
    return len < 0 ? 0 : (len < n ? len : n - 1);
}
int fmtPercent(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%.0f %%", 100.0 * static_cast<double>(v)));
}
int fmtPhase(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d\xc2\xb0", stepIndex(v, kPhase)));
}
int fmtFrequency(float v, char* b, int n) noexcept // f = 300·60^(rint(256v)/256): 300 Hz … 18 kHz
{
    const double hz = 300.0 * std::pow(60.0, stepIndex(v, kFrequency) / 256.0);
    if (hz >= 1000.0)
        return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%.1f kHz", hz / 1000.0));
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%.0f Hz", hz));
}
int fmtFeedback(float v, char* b, int n) noexcept // fb = 0.99v
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%.0f %%", 99.0 * static_cast<double>(v)));
}
int fmtMix(float v, char* b, int n) noexcept
{
    return put(n, std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kMix)));
}

constexpr float kRateDefault = 199.0f / 999.0f; // 2.00 Hz

constexpr EffectParamDef kParams[] = {
    {.key = "rate",
     .label = "RATE",
     .shortLabel = "RATE",
     .k = 0,
     .free = {1, 1001},
     .sync = {0, 12},
     .family = Family::Phaser,
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
     .format = &fmtPercent},
    {.key = "phase", .label = "PHASE", .shortLabel = "PHASE", .k = 2, .free = kPhase, .format = &fmtPhase},
    {.key = "frequency",
     .label = "FREQUENCY",
     .shortLabel = "FREQ",
     .k = 3,
     .free = kFrequency,
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtFrequency},
    {.key = "feedback", .label = "FEEDBACK", .shortLabel = "FDBK", .k = 4, .free = kFeedback, .format = &fmtFeedback},
    {.key = "mix", .label = "MIX RATIO", .shortLabel = "MIX", .k = 5, .free = kMix, .format = &fmtMix},
};
constexpr EffectOptionDef kOptions[] = {
    {.key = "sync", .label = "SYNC", .shortLabel = "SYNC"},
};

void addPhaserParams(ParamRegistry& r)
{
    registerEffect(r, "phaser", kParams, kSlotInsert | kSlotSend | kSlotMaster, {}, kOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_phaser", 712, &core::fx::addPhaserParams)
