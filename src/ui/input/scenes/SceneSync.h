// Keeps the engine's scene/pattern switcher in step with the document: the bridge half of the scene commands. Wiring:
// the composition root calls onChange() from the CommandBus bridge hook (core::commands::setBridge, next to the
// parameter translator and services::library::TrackSampleSync) and syncAll() after a project is opened. Message thread
// only.
//
//   queue[<v>]                       (selectScenePattern & co.) → ParamChange{scene.queue, 0xFFFF, v}
//   scene[s]                         → scene.bpm (scope s) + the 16 patterns below
//   scene[s].bpm                     → scene.bpm (scope s; 0 = none)
//   scene[s].pattern[p][.…]          → pattern.length, pattern.mutes, pattern.solos (scope (s << 4) | p)
//   settings / settings.patternQuantize → project.patternQuantize (STEP 0, BAR 1, PATTERN 2; OFF → STEP,
//                                       BEAT → STEP until the engine has a BEAT unit)
// The preferences scene.bpmPolicy / scene.switchMode are not document values: ParamAccess posts them directly.
#pragma once

#include "core/commands/Command.h"
#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"

#include <cstdint>
#include <optional>

namespace core::model
{
struct Project;
struct Scene;
struct Pattern;
} // namespace core::model

namespace ui::input::scenes
{
class SceneSync
{
public:
    SceneSync(const core::ParamRegistry&, engine::EngineBridge&);

    void onChange(const core::ChangeSet&, const core::model::Project&);
    // Every scene value of the project (16 BPMs, 256 × length / mutes / solos, the quantize unit) and an immediate
    // switch to its current scene/pattern (the transport is stopped after an open).
    void syncAll(const core::model::Project&);
    // SCN-023: blocks / allows switching in the engine (SAMPLING / EDIT screens).
    void setSwitchable(bool);

    std::uint32_t posted() const noexcept { return posted_; } // ParamChanges accepted by the bridge (tests)

    // The engine values of the model (also used by tests).
    static std::uint16_t muteMask(const core::model::Pattern&) noexcept;
    static std::uint16_t soloMask(const core::model::Pattern&) noexcept;
    static int quantizeIndex(const core::model::Project&) noexcept; // 0 STEP, 1 BAR, 2 PATTERN

private:
    void post(const std::optional<core::ParamId>& id, std::uint16_t scope, float value);
    void postScene(const core::model::Project&, int scene);
    void postPattern(const core::model::Project&, int scene, int pattern);

    engine::EngineBridge& bridge_;
    std::optional<core::ParamId> queue_, bpm_, length_, mutes_, solos_, quantize_;
    std::uint32_t posted_ = 0;
};
} // namespace ui::input::scenes
