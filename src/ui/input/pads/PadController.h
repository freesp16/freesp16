// The pads and pad-mode keys on the message thread (spec 02-features/pads-and-scales.md
// PAD-001…006, tracks.md TRK-041, 03-ui/control-surface.md UI-104, UI-118, UI-150…152):
//
//   mode keys  TRACK / MUTE / SLICE / SCALE are one-hot; pressing the lit key turns it off; none lit = PLAY (PAD-001)
//   PLAY       pad n plays track n and selects it          TRACK  pad n selects track n, no sound (PAD-004)
//   MUTE       pad n toggles track n's mute; SHIFT + pad n mutes every other track, the same again unmutes all — one
//              undoable CommandGroup (PAD-006), the same per-pattern mute as the MIXER toggle (UI-271): it lands in the
//              current pattern's `trackMutes`, so each pattern keeps its own (cmd::track::setMuted)
//   SLICE      pads play the selected track's slices (the engine picks the slice)  SCALE  pads play the selected track
//              at the pad's scale degree (PAD-010…013, the engine maps it)
//   SHIFT+pad  selects track n in every mode but MUTE (PAD-004), without sound
//   MUTE held  pad n toggles track n's SOLO (`track.solo`, the MIXER's); SHIFT + pad n mutes the
//              others as in MUTE mode. The pads never sound meanwhile (`pads.mode` = MUTE to the engine); when a pad
//              was used, the release restores the mode the press found, else the press / hold works as alone (PAD-007)
//
// Sound never waits for this thread (UI-118): the panel and the keyboard push each press into the engine's
// TriggerSink from their reader thread, and the engine's TriggerRouter routes it with the pad state this controller
// posts (`pads.mode`, `pads.selected`, `pads.shift` ParamChanges). The controller posts a TriggerNow itself only for
// presses of sources that did not (onPad(…, sentDirect = false): virtual panel, tests), after the velocity curve, and
// for every release (no source pushes releases; GATE tracks need them, TRK-011).
//
// Lamps (paintLamps, PAD-003 / UI-151/152): one ladder in every pad mode, PLAY included (io::padLamp), in the track
// colour (`track.padColor`): dark = muted, faint = silenced by another track's solo, dim = the others, empty ones
// included, full = the selected track (SCALE: a root note), a soloed track breathes, and a hit — a pad press at once,
// or any trigger the engine reports (observeHits: sequencer steps, MIDI, retrigger, repeat) — flashes white and eases
// back within 1 s (an empty track's pad does not, in PLAY, TRACK and MUTE). SLICE / SCALE pads flash on their own
// presses (the engine reports hits per track, not per slice or note). The mode keys white while on; MUTE held: the MUTE
// key blinks fast. At most 60 lamp frames per second.
#pragma once

#include "core/commands/Command.h"
#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "ui/input/pads/VelocityCurve.h"

#include <array>
#include <cstdint>
#include <functional>

namespace ui::input::pads
{
enum class PadMode : std::uint8_t { Play, Track, Mute, Slice, Scale }; // PAD-001 (none lit = Play); = pads.mode

// The one selected track (TRK-041: exactly one, default track 1). The shell's UI state implements it in the app;
// SimpleSelection is the stand-alone one.
class Selection
{
public:
    virtual ~Selection() = default;
    virtual int selected() const = 0; // 0–15
    virtual void select(int track) = 0;
};
class SimpleSelection final : public Selection
{
public:
    int selected() const override { return track_; }
    void select(int track) override { track_ = track & 15; }

private:
    int track_ = 0;
};
// TRK-041: the rotary selector moves the selection, wrapping 16 → 1 and 1 → 16.
constexpr int wrapTrack(int track, int detents) noexcept { return ((track + detents) % 16 + 16) % 16; }

class PadController
{
public:
    PadController(core::CommandBus&, engine::EngineBridge&, Selection&,
                  const core::ParamRegistry& params = core::builtinParams());

