// Parameter catalogue, area EXTENSIONS / E11 (spec 02-features/tracks.md TRK-012, P3, our design; 03-ui/enumerations.md
// "TOGGLE"): the Toggle trigger mode as a per-track extension value. Order 9950 (after every other catalogue,
// ui_m2_stubs' 9910 included, so no existing ParamId moves). Behind the feature flag ext.trk.toggle: the message thread
// posts it only while ext.enabled + ext.trk.toggle are on (engine/trigger/ext/toggle/ToggleLatch.h).
//
// The id is `ext.toggle.track`, not the flag's key `ext.trk.toggle` (that key is the preference, PrefSpec ids and
// ParamSpec ids share one namespace). A separate ParamSpec instead of a third TRIGGER MODE value:
// core::model::TriggerMode is fixed (ONE SHOT / GATE, M p.38) and the TRIGGER MODE value list belongs to track.cpp. Not
// lockable (sequencer steps play One Shot whatever the trigger mode, SEQ-015), no LFO / strip target, no fixed CC.
// Stored in Track::extras as "native.ext.trk.toggle" (the native codec keeps unknown keys), not in the reference format
// (the exporter drops it).
#include "core/params/ParamRegistry.h"

namespace core
{
namespace
{
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};

void addExtToggleParams(ParamRegistry& r)
{
    r.add({.id = "ext.toggle.track", .label = "TOGGLE", .shortLabel = "TOGGLE", .group = "TRACK",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .featureFlag = "ext.trk.toggle", .scope = ParamScope::Track, .storage = {.key = "ext.toggle.track"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("ext_trk_toggle", 9950, &core::addExtToggleParams)
