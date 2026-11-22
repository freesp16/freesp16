// Parameter catalogue of the Ducker (spec 02-features/effects/ducker.md "Parameters", parameters.md
// FX-060/067/068/069/072/092; 07-ui-design/screens/effects.md). Insert only (FX-1244). Knobs in page order Attack,
// Attack curve, Release, Release curve, —, Depth (k = page position; k4 is unused, FX-062). Attack and Release have a
// SYNC form (`<id>.div`, the Sixteen tables of FX-1210, "%.2f" sixteenths; free "%.1f" ms, FX-068) through the
// DuckerAttack / DuckerRelease families of core::fx. Curves are shown −100 … +100 (0 = linear), Depth in %.
//
// Options (model fields): sync (Bool), type1 Source (TRACK 1 … TRACK 16 → 0 … 15; FX-1262: SelectFxType sets a new
// Ducker's Source to its own track). FX-1263 "Follow MIDI track" (flag fx.ducker.followMidi) is an engine option only
// (Ducker::kOptFollowMidi): the model has no field for an effect's own options yet.
#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"

#include <cstdio>

namespace core::fx
{
namespace
{
constexpr Grid kCurve{-100, 101}, kDepth{0, 101};

int formatCurve(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%d", stepIndex(v, kCurve));
}
int formatDepth(float v, char* b, int n) noexcept
{
    return std::snprintf(b, static_cast<std::size_t>(n), "%d %%", stepIndex(v, kDepth));
}

// Defaults (F089 insert defaults on the step grids): Attack 30 ms = step 300 of [0, 10001); Release ≈ 300 ms = step
// 3000 of [1, 80001) (0.037488); curves linear; Depth 100 %.
constexpr float kAttackDefault = 300.0f / 10000.0f;
constexpr float kReleaseDefault = 2999.0f / 79999.0f;

constexpr EffectParamDef kParams[] = {
    {.key = "attack",
     .label = "ATTACK",
     .shortLabel = "ATTACK",
     .k = 0,
     .free = {0, 10001},
     .sync = {0, 128},
     .family = Family::DuckerAttack,
     .defInsert = kAttackDefault,
     .defSend = kAttackDefault,
     .defMaster = kAttackDefault},
    {.key = "attackCurve",
     .label = "CURVE",
     .shortLabel = "CURVE",
     .k = 1,
     .free = kCurve,
     .defInsert = 0.5f,
     .defSend = 0.5f,
     .defMaster = 0.5f,
     .bipolar = true,
     .format = &formatCurve},
    {.key = "release",
     .label = "RELEASE",
     .shortLabel = "RELEASE",
     .k = 2,
     .free = {1, 80001},
     .sync = {0, 128},
     .family = Family::DuckerRelease,
     .defInsert = kReleaseDefault,
     .defSend = kReleaseDefault,
     .defMaster = kReleaseDefault},
    {.key = "releaseCurve",
     .label = "CURVE",
     .shortLabel = "CURVE",
     .k = 3,
     .free = kCurve,
     .defInsert = 0.5f,
     .defSend = 0.5f,
     .defMaster = 0.5f,
     .bipolar = true,
     .format = &formatCurve},
    {.key = "depth",
     .label = "DEPTH",
     .shortLabel = "DEPTH",
     .k = 5,
     .free = kDepth,
     .defInsert = 1.0f,
     .defSend = 1.0f,
     .defMaster = 1.0f,
     .format = &formatDepth},
};

constexpr std::string_view kTrackLabels[] = {"TRACK 1",  "TRACK 2",  "TRACK 3",  "TRACK 4",  "TRACK 5",  "TRACK 6",
                                             "TRACK 7",  "TRACK 8",  "TRACK 9",  "TRACK 10", "TRACK 11", "TRACK 12",
                                             "TRACK 13", "TRACK 14", "TRACK 15", "TRACK 16"};
constexpr EffectOptionDef kOptions[] = {
    {.key = "sync", .label = "SYNC", .shortLabel = "SYNC"},
    {.key = "type1", .label = "SOURCE", .shortLabel = "SOURCE", .enumLabels = kTrackLabels},
};

void addDuckerParams(ParamRegistry& r)
{
    registerEffect(r, "ducker", kParams, kSlotInsert, {}, kOptions);
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx_ducker", 711, &core::fx::addDuckerParams)
