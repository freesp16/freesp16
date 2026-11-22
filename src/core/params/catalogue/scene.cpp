// Parameter catalogue, area SCENE (02-features/scenes-and-patterns.md SCN-003/004/005/011/020/023/025). One file per
// area (UID-200/201). The engine binds these ids in engine::Engine → stages::SceneSwitchStage (engine/scene/). Order
// band 1100 (project/scene).
//
// Engine scopes (ParamChange::scope): scene.* = the scene; pattern.* = (scene << 4) | pattern (a pattern is
// addressed like a track; ParamScope has no Pattern value, so the specs say Scene and the commands/SceneSync post the
// pattern scope); project.* and the preferences = 0. Internal ids (scene.queue, pattern.mutes, pattern.solos) are engine
// messages, never saved: their storage key starts with "internal." and no strip shows them.
#include "core/commands/scene/SceneCommands.h"
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core
{
namespace
{
constexpr std::string_view colours[] = {"#01", "#02", "#03", "#04", "#05", "#06", "#07", "#08",
                                        "#09", "#10", "#11", "#12", "#13", "#14", "#15", "#16"};
constexpr std::string_view switchUnit[] = {"STEP", "BAR", "PATTERN"}; // reference patternQuantizeUnit (M p.42)
constexpr std::string_view bpmPolicy[] = {"APPLY", "ASK", "IGNORE"};  // SCN-005
constexpr std::string_view switchMode[] = {"RESTART", "CONTINUE"};    // SCN-025

int formatSceneBpm(float v, char* buf, int n) noexcept // 0 = the scene follows the project tempo
{
    return v <= 0 ? std::snprintf(buf, static_cast<std::size_t>(n), "---")
                  : std::snprintf(buf, static_cast<std::size_t>(n), "%.1f", static_cast<double>(v));
}

// scene.name holds text, which a ParamSpec value cannot: the value is a change counter and the column shows the
// focused scene's name (the SCENE MANAGER sets it, core::commands::scene::setFocusedSceneName).
int formatSceneName(float, char* buf, int n) noexcept
{
    const auto name = commands::scene::focusedSceneName();
    if (name.empty())
        return std::snprintf(buf, static_cast<std::size_t>(n), "---");
    return std::snprintf(buf, static_cast<std::size_t>(n), "%.*s", static_cast<int>(name.size()), name.data());
}

void addSceneParams(ParamRegistry& r)
{
    // ---- per scene (SCN-003/004) ----
    r.add({.id = "scene.name", .label = "NAME", .shortLabel = "NAME", .group = "SCENE", .type = ParamType::Integer,
           .minValue = -1e6f, .maxValue = 1e6f, .format = formatSceneName, .scope = ParamScope::Scene,
           .storage = {.reference = "name"}});
    r.add({.id = "scene.color", .label = "COLOR", .shortLabel = "COLOR", .group = "SCENE", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 15, .enumLabels = colours, .scope = ParamScope::Scene,
           .storage = {.key = "scene.colour", .reference = "colour"}});
    r.add({.id = "scene.bpm", .label = "SCENE BPM", .shortLabel = "SCN BPM", .group = "SCENE",
           .type = ParamType::Continuous, .minValue = 0, .maxValue = 300, .defaultValue = 0, .coarseStep = 1,
           .fineStep = 0.1f, .format = formatSceneBpm, .scope = ParamScope::SceneOverrideElseProject,
           .storage = {.reference = "bpm"}});
    // Engine message: scene × 16 + pattern (+256 = apply the scene BPM at this switch), −1 cancel, −2/−3 block/unblock
    // switching (engine::scene::kQueue*). Not saved, not shown.
    r.add({.id = "scene.queue", .label = "SCENE QUEUE", .shortLabel = "QUEUE", .group = "SCENE",
           .type = ParamType::Integer, .minValue = -3, .maxValue = 511, .defaultValue = -1, .scope = ParamScope::Project,
           .storage = {.key = "internal.scene.queue"}});

    // ---- per pattern (SCN-011/012) ----
    r.add({.id = "pattern.length", .label = "PATTERN LENGTH", .shortLabel = "LENGTH", .group = "SCENE",
           .type = ParamType::Integer, .minValue = 8, .maxValue = 64, .defaultValue = 16, .coarseStep = 1,
           .fineStep = 1, .scope = ParamScope::Scene, .storage = {.reference = "length"}});
    r.add({.id = "pattern.mutes", .label = "PATTERN MUTES", .shortLabel = "P.MUTES", .group = "SCENE",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 65535, .defaultValue = 0, .scope = ParamScope::Scene,
           .storage = {.key = "internal.pattern.mutes"}});
    r.add({.id = "pattern.solos", .label = "PATTERN SOLOS", .shortLabel = "P.SOLOS", .group = "SCENE",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 65535, .defaultValue = 0, .scope = ParamScope::Scene,
           .storage = {.key = "internal.pattern.solos"}});

    // ---- project (SCN-020: stored per project in 1.60) ----
    r.add({.id = "project.patternQuantize", .label = "PATTERN QUANTIZE", .shortLabel = "P.QUANT", .group = "SCENE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 2, .defaultValue = 2, .enumLabels = switchUnit,
           .scope = ParamScope::Project, .storage = {.key = "settings.patternQuantize",
                                                     .reference = "patternQuantizeUnit"}});

    // ---- preferences (SCN-005, SCN-025: settings file of the device) ----
    r.add({.id = "scene.bpmPolicy", .label = "SCENE BPM", .shortLabel = "SCN BPM", .group = "SCENE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 2, .defaultValue = 1, .enumLabels = bpmPolicy, // ASK
           .scope = ParamScope::Global});
    r.add({.id = "scene.switchMode", .label = "SWITCH MODE", .shortLabel = "SWITCH", .group = "SCENE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = switchMode,
           .scope = ParamScope::Global});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("scene", 1110, &core::addSceneParams)
