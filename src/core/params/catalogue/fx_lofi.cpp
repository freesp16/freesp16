// Parameter catalogue of the LoFi effect (spec 02-features/effects/lofi.md "Parameters", FX-961: the
// same defaults in every slot type). ParamSpecs fx1|fx2|send|master.lofi.<key> through core::fx::registerEffect; the
// engine side is engine/fx/effects/LoFi.cpp (descriptor "lofi", knobs fx.lofi.<key> in page order = internal index).
// Labels and option texts are our own (H019, effects.md): FILTER TYPE HP/LP/BP/OFF, POSITION PRE/POST (FX-982: the
// reference's position values 1 and 2 are the same, the app offers two).
#include "core/params/catalogue/fx.h"

#include <cmath>
#include <cstdio>

namespace core::fx
{
namespace
{
int put(char*, int n, int len) noexcept
{
    return len < 0 ? 0 : (len < n ? len : n - 1);
}
int fmtHz(double hz, char* buf, int n) noexcept
{
    if (hz >= 1000.0)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f kHz", hz / 1000.0));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f Hz", hz));
}
int fmtPreamp(float v, char* buf, int n) noexcept // 0–42 dB drive, 1 dB steps
{
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f dB", 42.0 * static_cast<double>(v)));
}
int fmtReduction(float v, char* buf, int n) noexcept // rate = round(44.1·1000^v) Hz; 1.0 = off
{
    if (v >= 1.0f)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "OFF"));
    return fmtHz(std::round(44.1 * std::pow(1000.0, static_cast<double>(v))), buf, n);
}
int fmtBits(float v, char* buf, int n) noexcept // [1…16, 24, off][round(17v)]
{
    constexpr int kBits[18] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 0};
    const double x = static_cast<double>(v * 17.0f); // the engine's index: round half to even of f32(17v)
    int i = static_cast<int>(std::floor(x));
    const double fr = x - static_cast<double>(i);
    if (fr > 0.5 || (fr == 0.5 && (i & 1) != 0))
        ++i;
    i = i < 0 ? 0 : (i > 17 ? 17 : i);
    if (kBits[i] == 0)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "OFF"));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%d bit", kBits[i]));
}
int fmtCutoff(float v, char* buf, int n) noexcept // 20·1000^v Hz
{
    return fmtHz(20.0 * std::pow(1000.0, static_cast<double>(v)), buf, n);
}
int fmtResonance(float v, char* buf, int n) noexcept // Q = 0.2 + 9.8v
{
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f", 0.2 + 9.8 * static_cast<double>(v)));
}
int fmtPercent(float v, char* buf, int n) noexcept
{
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f %%", 100.0 * static_cast<double>(v)));
}

constexpr float kResonanceDefault = 5.0f / 98.0f; // Q 0.7 (the reference's f32(5/98))

constexpr EffectParamDef kLoFiParams[] = {
    {.key = "preamp", .label = "PREAMP", .shortLabel = "PREAMP", .k = 0, .free = {0, 43}, .format = &fmtPreamp},
    {.key = "reduction",
     .label = "REDUCTION",
     .shortLabel = "REDUCE",
     .k = 1,
     .free = {0, 128},
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtReduction},
    {.key = "bits",
     .label = "BIT DEPTH",
     .shortLabel = "BITS",
     .k = 2,
     .free = {0, 18},
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtBits},
    {.key = "cutoff",
     .label = "CUT OFF",
     .shortLabel = "CUTOFF",
     .k = 3,
     .free = {0, 1001},
     .defInsert = 0.566f,
     .defSend = 0.566f,
     .defMaster = 0.566f,
     .format = &fmtCutoff},
    {.key = "resonance",
     .label = "RESONANCE",
     .shortLabel = "RESO",
     .k = 4,
     .free = {2, 101},
     .defInsert = kResonanceDefault,
     .defSend = kResonanceDefault,
     .defMaster = kResonanceDefault,
     .format = &fmtResonance},
    {.key = "mix",
     .label = "MIX RATIO",
     .shortLabel = "MIX",
     .k = 5,
     .free = {0, 101},
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtPercent},
};
constexpr std::string_view kTypeLabels[] = {"HP", "LP", "BP", "OFF"};
constexpr std::string_view kPositionLabels[] = {"PRE", "POST"};
constexpr EffectOptionDef kLoFiOptions[] = {
    {.key = "type1",
     .label = "FILTER TYPE",
     .shortLabel = "TYPE",
     .enumLabels = kTypeLabels,
     .defInsert = 3.0f,
     .defSend = 3.0f,
     .defMaster = 3.0f},
    {.key = "type2",
     .label = "POSITION",
     .shortLabel = "POS",
     .enumLabels = kPositionLabels,
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f},
};

void addLoFiParams(ParamRegistry& r)
{
    registerEffect(r, "lofi", kLoFiParams, kSlotInsert | kSlotSend | kSlotMaster, {}, kLoFiOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_lofi", 708, &core::fx::addLoFiParams)
