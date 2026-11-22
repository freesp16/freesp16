// Parameter catalogue, area RIBBON (02-features/touch-strip.md RIB-001…005/009/011). One file per area (UID-200/201).
// Order band 800 (performance/ribbon).
//
// Engine messages, never saved (storage key "internal.*", no strip shows them): the project's ribbon settings
// (core::model::RibbonSettings, edited with core/commands/ribbon and stored by the project codec) and the panel's
// MODE / HOLD state reach engine::ribbon::RibbonEngine as ParamChanges of these ids (io/controllers/StripSource.h
// posts them):
//   ribbon.mode         0–3 = PITCH, REPEAT, USER1, USER2 (core::ribbon::Mode), scope 0
//   ribbon.hold         0/1, scope 0
//   ribbon.pitchTop     topCent 0…+2400, ribbon.pitchBottom bottomCent −2400…0 (RIB-002), scope 0
//   ribbon.userTarget   the item's target ParamId value (−1 = empty), ribbon.userTop / ribbon.userBottom 0–1
//                       (normalised, like RibbonItem), scope = set × 8 + item (USER1 = set 0)
// Preference (SET, settings file): ribbon.applyTo — where PITCH and USER act: the held pads' tracks (the reference),
// the selected track, or every track (RIB-002; the last two are ours).
#include "core/params/ParamRegistry.h"

namespace core
{
namespace
{
constexpr std::string_view modeLabels[] = {"PITCH", "REPEAT", "USER1", "USER2"};
constexpr std::string_view offOnLabels[] = {"OFF", "ON"};
constexpr std::string_view applyLabels[] = {"HELD PADS", "SELECTED TRACK", "ALL TRACKS"};

ParamSpec internal(std::string_view id, std::string_view label, ParamType type, float lo, float hi, float def,
                   std::string_view key, std::string_view shortLabel = {})
{
    return {.id = id, .label = label, .shortLabel = shortLabel.empty() ? label : shortLabel, .group = "TOUCH STRIP", .type = type, .minValue = lo,
            .maxValue = hi, .defaultValue = def, .scope = ParamScope::Project, .storage = {.key = key}};
}

void addRibbonParams(ParamRegistry& r)
{
    auto mode = internal("ribbon.mode", "MODE", ParamType::Enum, 0, 3, 0, "internal.ribbon.mode");
    mode.enumLabels = modeLabels;
    r.add(mode);
    auto hold = internal("ribbon.hold", "HOLD", ParamType::Enum, 0, 1, 0, "internal.ribbon.hold");
    hold.enumLabels = offOnLabels;
    r.add(hold);
    auto top = internal("ribbon.pitchTop", "TOP", ParamType::Integer, 0, 2400, 200, "internal.ribbon.pitchTop");
    top.unit = "cent";
    r.add(top);
    auto bottom = internal("ribbon.pitchBottom", "BOTTOM", ParamType::Integer, -2400, 0, -200, "internal.ribbon.pitchBottom");
    bottom.unit = "cent";
    r.add(bottom);
    r.add(internal("ribbon.userTarget", "TARGET", ParamType::Integer, -1, 65534, -1, "internal.ribbon.userTarget"));
    r.add(internal("ribbon.userTop", "TOP VALUE", ParamType::Continuous, 0, 1, 1, "internal.ribbon.userTop", "TOP"));
    r.add(internal("ribbon.userBottom", "BOTTOM VALUE", ParamType::Continuous, 0, 1, 0, "internal.ribbon.userBottom", "BOTTOM"));

    r.add({.id = "ribbon.applyTo", .label = "STRIP ACTS ON", .shortLabel = "STRIP ON", .group = "TOUCH STRIP",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 2, .defaultValue = 0, .enumLabels = applyLabels,
           .scope = ParamScope::Global});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("ribbon", 800, &core::addRibbonParams)
