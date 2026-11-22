// E12 — record touch-strip moves into the sequence (RIB-008, P3 [MAN]; flag
// `ext.strip.record`, off by default). On the reference the strip acts only while a pad is held and is neither
// recorded nor applied to sequenced triggers (M p.15); this Sixteen option records the moves made while recording as
// p-locks on the steps they occur on: USER1 / USER2 items as locks of their target parameters, PITCH as pitch locks
// (playback.pitch, the ribbon's PITCH range in semitones). REPEAT moves are not recorded (its hits are repeats, not
// parameter moves).
//
// Where it lives: the recording is message-thread work like SEQ-023's encoder recording (StepController::
// recordEncoder: a lock on the playing step while REC is armed and the transport runs), so this is a ui/input
// class rather than an engine/strip/ext/record/ one (the engine may not see the model's ribbon items):
// one frame() per UI frame turns the strip's live state (ui::m4::StripLive) and the playhead into lock writes; the
// glue (app/ext) executes them as SetLock commands. Targets that are not lockable track parameters (FX-094: Send /
// Master FX; preferences) are skipped. A lock is written once per (track, step, slot) and value: holding the finger
// still costs nothing; the first frame after a new touch always writes.
//
// Message thread; no allocation beyond `out`. The TOUCH STRIP SETTING rail item 2 `RECORD MOVES` (utility.md
// "Extension points") is not added: that rail is a fixed list of the screen (no registration point) — the feature
// flag is the switch until the screen offers the slot.
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
    int mode = 0;                  // core::ribbon::Mode: 0 PITCH, 1 REPEAT, 2 USER1, 3 USER2
    bool contact = false;
    int position = 0;              // 0 (bottom) … 127 (top)
    std::uint16_t tracks = 0;      // bit t = the strip acts on track t (ribbon.applyTo)
};

struct LockWrite
{
    int scene = 0, pattern = 0, track = 0, step = 0;
    core::model::LockSlot slot = 0;
    float value = 0; // the slot's parameter units
    const core::ParamSpec* spec = nullptr;
    friend bool operator== (const LockWrite&, const LockWrite&) = default;
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
    void emit(const StripRecordState& s, const core::ParamSpec& spec, float value, std::vector<LockWrite>& out);

    const core::ParamRegistry& registry_;
    const core::ParamSpec* pitch_ = nullptr;
    std::array<std::array<Last, core::model::kLockSlots>, core::model::kTracks> last_ {};
    bool touching_ = false;
};
} // namespace ui::input::ext
