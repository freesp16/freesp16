// The panel's PATTERN mode, two-stage SCENE mode and pattern-length mode (spec 02-features/
// scenes-and-patterns.md SCN-011/020…027, 03-ui/control-surface.md §1.1b, UI-154/155/161).
//
//   PATTERN            toggles PATTERN mode (PATTERN white): the step keys show the current scene's 16 patterns in the
//                      scene colour (content dim, empty off, current full, pending blinking 2 Hz, SCN-022); a key
//                      selects that pattern — queued to the PATTERN QUANTIZE boundary while playing (SCN-020), the
//                      current one again cancels (SCN-021), empty ones too (SCN-026).
//   SCENE              toggles SCENE mode (SCENE white): the keys show the 16 scenes (same rule); a scene key shows that
//                      scene's patterns, a pattern key commits scene + pattern (the keys keep showing that scene);
//                      leaving without a pattern key restores the previous scene (nothing was sent, SCN-027).
//   SHIFT + PATTERN    pattern-length mode (PATTERN white, step keys white, BAR keys 1…n blue): BAR key n = n bars;
//                      step key k = the length (bars − 1) × 16 + k in the last bar of the current length, 8–64
//                      (SCN-011); PATTERN leaves.
//   chains (SCN-030) in PATTERN mode, with a chain sink installed: hold the current pattern's key and
//                      press others → on its release the chain [current, pressed…] goes to the sink (the engine loops it,
//                      advancing exactly at pattern ends, SCN-031); the current key pressed and released alone cancels a
//                      pending switch as before; any plain selection clears a chain this posted.
// While a mode is on ownsStepKeys() is true: the sequencer's step handler and lamp painter do nothing.
//
// Scene BPM policy ASK (SCN-005): a switch to another scene that has a BPM first calls askBpm (the shell's popup); the
// answer is sent with the request (kQueueApplyBpm). Message thread only; the key handlers allocate only for the commands
// they execute, the lamp painters never.
#pragma once

#include "core/commands/Command.h"
#include "core/edit/PatternChainBuilder.h"
#include "engine/EngineApi.h"
#include "io/controllers/LampRules.h"

#include <array>
#include <cstdint>
#include <functional>

namespace core::model
{
struct Project;
}

namespace ui::input::scenes
{
enum class KeyMode : std::uint8_t { Off, Pattern, Scene, ScenePatterns, Length };
enum class BpmPolicy : std::uint8_t { Apply, Ask, Ignore }; // scene.bpmPolicy

class ScenePatternKeys
{
public:
    // apply(true) = also set the scene's BPM at this switch.
    using AskBpm = std::function<void(int scene, double bpm, std::function<void(bool apply)> answer)>;

    explicit ScenePatternKeys(core::CommandBus&);
    ~ScenePatternKeys();
    ScenePatternKeys(const ScenePatternKeys&) = delete;
    ScenePatternKeys& operator=(const ScenePatternKeys&) = delete;

    void onScene();
    void onPattern(bool shift);
    void onStepKey(int key); // 1–16
    void onBarKey(int bar);  // 1–4
    // Press events of SCENE / PATTERN (always), step keys (while a mode is on) and BAR keys (in length mode); true =
    // consumed. `shift` = SHIFT held.
    bool handle(const io::ControlEvent&, bool shift);
    bool ownsStepKeys() const noexcept { return mode_ != KeyMode::Off; }
    KeyMode mode() const noexcept { return mode_; }

    // The engine's playhead (current and pending scene/pattern), once per frame; before the first engine block the
    // model's settings.currentScene/currentPattern count as current.
    void observe(const engine::PlayheadFeed&) noexcept;
    // SwitchDone (EngineEvent value = scene × 16 + pattern) → the model's current scene/pattern (not undoable).
    void onEngineEvent(const engine::EngineEvent&);

    void setBpmPolicy(BpmPolicy p) noexcept { policy_ = p; }
    void setAskBpm(AskBpm ask) { askBpm_ = std::move(ask); }
    // SCN-030: where built chains go (the composition root posts them to the engine); none = no chain gesture.
    using ChainSink = std::function<void(const core::chain::ChainPlan&)>;
    void setChainSink(ChainSink sink) { chainSink_ = std::move(sink); }
    bool chainHeld() const noexcept { return chain_.holding(); }

    // 0-based current scene / pattern and the scene the keys show.
    int currentScene() const noexcept;
    int currentPattern() const noexcept;
    int shownScene() const noexcept { return mode_ == KeyMode::ScenePatterns ? chosenScene_ : currentScene(); }

    // Refines a lamp frame (ui::LampModel / LampSource): SCENE, PATTERN, the step keys and, in length mode, the BAR
    // keys. Off: leaves the frame alone. No allocation.
    void lamps(io::LampFrame&, const engine::PlayheadFeed&, double nowMs) const noexcept;
    // The same lamps resolved straight into a sink (UI-150).
    void paintLamps(io::LampSink&, const engine::PlayheadFeed&, double nowMs) const;

private:
    void select(int scene, int pattern);
    void onStepRelease(int key); // 1–16
    void clearChain();
    void setLength(int steps);
    const core::model::Project* project() const noexcept;
    void refreshContent() const noexcept;

    core::CommandBus& bus_;
    core::Subscription changes_;
    KeyMode mode_ = KeyMode::Off;
    int chosenScene_ = 0;
    BpmPolicy policy_ = BpmPolicy::Apply;
    AskBpm askBpm_;
    ChainSink chainSink_;
    core::edit::PatternChainBuilder chain_;
    int chainKey_ = 0;          // the held current-pattern key (1–16), 0 = none
    bool chainPosted_ = false;  // a chain this posted may be playing
    engine::PlayheadFeed feed_ {};
    mutable bool contentDirty_ = true;
    mutable std::array<std::uint16_t, 16> patternContent_ {}; // bit p = pattern p has a trigger
    mutable std::uint16_t sceneContent_ = 0;                  // bit s = scene s has content
};
} // namespace ui::input::scenes
