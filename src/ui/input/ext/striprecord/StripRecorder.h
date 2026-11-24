// Touch strip moves recorded into the sequence as motion (RIB-008, SEQ-023). On the reference the strip acts only
// while a pad is held and is neither recorded nor applied to sequenced triggers (M p.15); Sixteen records the moves
// like a knob's: while REC is armed and the transport runs, on every track the strip acts on whose MOTION RECORD
// (track.motionRec) is ON — or on every such track when the extension switch `ext.strip.record` is on (E12) — as
// p-locks of the steps the playhead plays, so they show in TRACK MENU › MOTION's lanes:
//   PITCH          playback.pitch locks: the track's own pitch + the strip's bend (the live strip is an offset on
//                  the stored pitch; a lock replaces it, so the bend is added here)
//   USER1 / USER2  each item's target locked at the item's value (the live strip is an absolute Override)
//   REPEAT         no parameter move: its hits are recorded as notes by the sequencer (RIB-003)
// The value recorded is the one the strip applies: touched, or kept by HOLD after the release (RIB-005).
//
// Where it lives: message-thread work like the knobs' motion recording (StepController::recordEncoder), so a ui/input
// class (the engine does not see the model's ribbon items): one frame() per UI frame turns the strip's state and the
// playhead into lock writes; the app executes them in the recording pass's undo group (StepController::
// recordStripLocks). Targets that are not lockable track parameters (FX-094: Send / Master FX; preferences) are
// skipped. A lock is written once per (track, step, slot) and value: holding the finger still costs nothing; the
// first frame after a new touch always writes, and the steps a slow frame skipped get the value too.
//
// StripHeld follows which tracks the strip acts on from the pad presses, like the engine's RibbonEngine::observe: a
// held pad's track (PLAY: pad n; SLICE / SCALE: the selected track; TRACK / MUTE / SHIFT + pad make no sound), kept
// under HOLD after the pad's release until HOLD goes off; ribbon.applyTo SELECTED / ALL replaces the held pads.
//
// Message thread; no allocation beyond `out`.
#pragma once

#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ui::input::ext
{
struct StripRecordState
{
    bool recording = false;        // REC armed and the transport running
    int scene = 0, pattern = 0;    // where the playhead plays
    int step = -1;                 // the playing step 0–63 (−1 = none)
    int length = 64;               // the playing pattern's length in steps (the skipped-step fill wraps in it)
    int mode = 0;                  // core::ribbon::Mode: 0 PITCH, 1 REPEAT, 2 USER1, 3 USER2
    bool contact = false;          // the strip applies a value: touched, or kept by HOLD
    int position = 0;              // 0 (bottom) … 127 (top)
    std::uint16_t tracks = 0;      // bit t = the strip acts on track t
    std::uint16_t record = 0xFFFF; // bit t = track t records (its MOTION RECORD, or the extension switch)
    std::array<float, 16> pitch {}; // each track's stored playback.pitch (semitones): PITCH locks are this + bend
};

struct LockWrite
{
    int scene = 0, pattern = 0, track = 0, step = 0;
    core::model::LockSlot slot = 0;
    float value = 0; // the slot's parameter units
    const core::ParamSpec* spec = nullptr;
    friend bool operator== (const LockWrite&, const LockWrite&) = default;
};

class StripHeld
{
public:
    // A pad press / release. padMode = ui::input::pads::PadMode as int (0 PLAY, 1 TRACK, 2 MUTE, 3 SLICE, 4 SCALE,
    // the effective one: MUTE while the MUTE key is held); shift = SHIFT held (the pad only selects).
    void onPad(int pad, bool pressed, int padMode, bool shift, int selected) noexcept;
    // The tracks the strip acts on: applyTo 0 HELD PADS (the reference), 1 SELECTED TRACK, 2 ALL TRACKS. Call once per
    // frame: under HOLD with a value shown, the held pads' tracks stay (`holding`); HOLD off forgets them.
    std::uint16_t tracks(int applyTo, int selected, bool holding) noexcept;
    void reset() noexcept { *this = {}; }

private:
    std::array<std::int8_t, 16> padTrack_ = filled(); // the track each held pad plays, −1 = not held / silent
    std::uint16_t sticky_ = 0;
    static constexpr std::array<std::int8_t, 16> filled() noexcept
    {
        std::array<std::int8_t, 16> a {};
        a.fill(-1);
        return a;
    }
};

class StripRecorder
{
public:
    explicit StripRecorder(const core::ParamRegistry& registry = core::builtinParams());

    void frame(const StripRecordState&, const core::model::RibbonSettings&, std::vector<LockWrite>& out);
    void reset() noexcept;

private:
    struct Last
    {
        int step = -1;
        float value = 0;
    };
    void emit(const StripRecordState& s, const core::ParamSpec& spec, float value, bool addPitch,
              std::vector<LockWrite>& out);

    const core::ParamRegistry& registry_;
    const core::ParamSpec* pitch_ = nullptr;
    std::array<std::array<Last, core::model::kLockSlots>, core::model::kTracks> last_ {};
    bool touching_ = false;
};
} // namespace ui::input::ext