    void onModeKey(PadMode);    // a mode key press: one-hot, the lit key turns its mode off
    void onShift(bool held);    // SHIFT state (the engine needs it for pad hits from the reader thread)
    void onPad(int pad, int rawVelocity, bool shift, bool pressed, double nowMs, bool sentDirect = true);
    // Convenience for the input dispatcher: pad, mode-key and SHIFT events; true when the event was consumed (SHIFT
    // is observed, never consumed). Panel and keyboard presses count as sent directly (UI-118).
    bool handle(const io::ControlEvent&);
    // Posts the selection when it changed elsewhere (HOME tap, rotary): call once per UI frame. Ends SLICE mode when
    // the selected track is a MIDI / AS-1 track (the SLICE key refuses it there).
    void sync();
    void setVelocityCurve(VelocityCurve c, int fixed) noexcept
    {
        curve_ = c;
        fixed_ = fixed;
    }
    // PAD-007 / SET-016 PAD MODE SELECT RESPONSE: 0 = IMMEDIATELY (a mode key switches on press); > 0 = OVER 1sec
    // (our choice: the key must be held that long before the mode changes; a shorter press does nothing).
    void setModeSelectHoldMs(double ms) noexcept
    {
        holdMs_ = ms;
        heldMode_ = PadMode::Play;
    }
    // Once per UI frame: a mode key held for the hold time switches now.
    void tick(double nowMs);
    void paintLamps(io::LampSink&, double nowMs, const engine::MeterFeed&) const;
    // Once per UI frame: a track whose engine hit count moved since the last call flashes (the sequencer's steps, MIDI,
    // retrigger and repeat hits; a pad press already flashed at the press). The first call only takes the counts.
    void observeHits(const engine::PlayheadFeed&, double nowMs) noexcept;
    // Whether track `track` (0–15) has something to play (unset = from the model: a sample, or a non-sample mode);
    // the app answers with the shell's track summary (a sample still loading counts, a missing one does not).
    std::function<bool(int track)> hasContent;
    // The slice pad `pad` plays on `track` (−1 = none: the pad stays dark in SLICE mode, UI-152); unset =
    // the default 16 slices.
    std::function<int(int track, int pad)> slicePad;

    PadMode mode() const noexcept { return mode_; }
    // The mode the pads act in: MUTE while the MUTE key is held (solo view), else mode().
    PadMode effectiveMode() const noexcept { return muteDown_ ? PadMode::Mute : mode_; }
    bool soloHeld() const noexcept { return muteDown_; }
    bool shift() const noexcept { return shift_; }
    double lastHitMs(int track) const noexcept { return lastHit_[static_cast<std::size_t>(track & 15)]; }
    int scene() const noexcept;

private:
    void post(std::string_view id, float value);
    void postMode() { post("pads.mode", static_cast<float>(effectiveMode())); }
    void select(int track);
    void onMuteKey(bool pressed, double nowMs);
    bool selectedIsMidi() const; // the selected track is a MIDI / AS-1 track (no SLICE mode)

    core::CommandBus& bus_;
    engine::EngineBridge& bridge_;
    Selection& selection_;
    const core::ParamRegistry& params_;
    PadMode mode_ = PadMode::Play;
    bool shift_ = false;
    VelocityCurve curve_ = VelocityCurve::Linear;
    int fixed_ = 100;
    double holdMs_ = 0;                 // PAD MODE SELECT RESPONSE (0 = IMMEDIATELY)
    PadMode heldMode_ = PadMode::Play;  // the mode key held under OVER 1sec (Play = none)
    double heldSinceMs_ = -1;           // the first frame that saw it held
    bool muteDown_ = false, soloUsed_ = false; // the MUTE key is held; a pad was used meanwhile
    PadMode modeBeforeMute_ = PadMode::Play;   // the mode the MUTE press found (restored after a solo gesture)
    double muteDownMs_ = 0;
    int postedSelection_ = -1;
    std::array<double, 16> lastHit_ {};    // per track (PLAY / TRACK / MUTE lamps)
    std::array<double, 16> lastPadHit_ {}; // per pad (SLICE / SCALE lamps: all pads show the selected track)
    std::array<std::uint16_t, 16> seenHits_ {}; // the engine's hit counts at the last observeHits
    bool hitsSeen_ = false;
    mutable double lastPaintMs_ = -1e12;
};

// The engine side of the track commands (core/commands/track): every track field a ChangeSet touched as a
// ParamChange (the CommandBus bridge hook calls it; message thread).
void forwardTrackChanges(const core::ChangeSet&, const core::model::Project&, engine::EngineBridge&,
                         const core::ParamRegistry& params = core::builtinParams());
} // namespace ui::input::pads
