// Parameter catalogue of the Reverb (spec 02-features/effects/reverb.md "Parameters", parameters.md
// FX-060/067/092/093/094). ParamSpecs send|master.reverb.<key> through core::fx::registerEffect (not lockable, FX-094);
// the engine side is engine/fx/effects/Reverb.cpp (descriptor "reverb", knobs in page order Pre-delay, Room, Time,
// Damping, Spread, Mix = internal index k). Grids are the spec's own choice: [0, 101) for every knob except Room,
// [0, 100) with i = room − 1. Time keeps the spec's default 49/99 (8.70 s), which lies between two [0, 101) steps (the
// first edit snaps it to the grid, FX-063). Defaults: the reference's slot-52 values (H085, FX-092), Mix Send
// 1.0 / Master 0.5 (FX-093). No options, no SYNC.
//
// The value texts follow the engine's laws; the decay anchors below are engine::fx::ReverbConstants' (core may not
// include the engine; tests/unit/fx/test_fx_reverb.cpp checks the formatter against Reverb::t60).
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
int fmtPercent(float v, char* buf, int n) noexcept
{
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%d %%", stepIndex(v, Grid{0, 101})));
}
int fmtPreDelay(float v, char* buf, int n) noexcept // ms = 100v, at least 0.1 ms (FX-820)
{
    double ms = 100.0 * static_cast<double>(v);
    ms = ms < 0.1 ? 0.1 : (ms > 100.0 ? 100.0 : ms);
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f ms", ms));
}
int fmtRoom(float v, char* buf, int n) noexcept // room = trunc(1 + 99v), 1…100 (FX-821)
{
    const float x = 1.0f + 99.0f * v;
    const int room = x < 1.0f ? 1 : (x > 100.0f ? 100 : static_cast<int>(x));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%d", room));
}
constexpr double kT60v[5] = {0.0, 0.09, 0.3, 49.0 / 99.0, 1.0};
constexpr double kT60s[5] = {0.10, 0.87, 3.55, 8.70, 100.6};
int fmtTime(float v, char* buf, int n) noexcept // FX-822: log T60 linear in v between the anchors
{
    const double x = v < 0.0f ? 0.0 : (v > 1.0f ? 1.0 : static_cast<double>(v));
    int i = 0;
    while (i < 3 && x > static_cast<double>(static_cast<float>(kT60v[i + 1])))
        ++i;
    const double v0 = static_cast<float>(kT60v[i]), v1 = static_cast<float>(kT60v[i + 1]);
    const double l0 = std::log(static_cast<double>(static_cast<float>(kT60s[i])));
    const double l1 = std::log(static_cast<double>(static_cast<float>(kT60s[i + 1])));
    const double t = std::exp(l0 + (x - v0) / (v1 - v0) * (l1 - l0));
    if (t < 10.0)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.2f s", t));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f s", t));
}
int fmtDamping(float v, char* buf, int n) noexcept // step = trunc(1 + 99(1 − v)); 2000·9^((step − 1)/99), max 18 kHz
{
    const float x = 1.0f + (1.0f - v) * 99.0f;
    int step = x > 0.0f ? static_cast<int>(x) : 0;
    step = step < 1 ? 1 : (step > 100 ? 100 : step);
    const double hz = 2000.0 * std::pow(9.0, static_cast<double>(step - 1) / 99.0);
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f kHz", hz / 1000.0));
}

constexpr float kHalf = 49.0f / 99.0f; // room 50, 8.70 s

constexpr EffectParamDef kParams[] = {
    {.key = "preDelay", .label = "PRE DELAY", .shortLabel = "PRE DLY", .k = 0, .free = {0, 101}, .format = &fmtPreDelay},
    {.key = "room",
     .label = "ROOM SIZE",
     .shortLabel = "ROOM",
     .k = 1,
     .free = {0, 100},
     .defInsert = kHalf,
     .defSend = kHalf,
     .defMaster = kHalf,
     .format = &fmtRoom},
    {.key = "time",
     .label = "TIME",
     .shortLabel = "TIME",
     .k = 2,
     .free = {0, 101},
     .defInsert = kHalf,
     .defSend = kHalf,
     .defMaster = kHalf,
     .format = &fmtTime},
    {.key = "damping", .label = "DAMPING", .shortLabel = "DAMPING", .k = 3, .free = {0, 101}, .format = &fmtDamping},
    {.key = "spread", .label = "SPREAD", .shortLabel = "SPREAD", .k = 4, .free = {0, 101}, .format = &fmtPercent},
    {.key = "mix",
     .label = "MIX RATIO",
     .shortLabel = "MIX",
     .k = 5,
     .free = {0, 101},
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 0.5f,
     .format = &fmtPercent},
};

void addReverbParams(ParamRegistry& r)
{
    registerEffect(r, "reverb", kParams, kSlotSend | kSlotMaster);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_reverb", 707, &core::fx::addReverbParams)
