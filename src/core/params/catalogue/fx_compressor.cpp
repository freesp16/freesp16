// Parameter catalogue of the Compressor (spec 02-features/effects/compressor.md "Parameters",
// parameters.md FX-060/067/069/092; 07-ui-design/screens/effects.md "The 12 effects"). Knobs in page order Ratio,
// Attack, Release, Threshold, Gain, Mix (FX-067) with internal index k (FX-060); grids [lo, hi) as the page's table
// (the DSP uses the same step law, engine/fx/effects/Compressor.cpp qmap = core::fx::stepIndex); the neutral defaults
// in every slot kind (FX-092: threshold 0 dB, 1:1, 0.05 ms, 10 ms, 0 dB, Mix 100 %).
//
// Options (model fields, core::model::EffectSlot): type1 Knee (Bool: Hard / Soft 18 dB), type2 Detect (PEAK / RMS),
// lookAhead on Send/Master only: no fx1/fx2 ParamSpec, so the Insert panel's cell 1 stays empty (OQ-15, UID-827). The
// engine honours the option on an Insert too (FX-1163, P3, flag fx.compressor.insertLookAhead, off by default); its
// ParamSpec waits for the model and the panel to honour feature-flagged option specs.
#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"

#include <cstdio>

namespace core::fx
{
namespace
{
constexpr Grid kThreshold{-52, 1}, kRatio{10, 301}, kAttack{1, 1020}, kRelease{10, 4001}, kGain{0, 51}, kMix{0, 101};

int formatThreshold(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%d dB", stepIndex(v, kThreshold));
}
int formatRatio(float v, char* b, int n) noexcept
{
    const int i = stepIndex(v, kRatio);
    return std::snprintf(b, static_cast<std::size_t>(n), "%d.%d:1", i / 10, i % 10);
}
int formatAttack(float v, char* b, int n) noexcept // 0.05 … 1.00 ms in 0.05 ms steps, then 2 … 1000 ms
{
    const int i = stepIndex(v, kAttack);
    if (i <= 20)
        return std::snprintf(b, static_cast<std::size_t>(n), "%.2f ms", static_cast<double>(i) * 0.05);
    return std::snprintf(b, static_cast<std::size_t>(n), "%d ms", i - 19);
}
int formatRelease(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%d ms", stepIndex(v, kRelease));
}
int formatGain(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%d dB", stepIndex(v, kGain));
}
int formatMix(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kMix));
}

constexpr EffectParamDef kParams[] = {
    {.key = "ratio", .label = "RATIO", .shortLabel = "RATIO", .k = 1, .free = kRatio, .format = &formatRatio},
    {.key = "attack", .label = "ATTACK", .shortLabel = "ATTACK", .k = 2, .free = kAttack, .format = &formatAttack},
    {.key = "release",
     .label = "RELEASE",
     .shortLabel = "RELEASE",
     .k = 3,
     .free = kRelease,
     .format = &formatRelease},
    {.key = "threshold",
     .label = "THRESHOLD",
     .shortLabel = "THRESH",
     .k = 0,
     .free = kThreshold,
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &formatThreshold},
    {.key = "gain", .label = "GAIN", .shortLabel = "GAIN", .k = 4, .free = kGain, .format = &formatGain},
    {.key = "mix",
     .label = "MIX RATIO",
     .shortLabel = "MIX",
     .k = 5,
     .free = kMix,
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &formatMix},
};

constexpr std::string_view kDetectLabels[] = {"PEAK", "RMS"};
constexpr EffectOptionDef kLookAhead{.key = "lookAhead", .label = "LOOK AHEAD", .shortLabel = "LOOK AHD"};
constexpr EffectOptionDef kDetect{.key = "type2", .label = "DETECT", .shortLabel = "DETECT", .enumLabels = kDetectLabels};
constexpr EffectOptionDef kKnee{.key = "type1", .label = "SOFT KNEE", .shortLabel = "KNEE"};
constexpr EffectOptionDef kBusOptions[] = {kLookAhead, kDetect, kKnee}; // UID-827 cell order
constexpr EffectOptionDef kInsertOptions[] = {kDetect, kKnee};

void addCompressorParams(ParamRegistry& r)
{
    // The first call records the effect's parameter set (defaults, SYNC, the bridge's option list) for every slot.
    registerEffect(r, "compressor", kParams, kSlotSend | kSlotMaster, {}, kBusOptions);
    registerEffect(r, "compressor", kParams, kSlotInsert, {}, kInsertOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_compressor", 710, &core::fx::addCompressorParams)
