// The sequencer's message-thread half (spec 02-features/sequencer.md SEQ-012/022/040/
// 041/043/046, 01-architecture/state-and-undo.md EDT-U1…U3): undoable step-entry, lock and record commands over
// scene[s].pattern[p] of the document, and the pattern image the engine's pattern objects are built from.
//
// Each command applies a small edit to one pattern; its inverse restores that pattern exactly as it was (a copy of the
// one pattern, ≈ 9 KB + its locks — never a project snapshot), so undo is bit-identical. Edits are heard on the next
// pass of the step (SEQ-061): the bridge hook (ui::input::steps::forwardPatternChanges) rebuilds the one PatternData
// the path "scene[s].pattern[p]" names and swaps it in. Coalescing (EDT-U2): lock edits of one cell and slot within
// the UndoStack's window are one undo step (hold + turn), and inside a recording group (CommandBus beginGroup, EDT-U3)
// every recordHit / clearStep of a pattern keeps a single inverse — one recording pass = one undo step.
//
// Cells: a trigger is stored as the reference codes FULL 15 / HALF 11 / PARAM 8 (SEQ-011, OQ-09) with velocity
// 1–127 in Step::value2; locks in Pattern::locks, the cell's lockIndex pointing at them (sparse; unreferenced or empty
// entries are dropped after every edit, the order of the others is kept). Indices are 0-based; out-of-range steps or
// slots are ignored (the command still applies, as a no-op).
#pragma once

#include "core/commands/Command.h"
#include "core/model/Pattern.h"
#include "core/params/ParamRegistry.h"
#include "core/util/sequencer/PatternImage.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace core::commands::sequencer
{
namespace model = ::core::model;

// SEQ-040: empty → a trigger of `type` with `velocity`; a trigger (or a lock-only cell) → empty, with its locks.
std::unique_ptr<core::Command> toggleStep(int scene, int pattern, int track, int step, model::TriggerType, int velocity);
// SEQ-012: the step's trigger and all its locks.
std::unique_ptr<core::Command> clearStep(int scene, int pattern, int track, int step);
// SEQ-022: one lock (value in the slot's ParamSpec units). Locking an empty step makes it a PARAM step (it
// would never play otherwise).
std::unique_ptr<core::Command> setLock(int scene, int pattern, int track, int step, model::LockSlot, float value);
std::unique_ptr<core::Command> removeLock(int scene, int pattern, int track, int step, model::LockSlot);
// MOTION REC (SEQ-023): `value` on each of `steps` in one slot (an empty step becomes a PARAM step),
// overwriting the motion there; merges with the pass's other recorded edits (one pattern copy per pass).
std::unique_ptr<core::Command> recordLocks(int scene, int pattern, int track, std::span<const int> steps,
                                           model::LockSlot, float value);
// SEQ-041/046: overdub a recorded hit — a FULL trigger with `velocity`; the micro-timing as the OFF SET lock (slot 15,
// removed for offset 0). A pitched hit (a MIDI note, a SCALE pad) also sets its pitch: `pitch` in slot `pitchSlot`
// (PITCH, semitones), NaN removes that lock (the hit played at the track's own pitch); pitchSlot < 0 = unpitched.
std::unique_ptr<core::Command> recordHit(int scene, int pattern, int track, int step, int velocity, int offset24,
                                         int pitchSlot = -1, float pitch = 0.0f);
// SEQUENCE CLEAR TRIGGERS (UID-758): the triggers and locks of `steps`.
std::unique_ptr<core::Command> clearTriggers(int scene, int pattern, int track, std::span<const int> steps);
// SEQ-043: moves the OFF SET locks of `steps` toward 0 by `strength` 0–1 (1 = onto the grid; 0 removes the lock).
std::unique_ptr<core::Command> quantizeOffsets(int scene, int pattern, int track, std::span<const int> steps,
                                               float strength);
// The SEQUENCE strip on target steps (UID-755): values[i] on steps[i]; a value equal to `removeAt` removes the lock
// (OFF SET 0, RETRIGGER CNT 1, …). Consecutive turns of one slot coalesce.
std::unique_ptr<core::Command> setLocks(int scene, int pattern, int track, std::span<const int> steps,
                                        model::LockSlot, std::span<const float> values, float removeAt,
                                        std::string_view label);
// MOTION (TRACK MENU › MOTION): the motion of one slot on `steps` — values[i] on steps[i], NaN removes the lock there.
// A lock on an empty step makes it a PARAM step; a PARAM step left without locks becomes empty again. Consecutive
// edits of one track and slot coalesce (a finger drawing a lane is one undo step).
std::unique_ptr<core::Command> editMotion(int scene, int pattern, int track, std::span<const int> steps,
                                          model::LockSlot, std::span<const float> values);
// MOTION CLEAR: every lock of the slots in `slotMask` (bit s = slot s) on every step of the track; PARAM steps left
// without locks become empty.
std::unique_ptr<core::Command> clearMotion(int scene, int pattern, int track, std::uint64_t slotMask);
// SEQ-036 step shift: rotates the cells (with their locks) of `track` by `detents` inside [first, first + count).
std::unique_ptr<core::Command> shiftSteps(int scene, int pattern, int track, int first, int count, int detents);

// ---- helpers (message thread) ----
// The model path a pattern edit reports ("scene[3].pattern[7]") and its parser (prefix match: "scene[3].pattern[7]…"
// also parses; false for any other path).
std::string patternPath(int scene, int pattern);
bool parsePatternPath(std::string_view path, int& scene, int& pattern) noexcept;
// The lock slot of a lockable ParamSpec (sequencer.md "Lockable parameters", OQ-01: the app's own ParamIndex order,
// keyed by id; FX1 / FX2 knobs by their internal index k, OQ-07), −1 when it has none.
int lockSlotOf(const core::ParamSpec&, const core::ParamRegistry& = core::builtinParams());
// A cell's lock of `slot`, false when it has none.
bool lockOf(const model::Pattern&, int step, int track, model::LockSlot, float& value) noexcept;
// The engine's view of a pattern (core/util/sequencer/PatternImage.h): velocities, kinds, the OFF SET / RETRIGGER
// locks per cell, every lock set.
void makePatternImage(const model::Pattern&, ::core::sequencer::PatternImage& out);
} // namespace core::commands::sequencer
