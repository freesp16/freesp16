// The step keys on the message thread (step entry and lock editing, real-time recording; spec 02-features/sequencer.md
// SEQ-013/022/023/040/041/046, 03-ui/control-surface.md UI-101/153, state-and-undo.md EDT-U3):
//
//   step entry   a short press of step key n toggles a trigger of the selected track on step (bar × 16 + n) of the
//                current pattern, with `seq.triggerType` and velocity 127 (SEQ-040); holding the key and turning a
//                lockable parameter writes a lock on that step instead (SEQ-022), and its release does not toggle
//   recording    REC armed → `seq.record` to the engine; the engine's RecordedHit / ErasedStep events become recordHit /
//                clearStep commands in one CommandGroup per recording pass (EDT-U3: one undo step), closed on stop or
//                when REC goes off; encoder turns on a playing track while recording write a lock on the playing step,
//                in the same group, coalesced per step (SEQ-023) — only while the track's MOTION REC is ON
//                (setMotionRec): every step the playhead enters while the knob keeps turning (moves less than
//                kMotionHoldMs apart) gets the new value, overwriting the motion there
//   ERASE        held → `seq.erase` for the selected track (SEQ-046)
//   COPY / PASTE ext.copy / ext.paste held (EDT-005): hold COPY + step keys → those steps of the selected
//                track are copied on COPY's release (no key: the shown bar); hold PASTE + a step key → pasted from that
//                step, wrapping at the pattern length (core::edit::StepCopyGesture on the session's SceneEditor);
//                the step keys enter nothing meanwhile
//   lamps        the shown bar of the selected track: FULL in the track colour, HALF dim, PARAM low, the playing step
//                white, steps recorded in the last second flash (SEQ-013, UI-150/151). The recorded motion is shown
//                on TRACK MENU › MOTION (ui/screens/motion), not on the step keys
//   scene keys   while ScenePatternKeys::ownsStepKeys() is true (setStepKeysOwned), the step handler and
//                the lamp painter do nothing
//
// StepLockAccess is the ParamAccess decorator that gives the shell's hold + turn (UID-166, Shell::turnEncoder) and the
// knob strip's lock display the pattern model: lockOf / setLock on step 1–16 of the shown bar go to the selected
// pattern's cell through the controller; get / set pass through. The composition root wraps its ModelParamAccess with
// it. Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "core/edit/StepCopyGesture.h"
#include "core/model/enums/Enums.h"
#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "ui/shell/ParamAccess.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace core::model
{
struct Project;
}

namespace ui::input::steps
{
// The bar the step keys show (0–3; the bar selection keys and the SEQUENCE screen's BAR, SEQ-002). process-wide
// until UiState carries it.
int shownBar() noexcept;
void setShownBar(int bar0) noexcept;

class StepController
{
public:
    // `values` (may be null) gives the track's base value when a lock starts on an unlocked step; `bridge` (may be
    // null) receives seq.record / seq.erase.
    StepController(core::CommandBus&, engine::EngineBridge* bridge, const ParamAccess* values = nullptr,
                   const core::ParamRegistry& = core::builtinParams());

    // ---- context (per UI frame or on change; 0-based) ----
    void setContext(int scene, int pattern, int track) noexcept;
    void setEntryType(core::model::TriggerType t) noexcept { entryType_ = t; }
    void setStepKeysOwned(bool sceneKeysOwnThem) noexcept { scenesOwn_ = sceneKeysOwnThem; }
    void setMotionRec(bool on) noexcept { motionRec_ = on; } // the selected track's track.motionRec
    void tick(double nowMs) noexcept { nowMs_ = nowMs; }
    // Transport state: REC armed and playing. Posts seq.record on a change; ends the recording group on stop / disarm.
    void setTransport(bool playing, bool armed);
    void setErase(bool held); // ERASE key (UI-102)
    void setCopy(bool held);  // ext.copy (EDT-005)
    void setPaste(bool held); // ext.paste (EDT-005)
    bool copyGestureActive() const noexcept { return copyGesture_ && copyGesture_->active(); }

