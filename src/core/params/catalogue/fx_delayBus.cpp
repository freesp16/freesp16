// Parameter catalogue of the Send/Master Delay (spec 02-features/effects/send-delay.md "Parameters",
// parameters.md FX-060/067/072/092/093/094). ParamSpecs send|master.delayBus.<key> through core::fx::registerEffect
// (not lockable, FX-094); the engine side is engine/fx/effects/SendDelay.cpp (descriptor "delayBus", knobs in page
// order Time, Feedback, Hi cut, Lo cut, Spread, Mix = internal index k). Time is the SendDelay family: free grid
// [10, 4001) ms, SYNC grid [0, 8) divisions; the k1–k5 grids [0, 101) are the spec's own choice. Defaults: the
// reference's slot-52 values (H085, FX-092), Mix Send 1.0 / Master 0.5 (FX-093). Option row: SYNC.
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
int fmtHz(double hz, char* buf, int n) noexcept
{
    if (hz >= 1000.0)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f kHz", hz / 1000.0));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f Hz", hz));
}
int stepOf(float x) noexcept // trunc(999·x) + 1, clamped to 1…1000
{
    const float y = x * 999.0f;
    const int s = (y > 0.0f ? static_cast<int>(y) : 0) + 1;
    return s > 1000 ? 1000 : s;
}
int fmtHiCut(float v, char* buf, int n) noexcept // fc = 2000·10^((step − 1)/999) (FX-722)
{
    return fmtHz(2000.0 * std::pow(10.0, static_cast<double>(stepOf(v) - 1) / 999.0), buf, n);
}
int fmtLoCut(float v, char* buf, int n) noexcept // step of 1 − (1 − v)^4; fc = 1000^((step − 1)/999), 5 Hz floor
{
    const float x = 1.0f - v;
    const double hz = std::pow(1000.0, static_cast<double>(stepOf(1.0f - x * (x * (x * x))) - 1) / 999.0);
    if (hz <= 5.0) // FX-784: everything up to the 5 Hz floor sounds the same ("off")
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "OFF"));
    return fmtHz(hz, buf, n);
}

constexpr float kTimeDefault = 490.0f / 3990.0f; // 500 ms (0.122807, F115)

constexpr EffectParamDef kParams[] = {
    {.key = "time",
     .label = "TIME",
     .shortLabel = "TIME",
     .k = 0,
     .free = {10, 4001},
     .sync = {0, 8},
     .family = Family::SendDelay,
     .defInsert = kTimeDefault,
     .defSend = kTimeDefault,
     .defMaster = kTimeDefault},
    {.key = "feedback",
     .label = "FEEDBACK",
     .shortLabel = "FEEDBACK",
     .k = 1,
     .free = {0, 101},
     .defInsert = 0.7f,
     .defSend = 0.7f,
     .defMaster = 0.7f,
     .format = &fmtPercent},
    {.key = "hiCut",
     .label = "HI CUT",
     .shortLabel = "HI CUT",
     .k = 2,
     .free = {0, 101},
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &fmtHiCut},
    {.key = "loCut", .label = "LO CUT", .shortLabel = "LO CUT", .k = 3, .free = {0, 101}, .format = &fmtLoCut},
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
constexpr EffectOptionDef kOptions[] = {{.key = "sync", .label = "SYNC", .shortLabel = "SYNC"}};

void addSendDelayParams(ParamRegistry& r)
{
    registerEffect(r, "delayBus", kParams, kSlotSend | kSlotMaster, {}, kOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_delayBus", 706, &core::fx::addSendDelayParams)
