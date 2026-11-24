// The model-backed ParamAccess the composition root plugs into the shell (ParamAccess.h; state-and-undo.md, EDT-U1/U2,
// UID-160/166): every knob-strip edit of a parameter that has a document-model field becomes an undoable command on
// the CommandBus (one SetModelParam per edit; consecutive edits of one path coalesce into one undo step, EDT-U2), and
// the bus's bridge hook posts the change to the engine (forwardModelChanges). Parameters without a model field yet
// (preferences, BROWSE settings, PLAYBACK GRID SNAP, the FX knobs until the model stores them by ParamSpec) stay in a
// LocalParamValues fallback that posts straight to the engine, exactly as before the model existed.
//
// The table covers the track parameters: track.* strip / type / trigger / choke / colour / velocity,
// playback.*, env.*, scale.* / scaleMidi.*, and the project mixer levels; lfo.* (shape and
// destination kept in the LFO's extras where the frozen model has no field, see the .cpp) and the SEQUENCE track
// values seq.offset / retrigCount / retrigSpeed / transpose / shiftArea (core::perf::setStepsParam's paths).
// Then the routing: routing.outAssign (track), routing.assignMode / headphoneCue (scene fields:
// path "scene[s].<id>", engine scope = the scene) and routing.monitorLevel (project). Values are the ParamSpec's units (the
// engine's wire units); the model conversions are here (pitch cents ↔ semitones; envelope index ↔ normalised
// storage, ENV-001).
//
// Model paths follow the ParamPath grammar (core/model/paths): "scene[s].track[t].<id without track.>" for track ids,
// the id itself for project ids — the same paths core/commands/track and core/commands/library write, so
// forwardTrackChanges and forwardModelChanges see every edit whoever made it.
//
// `track.mute` is the exception: get() / set() are the user's per-pattern mute (cmd::track::isMuted / setMuted, the
// current pattern's `trackMutes`); the engine and forwardModelChanges still see the raw track-level field.
//
// Message thread only. get() never allocates (a table lookup by registry index).
#pragma once

#include "core/commands/Command.h"
#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/UiState.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core::model
{
struct Project;
}

namespace ui
{
struct ModelField; // one row of the table (ModelParamAccess.cpp)

class ModelParamAccess final : public ParamAccess
{
public:
    ModelParamAccess(core::CommandBus&, engine::EngineBridge*, const core::ParamRegistry& = core::builtinParams());
    ~ModelParamAccess() override;

    float get(const core::ParamSpec&, std::uint16_t scope) const override;
    void set(const core::ParamSpec&, std::uint16_t scope, float value) override;
    // A value the engine already applied (an incoming CC): the model follows without an undo step
    // (non-undoable command; the caller coalesces per frame); values outside the model go to the fallback.
    void setRemote(const core::ParamSpec&, std::uint16_t scope, float value);
    // Step locks belong to the pattern model (StepLockAccess); here they stay in the fallback.
    bool lockOf(const core::ParamSpec&, std::uint16_t scope, int step, float& value) const override;
    void setLock(const core::ParamSpec&, std::uint16_t scope, int step, float value) override;

    bool modelBacked(const core::ParamSpec&) const noexcept; // true when the value lives in the model
    LocalParamValues& fallback() noexcept { return fallback_; }

private:
    const ModelField* fieldOf(const core::ParamSpec&) const noexcept;
    core::CommandBus& bus_;
    const core::ParamRegistry& params_;
    LocalParamValues fallback_;
    std::vector<const ModelField*> byIndex_; // registry index → field (null = fallback)
    const ModelField* muteField_;            // "track.mute": get / set are the per-pattern mute (TrackCommands.h)
};

// The bridge hook's half for the table's parameters: every changed path that names one becomes a ParamChange with
// the model's value. The six track fields core/commands/track forwards (mute, mode, triggerMode, chokeGroup, padColor,
// padVelocity: ui::input::pads::forwardTrackChanges) are skipped, so the hook calls both without double posts.
void forwardModelChanges(const core::ChangeSet&, const core::model::Project&, engine::EngineBridge&,
                         const core::ParamRegistry& = core::builtinParams());
// After a project is opened or replaced: posts every table parameter of the current scene's 16 tracks (all of them,
// including the six above) and the project parameters, so the engine matches the document. ≤ 16 × 55 messages.
// `scene` ≥ 0: that scene's tracks instead of the current one (resident scenes: the composition root posts all 16).
void forwardAllModel(const core::model::Project&, engine::EngineBridge&,
                     const core::ParamRegistry& = core::builtinParams(), int scene = -1);

// HOME / MIXER / context-bar track summaries (UiState::tracks) from the model: name (the pool entry's display name,
// else the file name of its path), type / stretch badges, loop glyph, content, mute, solo. The views point into the
// project, so call it every frame before the shell's frame() (cheap: 16 tracks, no allocation). `loading(t)` (may be
// null) marks a track whose sample is still decoding as having content.
void fillTrackSummaries(UiState&, const core::model::Project&, bool (*loading)(const void*, int track0) = nullptr,
                        const void* ctx = nullptr) noexcept;
} // namespace ui
