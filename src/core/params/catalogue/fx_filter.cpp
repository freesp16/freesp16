// ParamSpecs of the Multi-mode filter "filter" (spec 02-features/effects/multimode-filter.md
// "Parameters", parameters.md FX-061/067/069): fx1/fx2/send/master.filter.<key>, registered through
// core::fx::registerEffect. The engine side is engine/fx/effects/MultiModeFilter (descriptor "filter").
//
//   k  key     grid [lo, hi)   value                      default (Insert = Send = Master, F175)
//   0  cutoff  [0, 1001)       fc = 20·1000^v Hz           0.566 (≈ 998 Hz)
//   1  gain    [−30, 16)       45v − 30 dB                 2/3 (0 dB)
//   2  q       [2, 101)        0.2 + 9.8v                  5/98 (Q 0.7)
//   3  glide   [0, 101)        G = 64·(1 + 8v): G/3 ms per decade of cutoff   0
//   4  —       unused (FX-062)
//   5  mix     [0, 101)        wet v, dry 1 − v            1.0
//   type1  Type   LPF, HPF, BPF, OFF   (the clean enum, FX-069/070/582; labels: screens/effects.md)
//   type2  Slope  12 dB/Oct, 24 dB/Oct
// Captions: screens/effects.md "The 12 effects" (CUT OFF, GAIN, RESONANCE, GLIDE, MIX RATIO; TYPE, SLOPE). The
// option keys are the framework's type1/type2 (model::EffectSlot fields, ui ModelFxSlots / core FxCommands), not
// the UI spec's fx.filter.type / fx.filter.slope.
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
int formatCutoff(float v, char* buf, int n) noexcept
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
    return clip(std::snprintf(buf, static_cast<std::size_t>(n), "%.1f", 0.2 + 9.8 * static_cast<double>(v)), n);
}
int formatGlide(float v, char* buf, int n) noexcept
{
    const double g = 64.0 * (1.0 + 8.0 * static_cast<double>(v));
    return clip(std::snprintf(buf, static_cast<std::size_t>(n), "%ld ms", std::lround(g / 3.0)), n);
}
int formatMix(float v, char* buf, int n) noexcept
{
    return clip(std::snprintf(buf, static_cast<std::size_t>(n), "%ld %%", std::lround(100.0 * static_cast<double>(v))),
                n);
}

constexpr EffectParamDef kFilterParams[] = {
    {.key = "cutoff", .label = "CUT OFF", .shortLabel = "CUT OFF", .k = 0, .free = {0, 1001},
     .defInsert = 0.566f, .defSend = 0.566f, .defMaster = 0.566f, .format = &formatCutoff},
    {.key = "gain", .label = "GAIN", .shortLabel = "GAIN", .k = 1, .free = {-30, 16},
     .defInsert = 2.0f / 3.0f, .defSend = 2.0f / 3.0f, .defMaster = 2.0f / 3.0f, .bipolar = true,
     .format = &formatGain},
    {.key = "q", .label = "RESONANCE", .shortLabel = "RESO", .k = 2, .free = {2, 101},
     .defInsert = 5.0f / 98.0f, .defSend = 5.0f / 98.0f, .defMaster = 5.0f / 98.0f, .format = &formatQ},
    {.key = "glide", .label = "GLIDE", .shortLabel = "GLIDE", .k = 3, .free = {0, 101},
     .defInsert = 0.0f, .defSend = 0.0f, .defMaster = 0.0f, .format = &formatGlide},
    {.key = "mix", .label = "MIX RATIO", .shortLabel = "MIX", .k = 5, .free = {0, 101},
     .defInsert = 1.0f, .defSend = 1.0f, .defMaster = 1.0f, .format = &formatMix},
};
constexpr std::string_view kTypeLabels[] = {"LPF", "HPF", "BPF", "OFF"};
constexpr std::string_view kSlopeLabels[] = {"12 dB/Oct", "24 dB/Oct"};
constexpr EffectOptionDef kFilterOptions[] = {
    {.key = "type1", .label = "TYPE", .shortLabel = "TYPE", .enumLabels = kTypeLabels},
    {.key = "type2", .label = "SLOPE", .shortLabel = "SLOPE", .enumLabels = kSlopeLabels},
};

void addFilterParams(core::ParamRegistry& r)
{
    core::fx::registerEffect(r, "filter", kFilterParams,
                             core::fx::kSlotInsert | core::fx::kSlotSend | core::fx::kSlotMaster, {}, kFilterOptions);
}
} // namespace

APP_REGISTER_PARAMS("fx_filter", 704, &addFilterParams)
