#include "ui/input/steps/StepController.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/edit/SharedEditor.h"
#include "core/model/Project.h"
#include "io/controllers/LampRules.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ui::input::steps
{
namespace seqcmd = core::commands::sequencer;
using core::model::Step;

namespace
{
int gShownBar = 0;
core::ParamId idOf(const core::ParamRegistry& r, std::string_view id) { return r.idOf(id).value_or(core::ParamId {}); }

const Step* cellAt(const core::model::Project& p, int scene, int pattern, int track, int step) noexcept
{
    if (step < 0 || step >= core::model::kSteps)
        return nullptr;
    return &p.scenes[static_cast<std::size_t>(scene & 15)]
                .patterns[static_cast<std::size_t>(pattern & 15)]
                .steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track & 15)];
}
} // namespace

int shownBar() noexcept { return gShownBar; }
void setShownBar(int bar0) noexcept { gShownBar = std::clamp(bar0, 0, 3); }

StepController::StepController(core::CommandBus& bus, engine::EngineBridge* bridge, const ParamAccess* values,
                               const core::ParamRegistry& params)
    : bus_(bus), bridge_(bridge), values_(values), params_(params)
{
    recordId_ = idOf(params_, "seq.record");
    eraseId_ = idOf(params_, "seq.erase");
    recordedAt_.fill(-1e12);
    moveAt_.fill(-1e12);
}

const core::model::Project* StepController::project() const
{
    return core::commands::hasProject(bus_) ? &core::commands::project(static_cast<const core::CommandBus&>(bus_))
                                            : nullptr;
}

void StepController::setContext(int scene, int pattern, int track) noexcept
{
    if (track != track_ || pattern != pattern_ || scene != scene_)
    {
        recordedAt_.fill(-1e12);
            moveAt_.fill(-1e12);
    }
    scene_ = scene & 15;
    pattern_ = pattern & 15;
    track_ = track & 15;
}

void StepController::post(core::ParamId id, std::uint16_t scope, float v)
{
    if (bridge_ != nullptr && id.valid())
        bridge_->post(engine::ParamChange {id, scope, v, 0});
}

void StepController::openGroup()
{
    if (groupOpen_ && core::commands::groupOpen(bus_))
        return;
    core::commands::beginGroup(bus_, "RECORD");
    groupOpen_ = true;
}

void StepController::closeGroup()
{
    if (! groupOpen_)
        return;
    if (core::commands::groupOpen(bus_))
        core::commands::endGroup(bus_);
    groupOpen_ = false;
}

void StepController::setTransport(bool playing, bool armed)
{
    if (armed != armed_)
        post(recordId_, 0, armed ? 1.0f : 0.0f);
    const bool wasRecording = playing_ && armed_;
    playing_ = playing;
    armed_ = armed;
    if (wasRecording && ! (playing && armed) && ! erase_)
        closeGroup(); // the pass ends: one undo step (EDT-U3)
    if (! playing && erase_)
        closeGroup();
}

void StepController::setErase(bool held)
{
    if (held == erase_)
        return;
    erase_ = held;
    post(eraseId_, static_cast<std::uint16_t>(track_), held ? 1.0f : 0.0f);
    if (! held && ! (playing_ && armed_))
        closeGroup();
}

core::edit::StepCopyGesture& StepController::gesture()
{
    auto& editor = core::edit::sharedEditor(bus_);
    if (! copyGesture_)
        copyGesture_ = std::make_unique<core::edit::StepCopyGesture>(editor);
    copyGesture_->setEditor(editor);
    copyGesture_->setTarget(scene_, pattern_, track_, shownBar());
    return *copyGesture_;
}

void StepController::setCopy(bool held)
{
    if (held)
    {
        held_ = 0; // a step held for a lock is not toggled on its release
        gesture().copyPressed();
    }
    else if (copyGesture_)
        gesture().copyReleased();
}

void StepController::setPaste(bool held)
{
    if (held)
    {
        held_ = 0;
        gesture().pastePressed();
    }
    else if (copyGesture_)
        gesture().pasteReleased();
}

void StepController::onStepKey(int step, bool pressed, double nowMs)
{
    nowMs_ = nowMs;
    if (scenesOwn_ || step < 1 || step > 16)
        return;
    if (copyGestureActive())
    {
        if (pressed)
            gesture().stepKey(step); // EDT-005: copy / paste instead of a trigger
        return;
    }
    if (pressed)
    {
        held_ = step;
        turned_ = false;
        return;
    }
    if (held_ != step)
        return;
    held_ = 0;
    if (turned_)
        return; // hold + turn made a lock: the release does not toggle (SEQ-022)
    bus_.execute(seqcmd::toggleStep(scene_, pattern_, track_, patternStep(step), entryType_, 127));
}

