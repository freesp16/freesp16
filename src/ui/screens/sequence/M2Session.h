// What the ui::m2 screens (SEQUENCE, SCENE MANAGER, BPM/QUANTIZE, PROJECT) read and edit of the document: the model
// behind a core::CommandBus (steps, locks, scenes, patterns) and a few view helpers. Message thread only.
//
// The composition root installs its bus with Session::install(); with none installed the session owns a bus with a
// new default project (the emulator and the tests). The edits go through the commands in M2Commands.h (toggleStep,
// clearTriggers, quantizeOffsets, replaceScene, …), which forward to core::commands::sequencer / scene or replace a
// whole pattern / scene / track, so undo is exact.
//
// Per-step OFF SET, RETRIGGER CNT and RETRIGGER SPD are locks in the model's lock slots 15, 16 and 17 (sequencer.md
// "Lockable parameters", ParamIndex Offset / Retrigger count / Retrigger speed), in the units of their ParamSpecs.
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "ui/widgets/StepGrid.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace ui::m2
{
// Flag of the placeholder ParamSpecs of M2ParamStubs.cpp (ids no catalogue registered): the shell shows no column for
// them while it is off.
inline constexpr std::string_view kParamStubFlag = "ui.m2.paramStub";
// RENDER AUDIO is laid out behind this flag (the render service is services::render's).
inline constexpr std::string_view kRenderFlag = "render";

inline constexpr core::model::LockSlot kSlotOffset = 15, kSlotRetrigCount = 16, kSlotRetrigSpeed = 17;
inline constexpr int kRetrigSpeedDefault = 2; // 1/4 step = "1/64" (SEQ-030, M p.40; enumerations.md RETRIGGER SPD)

// The cell view the SEQUENCE screen and the overviews draw (ui/widgets/StepGrid.h).
using ui::StepCellView;
using ui::StepKind;

class Session
{
public:
    static Session& instance() noexcept;

    // The app's bus (model, undo, engine bridge); null = the session's own bus with a new default project.
    void install(core::CommandBus* bus);
    core::CommandBus& bus();
    const core::model::Project& project();
    // Bumped on every edit, undo and redo made through the session (screens rebuild cached views on change).
    std::uint32_t revision() const noexcept { return revision_; }
    void touch() noexcept { ++revision_; }

    void execute(std::unique_ptr<core::Command>);
    bool undo();
    bool redo();
    // Tests: drop an installed bus and start again from a new default project.
    void reset();

    // --- views (indices 0-based) ---
    StepCellView cell(int scene, int pattern, int track, int step);
    int patternLength(int scene, int pattern);
    bool trackHasTriggers(int scene, int pattern, int track);
    bool patternHasContent(int scene, int pattern); // any trigger (SCN-026)
    bool sceneHasContent(int scene);                // any pattern with content, a named scene or a scene BPM
    // Bit p = pattern p of the scene holds a trigger; cached per revision (SCENE MANAGER draws 32 keys a frame).
    std::uint16_t patternContentMask(int scene);

private:
    Session() = default;
    void subscribe();
    core::CommandBus* installed_ = nullptr;
    std::unique_ptr<core::CommandBus> own_;
    core::Subscription changes_;
    std::uint32_t revision_ = 1, cachedRevision_ = 0;
    std::array<std::uint16_t, core::model::kScenes> contentMask_{};
};

// The text the scene.name column shows (the focused scene's name; set by the SCENE MANAGER). No allocation.
std::string_view sceneNameText() noexcept;
void setSceneNameText(std::string_view) noexcept;

// Lock value of `slot` on a cell, or `fallback` when the cell has no such lock.
float lockValue(const core::model::Pattern&, const core::model::Step&, core::model::LockSlot slot, float fallback);
} // namespace ui::m2
