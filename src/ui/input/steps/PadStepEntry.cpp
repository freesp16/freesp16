#include "ui/input/steps/PadStepEntry.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/Project.h"
#include "ui/input/steps/StepController.h"

#include <algorithm>
#include <cmath>

namespace ui::input::steps
{
namespace
{
namespace seqcmd = core::commands::sequencer;
using core::model::Step;

const core::model::Pattern* patternOf(core::CommandBus& bus, const StepController& steps) noexcept
{
    if (! core::commands::hasProject(bus))
        return nullptr;
    return &core::commands::project(bus)
                .scenes[static_cast<std::size_t>(steps.scene() & 15)]
                .patterns[static_cast<std::size_t>(steps.pattern() & 15)];
}
const Step& cellOf(const core::model::Pattern& p, int step, int track) noexcept
{
    return p.steps[static_cast<std::size_t>(step & 63)][static_cast<std::size_t>(track & 15)];
}
} // namespace

float clampPitchLock(const core::ParamSpec& pitch, core::model::TrackMode m, float semitones) noexcept
{
    return isMidiMode(m) ? std::clamp(semitones, -kMidiPitchLockRange, kMidiPitchLockRange) : pitch.clamp(semitones);
}

void scaleStepEntry(core::CommandBus& bus, const StepController& steps, int track, float pitchLock, int step1to16)
{
    const auto* p = patternOf(bus, steps);
    if (p == nullptr || std::isnan(pitchLock) || step1to16 < 1 || step1to16 > 16)
        return; // the root key (or none selected yet): the plain trigger is the key
    const int step = steps.patternStep(step1to16);
    float current = 0.0f;
    if (cellOf(*p, step, track).trigger == Step::kEmpty ||
        (seqcmd::lockOf(*p, step, track, kPitchLockSlot, current) && current == pitchLock))
        return;
    bus.execute(seqcmd::setLock(steps.scene(), steps.pattern(), track, step, kPitchLockSlot, pitchLock));
}

void scaleStepHeld(core::CommandBus& bus, StepController& steps, int track, float pitchLock, int velocity,
                   int step1to16)
{
    const auto* p = patternOf(bus, steps);
    if (p == nullptr || step1to16 < 1 || step1to16 > 16)
        return;
    const int sc = steps.scene(), pat = steps.pattern(), step = steps.patternStep(step1to16);
    if (cellOf(*p, step, track).trigger == Step::kEmpty) // an empty step gets a trigger: the key programs a note there
        bus.execute(seqcmd::toggleStep(sc, pat, track, step, steps.entryType(), std::clamp(velocity, 1, 127)));
    float current = 0.0f;
    const bool locked = seqcmd::lockOf(*patternOf(bus, steps), step, track, kPitchLockSlot, current);
    if (std::isnan(pitchLock))
    {
        if (locked) // the root key: the step plays the track as it is, as step entry leaves it
            bus.execute(seqcmd::removeLock(sc, pat, track, step, kPitchLockSlot));
    }
    else if (! locked || current != pitchLock)
        bus.execute(seqcmd::setLock(sc, pat, track, step, kPitchLockSlot, pitchLock));
    steps.keepHeldStep(step1to16); // the held key's release toggles nothing
}

void sliceStepEntry(core::CommandBus& bus, const StepController& steps, int track, int slice, int step1to16)
{
    const auto* p = patternOf(bus, steps);
    if (p == nullptr || slice < 0 || step1to16 < 1 || step1to16 > 16)
        return;
    if (isMidiMode(core::commands::project(bus)
                       .scenes[static_cast<std::size_t>(steps.scene() & 15)]
                       .tracks[static_cast<std::size_t>(track & 15)]
                       .mode))
        return; // "SLICE mode cannot be used for MIDI tracks" (manual p.49)
    const int step = steps.patternStep(step1to16);
    float current = -1.0f;
    if (cellOf(*p, step, track).trigger == Step::kEmpty ||
        (seqcmd::lockOf(*p, step, track, kSliceLockSlot, current) && current == static_cast<float>(slice)))
        return;
    bus.execute(
        seqcmd::setLock(steps.scene(), steps.pattern(), track, step, kSliceLockSlot, static_cast<float>(slice)));
}
} // namespace ui::input::steps