bool StepController::clearHeldStep()
{
    if (scenesOwn_ || held_ < 1 || held_ > 16 || project() == nullptr)
        return false;
    bus_.execute(seqcmd::clearStep(scene_, pattern_, track_, patternStep(held_)));
    turned_ = true; // the release does not toggle the step back on
    return true;
}

bool StepController::lockOf(const core::ParamSpec& spec, int step, float& value) const
{
    const int slot = seqcmd::lockSlotOf(spec, params_);
    const auto* p = project();
    if (slot < 0 || p == nullptr)
        return false;
    return seqcmd::lockOf(p->scenes[static_cast<std::size_t>(scene_)].patterns[static_cast<std::size_t>(pattern_)],
                          patternStep(step), track_, static_cast<core::model::LockSlot>(slot), value);
}

void StepController::setLock(const core::ParamSpec& spec, int step, float value)
{
    const int slot = seqcmd::lockSlotOf(spec, params_);
    if (slot < 0 || step < 1 || step > 16)
        return;
    bus_.execute(seqcmd::setLock(scene_, pattern_, track_, patternStep(step), static_cast<core::model::LockSlot>(slot),
                                 spec.clamp(value)));
    if (step == held_)
        turned_ = true;
}

bool StepController::interceptEncoder(const core::ParamSpec& spec, int detents, bool fine)
{
    if (held_ == 0 || scenesOwn_)
        return false;
    if (! spec.lockable || seqcmd::lockSlotOf(spec, params_) < 0)
        return true; // greyed while a step is held (UI-204): consumed, nothing changes
    float v = 0.0f;
    if (! lockOf(spec, held_, v))
        v = values_ != nullptr ? values_->get(spec, static_cast<std::uint16_t>(scene_ << 4 | track_)) : spec.defaultValue;
    setLock(spec, held_, spec.step(v, detents, fine));
    turned_ = true;
    return true;
}

bool StepController::recordEncoder(const core::ParamSpec& spec, float value, int playStep)
{
    if (! motionRec_ || ! (playing_ && armed_) || ! spec.lockable || playStep < 0 || playStep >= core::model::kSteps)
        return false;
    const int slot = seqcmd::lockSlotOf(spec, params_);
    if (slot < 0 || slot >= core::model::kLockSlots)
        return false;
    // The steps the playhead entered since this knob's previous move, when the knob kept turning: they get the new
    // value too (the motion is overwritten, not left with the old locks between two moves).
    const auto* p = project();
    const int length = p != nullptr ? std::clamp(p->scenes[static_cast<std::size_t>(scene_)]
                                                     .patterns[static_cast<std::size_t>(pattern_)]
                                                     .length,
                                                 1, core::model::kSteps)
                                    : core::model::kSteps;
    std::array<int, 16> steps {};
    int n = 0;
    const auto s = static_cast<std::size_t>(slot);
    if (nowMs_ - moveAt_[s] < kMotionHoldMs && moveStep_[s] != playStep)
    {
        const int gap = ((playStep - moveStep_[s]) % length + length) % length;
        if (gap > 0 && gap <= static_cast<int>(steps.size()))
            for (int k = 1; k < gap; ++k)
                steps[static_cast<std::size_t>(n++)] = (moveStep_[s] + k) % length;
    }
    steps[static_cast<std::size_t>(n++)] = playStep;
    moveAt_[s] = nowMs_;
    moveStep_[s] = playStep;
    openGroup(); // the encoder turns of a pass join its group: one pattern copy per pass (SEQ-023, EDT-U3)
    bus_.execute(seqcmd::recordLocks(scene_, pattern_, track_, std::span<const int>(steps.data(), static_cast<std::size_t>(n)),
                                     static_cast<core::model::LockSlot>(slot), spec.clamp(value)));
    return true;
}

bool StepController::recordStripLocks(int scene, int pattern, int track, std::span<const int> steps,
                                      core::model::LockSlot slot, float value)
{
    if (! (playing_ && armed_) || steps.empty())
        return false;
    openGroup();
    bus_.execute(seqcmd::recordLocks(scene, pattern, track, steps, slot, value));
    return true;
}