    // ---- input ----
    // Step 1–16 of the shown bar. True when this release entered a trigger (toggled one on): not for a press, a
    // release that removed one, a hold + turn or held-step edit (keepHeldStep), or a COPY / PASTE gesture.
    bool onStepKey(int step, bool pressed, double nowMs);
    // ext.clear with a step held (UI-120: "Delete (with a step held: clear that step)", SEQ-012): the held
    // step's trigger and locks on the selected track go, one undo step; its release then toggles nothing. False when
    // no step is held (or the scene keys own the step keys).
    bool clearHeldStep();
    bool interceptEncoder(const core::ParamSpec&, int detents, bool fine); // true when a step is held → setLock
    // SEQ-023: an encoder value change on a lockable parameter of the selected track while recording and playing, with
    // the track's MOTION REC ON → a lock on the playing step (`playStep` 0–63, PlayheadFeed::step), and on the steps
    // the playhead passed since the previous move of the same knob when it was under kMotionHoldMs ago. True when it
    // was recorded.
    static constexpr double kMotionHoldMs = 300.0; // our choice
    bool recordEncoder(const core::ParamSpec&, float value, int playStep);
    // The touch strip's recorded moves (ui/input/ext/striprecord, RIB-008): `value` locked on `steps` of `track` in
    // pattern (scene, pattern), in the recording pass's group (one undo step with the knobs' motion and the hits).
    // False (nothing written) unless recording and playing.
    bool recordStripLocks(int scene, int pattern, int track, std::span<const int> steps, core::model::LockSlot slot,
                          float value);
    void onEngineEvent(const engine::EngineEvent&); // recorded hits / erased steps → commands in the open group
    void paintStepLamps(io::LampSink&, const engine::PlayheadFeed&) const;

    // ---- locks of the selected track's current pattern (StepLockAccess) ----
    bool lockOf(const core::ParamSpec&, int step, float& value) const; // step 1–16 of the shown bar
    void setLock(const core::ParamSpec&, int step, float value);

    int heldStep() const noexcept { return held_; } // 1–16, 0 = none
    // Step `step1to16` (when it is the held one) was edited while held, as by hold + turn: its release toggles nothing.
    void keepHeldStep(int step1to16) noexcept
    {
        if (step1to16 == held_ && held_ != 0)
            turned_ = true;
    }
    core::model::TriggerType entryType() const noexcept { return entryType_; }
    int patternStep(int step1to16) const noexcept { return shownBar() * 16 + step1to16 - 1; }
    bool groupOpen() const noexcept { return groupOpen_; }
    int scene() const noexcept { return scene_; }
    int pattern() const noexcept { return pattern_; }
    int track() const noexcept { return track_; }

private:
    void post(core::ParamId id, std::uint16_t scope, float v);
    void openGroup();
    void closeGroup();
    const core::model::Project* project() const;
    core::edit::StepCopyGesture& gesture(); // bound to the shared editor, targeting the current context

    core::CommandBus& bus_;
    engine::EngineBridge* bridge_;
    const ParamAccess* values_;
    const core::ParamRegistry& params_;
    core::ParamId recordId_ {}, eraseId_ {};
    int scene_ = 0, pattern_ = 0, track_ = 0;
    core::model::TriggerType entryType_ = core::model::TriggerType::Full;
    bool scenesOwn_ = false, playing_ = false, armed_ = false, erase_ = false, groupOpen_ = false;
    bool motionRec_ = false;
    int held_ = 0;
    bool turned_ = false;
    double nowMs_ = 0;
    std::unique_ptr<core::edit::StepCopyGesture> copyGesture_;
    std::array<double, 64> recordedAt_ {}; // per pattern step of the selected track: last recorded hit (flash)
    std::array<double, core::model::kLockSlots> moveAt_ {}; // per lock slot: the last recorded knob move
    std::array<int, core::model::kLockSlots> moveStep_ {};  // per lock slot: the step it wrote
};

class StepLockAccess final : public ParamAccess
{
public:
    StepLockAccess(ParamAccess& inner, StepController& steps) : inner_(inner), steps_(steps) {}
    float get(const core::ParamSpec& s, std::uint16_t scope) const override { return inner_.get(s, scope); }
    void set(const core::ParamSpec& s, std::uint16_t scope, float v) override { inner_.set(s, scope, v); }
    bool lockOf(const core::ParamSpec&, std::uint16_t scope, int step, float& value) const override;
    void setLock(const core::ParamSpec&, std::uint16_t scope, int step, float value) override;

private:
    ParamAccess& inner_;
    StepController& steps_;
};
} // namespace ui::input::steps
