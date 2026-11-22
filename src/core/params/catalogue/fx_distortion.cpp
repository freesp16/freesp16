// Parameter catalogue of the Distortion effect (spec 02-features/effects/distortion.md "Parameters":
// the same defaults in every slot type, fully wet). ParamSpecs fx1|fx2|send|master.distortion.<key> through
// core::fx::registerEffect; the engine side is engine/fx/effects/Distortion.cpp (descriptor "distortion", knobs in page
// order drive, lo freq, hi freq, dynamics, tone, mix; internal index k: 0 drive, 1 tone, 2 lo freq, 3 hi freq,
// 4 dynamics, 5 mix). Labels are our own (F170).
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
int fmtDrive(float v, char* buf, int n) noexcept // 0–40 dB, 0.5 dB steps
{
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f dB", 40.0 * static_cast<double>(v)));
}
int fmtFreq(float v, char* buf, int n) noexcept // 20·10^(3v), clamped to 50 Hz – 20 kHz
{
    double hz = 20.0 * std::pow(10.0, 3.0 * static_cast<double>(v));
    hz = hz < 50.0 ? 50.0 : (hz > 20000.0 ? 20000.0 : hz);
    if (hz >= 1000.0)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f kHz", hz / 1000.0));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f Hz", hz));
}
int fmtDynamics(float v, char* buf, int n) noexcept // threshold; 0 = auto-gain off
{
    if (v <= 0.0f)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "OFF"));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f %%", 100.0 * static_cast<double>(v)));
}
int fmtPercent(float v, char* buf, int n) noexcept
{
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f %%", 100.0 * static_cast<double>(v)));
}

constexpr EffectParamDef kDistortionParams[] = { // page order (FX-067)
    {.key = "drive", .label = "DRIVE", .shortLabel = "DRIVE", .k = 0, .free = {0, 81}, .format = &fmtDrive},
    {.key = "loFreq", .label = "LO FREQ", .shortLabel = "LO FREQ", .k = 2, .free = {0, 1001}, .format = &fmtFreq},
    {.key = "hiFreq",
     .label = "HI FREQ",
     .shortLabel = "HI FREQ",
     .k = 3,
     .free = {0, 1001},
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtFreq},
    {.key = "dynamics",
     .label = "DYNAMICS",
     .shortLabel = "DYNAMIC",
     .k = 4,
     .free = {0, 101},
     .format = &fmtDynamics},
    {.key = "tone",
     .label = "TONE",
     .shortLabel = "TONE",
     .k = 1,
     .free = {0, 1001},
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtFreq},
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

void addDistortionParams(ParamRegistry& r)
{
    registerEffect(r, "distortion", kDistortionParams, kSlotInsert | kSlotSend | kSlotMaster);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_distortion", 709, &core::fx::addDistortionParams)