void StepController::onEngineEvent(const engine::EngineEvent& e)
{
    using K = engine::EngineEvent::Kind;
    if (e.kind != K::RecordedHit && e.kind != K::ErasedStep)
        return;
    if (! core::commands::hasProject(bus_))
        return;
    openGroup();
    if (e.kind == K::RecordedHit)
    {
        // A MIDI note / SCALE pad hit keeps the pitch it played at as the step's PITCH lock: the track's pitch + the
        // hit's semitones on a Sample track, the transposition of its notes on a MIDI / AS-1 track; none for the
        // track's own pitch (an older pass's lock on that step goes).
        int pitchSlot = -1;
        float pitch = std::numeric_limits<float>::quiet_NaN();
        const auto* pitchSpec = params_.find("playback.pitch");
        const auto* p = project();
        if (e.pitched && pitchSpec != nullptr && p != nullptr)
        {
            pitchSlot = seqcmd::lockSlotOf(*pitchSpec, params_);
            const auto scope = static_cast<std::uint16_t>(scene_ << 4 | (e.track & 15));
            const auto mode =
                p->scenes[static_cast<std::size_t>(scene_)].tracks[static_cast<std::size_t>(e.track & 15)].mode;
            const bool midi = mode == core::model::TrackMode::Midi || mode == core::model::TrackMode::Instrument;
            const float base = midi || values_ == nullptr ? 0.0f : values_->get(*pitchSpec, scope);
            if (e.semitones != 0)
                pitch = pitchSpec->clamp(base + static_cast<float>(e.semitones));
        }
        bus_.execute(seqcmd::recordHit(scene_, pattern_, e.track, e.step, e.value, e.offset24, pitchSlot, pitch));
        if ((e.track & 15) == track_)
            recordedAt_[static_cast<std::size_t>(e.step & 63)] = nowMs_;
    }
    else
        bus_.execute(seqcmd::clearStep(scene_, pattern_, e.track, e.step));
}

void StepController::paintStepLamps(io::LampSink& sink, const engine::PlayheadFeed& ph) const
{
    if (scenesOwn_)
        return;
    const auto* p = project();
    const int bar = shownBar();
    const int colourIndex =
        p != nullptr ? p->scenes[static_cast<std::size_t>(scene_)].tracks[static_cast<std::size_t>(track_)].colour : track_;
    const std::uint32_t colour = theme::trackColour((colourIndex & 15) + 1) & 0xFFFFFFu;
    const bool here = ph.playing && ph.scene == scene_ && ph.pattern == pattern_;
    for (int n = 1; n <= 16; ++n)
    {
        const int step = bar * 16 + n - 1;
        std::uint32_t rgb = colour;
        std::uint8_t level = 0;
        const Step* c = p != nullptr ? cellAt(*p, scene_, pattern_, track_, step) : nullptr;
        if (c != nullptr && c->trigger != Step::kEmpty)
        {
            level = c->triggerType == core::model::TriggerType::Full   ? 255
                    : c->triggerType == core::model::TriggerType::Half ? io::kDimLevel
                                                                       : 40; // PARAM: low (no outline on a lamp)
            if (c->lockIndex != Step::kNoLocks) // a locked step: a lighter tint of the track colour
                rgb = theme::lockedTint(colour);
        }
        if (nowMs_ - recordedAt_[static_cast<std::size_t>(step)] < io::kFlashMs)
        {
            const bool on = std::fmod(nowMs_ - recordedAt_[static_cast<std::size_t>(step)], 250.0) < 125.0;
            rgb = on ? io::lampcolour::white : colour;
            level = 255;
        }
        if (here && ph.step == step)
        {
            rgb = io::lampcolour::white; // the playing step (SEQ-013)
            level = 255;
        }
        if (held_ == n)
        {
            rgb = io::lampcolour::white;
            level = 255;
        }
        sink.setLamp(io::step(n), rgb, level);
    }
}

bool StepLockAccess::lockOf(const core::ParamSpec& s, std::uint16_t scope, int step, float& value) const
{
    if (seqcmd::lockSlotOf(s) < 0 || (scope & 15) != steps_.track())
        return inner_.lockOf(s, scope, step, value);
    return steps_.lockOf(s, step, value);
}

void StepLockAccess::setLock(const core::ParamSpec& s, std::uint16_t scope, int step, float value)
{
    if (seqcmd::lockSlotOf(s) < 0 || (scope & 15) != steps_.track())
    {
        inner_.setLock(s, scope, step, value);
        return;
    }
    steps_.setLock(s, step, value);
}
} // namespace ui::input::steps
