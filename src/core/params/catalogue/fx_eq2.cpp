// ParamSpecs of the 2-band EQ "eq2" (spec 02-features/effects/parametric-eq.md "Parameters",
// parameters.md FX-061/067/069): fx1/fx2/send/master.eq2.<key>, registered through core::fx::registerEffect. The engine
// side is engine/fx/effects/ParametricEq (descriptor "eq2", knobs in page order B1 Q, B1 Freq, B1 Gain, B2 Q, B2 Freq,
// B2 Gain; the internal index k below routes each knob, FX-060).
//
//   k    key             grid [lo, hi)   value              default (Insert = Send = Master, F154/F156)
//   0/3  b1freq/b2freq   [0, 1001)       f = 20·1000^v Hz   0.566 (≈ 998 Hz)
//   1/4  b1gain/b2gain   [−30, 16)       45v − 30 dB        2/3 (0 dB)
//   2/5  b1q/b2q         [1, 101)        0.1 + 9.9v         6/99 (Q 0.7)
//   type1 / type2  band 1 / band 2 type, stored in the UI order of FX-069 / screens/effects.md: HPF 0, LPF 1, PEAK 2,
//                  HI-SHELF 3, LO-SHELF 4 (our order, FX-070 typed values; the effect maps them to the reference's
//                  LowShelving 0 … HighPass 4 as 4 − v); defaults LO-SHELF / HI-SHELF
// Captions: screens/effects.md "The 12 effects". The option keys are the framework's type1/type2 (model::EffectSlot
// fields, ui ModelFxSlots / core FxCommands), not the UI spec's fx.eq2.band1Type / band2Type.
#include "core/params/catalogue/fx.h"

#include <cmath>
#include <cstdio>

namespace
{
using core::fx::EffectOptionDef;
using core::fx::EffectParamDef;

int clip(int len, int n) noexcept
{
    return len < 0 ? 0 : (len < n ? len : n - 1);
}
int formatFreq(float v, char* buf, int n) noexcept
{
    const double hz = 20.0 * std::pow(1000.0, static_cast<double>(v));
    if (hz < 1000.0)
        return clip(std::snprintf(buf, static_cast<std::size_t>(n), "%d Hz", static_cast<int>(std::lround(hz))), n);
    return clip(std::snprintf(buf, static_cast<std::size_t>(n), hz < 10000.0 ? "%.2f kHz" : "%.1f kHz", hz / 1000.0),
                n);
}
int formatGain(float v, char* buf, int n) noexcept
{
    const long db = std::lround(45.0 * static_cast<double>(v) - 30.0);
    return clip(std::snprintf(buf, static_cast<std::size_t>(n), db > 0 ? "+%ld dB" : "%ld dB", db), n);
}
int formatQ(float v, char* buf, int n) noexcept
{
    return clip(std::snprintf(buf, static_cast<std::size_t>(n), "%.1f", 0.1 + 9.9 * static_cast<double>(v)), n);
}

constexpr float kF = 0.566f, kG = 2.0f / 3.0f, kQ = 6.0f / 99.0f;
// page order (FX-067): B1 Q, B1 Freq, B1 Gain, B2 Q, B2 Freq, B2 Gain
constexpr EffectParamDef kEq2Params[] = {
    {.key = "b1q", .label = "B1 Q", .shortLabel = "B1 Q", .k = 2, .free = {1, 101},
     .defInsert = kQ, .defSend = kQ, .defMaster = kQ, .format = &formatQ},
    {.key = "b1freq", .label = "B1 FREQUENCY", .shortLabel = "B1 FREQ", .k = 0, .free = {0, 1001},
     .defInsert = kF, .defSend = kF, .defMaster = kF, .format = &formatFreq},
    {.key = "b1gain", .label = "B1 GAIN", .shortLabel = "B1 GAIN", .k = 1, .free = {-30, 16},
     .defInsert = kG, .defSend = kG, .defMaster = kG, .bipolar = true, .format = &formatGain},
    {.key = "b2q", .label = "B2 Q", .shortLabel = "B2 Q", .k = 5, .free = {1, 101},
     .defInsert = kQ, .defSend = kQ, .defMaster = kQ, .format = &formatQ},
    {.key = "b2freq", .label = "B2 FREQUENCY", .shortLabel = "B2 FREQ", .k = 3, .free = {0, 1001},
     .defInsert = kF, .defSend = kF, .defMaster = kF, .format = &formatFreq},
    {.key = "b2gain", .label = "B2 GAIN", .shortLabel = "B2 GAIN", .k = 4, .free = {-30, 16},
     .defInsert = kG, .defSend = kG, .defMaster = kG, .bipolar = true, .format = &formatGain},
};
constexpr std::string_view kBandLabels[] = {"HPF", "LPF", "PEAK", "HI-SHELF", "LO-SHELF"};
constexpr EffectOptionDef kEq2Options[] = {
    {.key = "type1", .label = "BAND1 TYPE", .shortLabel = "BAND1", .enumLabels = kBandLabels,
     .defInsert = 4.0f, .defSend = 4.0f, .defMaster = 4.0f},
    {.key = "type2", .label = "BAND2 TYPE", .shortLabel = "BAND2", .enumLabels = kBandLabels,
     .defInsert = 3.0f, .defSend = 3.0f, .defMaster = 3.0f},
};

void addEq2Params(core::ParamRegistry& r)
{
    core::fx::registerEffect(r, "eq2", kEq2Params, core::fx::kSlotInsert | core::fx::kSlotSend | core::fx::kSlotMaster,
                             {}, kEq2Options);
}
} // namespace

APP_REGISTER_PARAMS("fx_eq2", 703, &addEq2Params)
