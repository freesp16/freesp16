// Parameter catalogue, area ROUTING (02-features/mixer-and-routing.md MIX-030/031, MIX-040, MIX-050/051; 03-ui/
// enumerations.md "Routing and outputs"). One file per area (UID-200/201). The engine binds these ids in engine::Engine
// → engine/mixer/OutputRouterStage.h (ParamTarget::Routing). The HEADPHONES level and the output trim are in mixer.cpp
// (mixer.headphoneVolume, mixer.outputTrim). Order band 1200.
//
// Engine scopes (ParamChange::scope): routing.assignMode / headphoneCue = the scene; routing.outAssign / thruBypass =
// (scene << 4) | track; routing.monitorLevel / monitor = 0.
#include "core/params/ParamRegistry.h"

namespace core
{
namespace
{
// ASSIGN MODE (M p.23): stored Master / 4Assign (core::model::AssignMode order).
constexpr std::string_view assignModeLabels[] = {"M + 6CH", "8CH"};
// OUT ASSIGN (core::model::OutputAssign order). "--" = master only (M + 6CH; OUT1-2 in 8CH); OUT1-2, 1 and 2
// are not offered in M + 6CH (the UI filters the list; the engine treats them as "--").
constexpr std::string_view outAssignLabels[] = {"--", "OUT1-2", "OUT3-4", "OUT5-6", "OUT7-8", "1", "2",
                                                "3",  "4",      "5",      "6",      "7",      "8"};
// HP CUE (core::model::HeadphoneCue order; stored CueMaster, Cue12 … Cue8). MASTER is the 8CH list's odd one out and
// OUT1-2 / 1 / 2 the M + 6CH list's: the engine corrects them per mode (MIX-040).
constexpr std::string_view cueLabels[] = {"MASTER", "OUT1-2", "OUT3-4", "OUT5-6", "OUT7-8", "1", "2",
                                          "3",      "4",      "5",      "6",      "7",      "8"};
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};

void addRoutingParams(ParamRegistry& r)
{
    // MIX-030: output mode per scene (RoutingData.assignMode).
    r.add({.id = "routing.assignMode", .label = "ASSIGN MODE", .shortLabel = "ASSIGN", .group = "OUTPUT",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = assignModeLabels,
           .scope = ParamScope::Scene, .storage = {.key = "routing.assignMode", .reference = "assignMode"}});
    // MIX-031: per track OUT ASSIGN (TrackRoutingData.assignableOutCh), knob 3 on OUTPUT SETTING.
    r.add({.id = "routing.outAssign", .label = "OUT ASSIGN", .shortLabel = "OUT", .group = "OUTPUT",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 12, .defaultValue = 0, .enumLabels = outAssignLabels,
           .scope = ParamScope::Track, .storage = {.key = "routing.output", .reference = "assignableOutCh"}});
    // MIX-040: HP CUE per scene (headphoneOutCh).
    r.add({.id = "routing.headphoneCue", .label = "HP CUE", .shortLabel = "HP CUE", .group = "OUTPUT",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 12, .defaultValue = 0, .enumLabels = cueLabels,
           .scope = ParamScope::Scene, .storage = {.key = "routing.headphoneSource", .reference = "headphoneOutCh"}});
    // MIX-050 / UI-323: MONITOR LEVEL 0–127, 100 = the mixer balance unchanged (unity).
    r.add({.id = "routing.monitorLevel", .label = "MONITOR LEVEL", .shortLabel = "MON LVL", .group = "OUTPUT",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 127, .defaultValue = 100, .coarseStep = 1,
           .fineStep = 1, .scope = ParamScope::Project,
           .storage = {.key = "monitorVolume", .reference = "monitorVolume"}});
    // MIX-050: input monitor on/off — runtime state the sampler switches, never saved.
    r.add({.id = "routing.monitor", .label = "INPUT MONITOR", .shortLabel = "MONITOR", .group = "OUTPUT",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Global, .storage = {.key = "session.routing.monitor"}});
    // MIX-051 / TRK-005: the THRU page's BYPASS (default OFF): the track goes out around its FX1/FX2 (FX-045 semantics).
    r.add({.id = "routing.thruBypass", .label = "BYPASS", .shortLabel = "BYPASS", .group = "THRU",
           .type = ParamType::Bool, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOnLabels,
           .scope = ParamScope::Track, .storage = {.key = "track.thruBypass"}});
    // The panel's MAIN OUT pot (0–1023, the absolute word; never saved): as on the reference (signal-flow.md F113) it
    // sets PHONES and OUT3-4…7-8 digitally (the output-trim law) and only gates MAIN (0 = mute; the MAIN jacks have
    // their own analog volume after the DAC). Hardware feedback: the pot was decoded but bound to nothing.
    r.add({.id = "routing.mainKnob", .label = "MAIN OUT", .shortLabel = "MAIN OUT", .group = "OUTPUT",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 1023, .defaultValue = 1023, .coarseStep = 8,
           .fineStep = 1, .scope = ParamScope::Global, .storage = {.key = "internal.routing.mainKnob"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("routing", 1200, &core::addRoutingParams)
