// Parameter catalogue of the Insert Delay (spec 02-features/effects/insert-delay.md "Parameters",
// parameters.md FX-060/067/072/092). ParamSpecs fx1|fx2.delay.<key> through core::fx::registerEffect (Insert only);
// the engine side is engine/fx/effects/InsertDelay.cpp (descriptor "delay", knobs fx.delay.<key> in page order =
// internal index k). Page order Time, Feedback, Hi cut, —, —, Mix; k3/k4 (LowCut, Spread) have no ParamSpec (F075).
// Time is the InsertDelay family: free grid [10, 1001) ms, SYNC grid [0, 6) divisions (the `.div` form, UID-830);
// option row: SYNC (Bool, the model's `sync` field). Labels (ours) follow the snapshot fixtures / effects.md (H019).
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
int fmtPercent(float v, char* buf, int n) noexcept // rint(100v) % (the feedback law g = rint(100v)/100)
{
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%d %%", stepIndex(v, Grid{0, 101})));
}
int fmtHiCut(float v, char* buf, int n) noexcept // step = trunc(999v) + 1; fc = 2000·10^((step − 1)/999) (FX-621)
{
    const float x = v * 999.0f;
    const int step = (x > 0.0f ? static_cast<int>(x) : 0) + 1;
    const double hz = 2000.0 * std::pow(10.0, static_cast<double>((step > 1000 ? 1000 : step) - 1) / 999.0);
    if (hz >= 1000.0)
        return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.1f kHz", hz / 1000.0));
    return put(buf, n, std::snprintf(buf, static_cast<std::size_t>(n), "%.0f Hz", hz));
}

constexpr float kTimeDefault = 49.0f / 99.0f; // 500 ms (F075)

constexpr EffectParamDef kParams[] = {
    {.key = "time",
     .label = "TIME",
     .shortLabel = "TIME",
     .k = 0,
     .free = {10, 1001},
     .sync = {0, 6},
     .family = Family::InsertDelay,
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
    {.key = "mix", .label = "MIX RATIO", .shortLabel = "MIX", .k = 5, .free = {0, 101}, .format = &fmtPercent},
};
constexpr EffectOptionDef kOptions[] = {{.key = "sync", .label = "SYNC", .shortLabel = "SYNC"}};

void addInsertDelayParams(ParamRegistry& r)
{
    registerEffect(r, "delay", kParams, kSlotInsert, {}, kOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_delay", 705, &core::fx::addInsertDelayParams)
