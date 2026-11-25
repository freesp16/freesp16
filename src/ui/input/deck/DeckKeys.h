// The DJ section of the DJS-1000's panel: controls the SP-16 panel does not have (io::Control PadModeHotSlice …
// FxSelect). No other input sends them, so on the SP-16 this does nothing.
//
//   TEMPO fader   the running tempo × (1 + range × position), as a deck's pitch fader: −range at its minus end, the
//                 tempo itself in its centre (a dead zone). It moves the engine's tempo only (TransportCmd SetTempo):
//                 the project's tempo is not edited, so a move is no undo step and the project keeps its BPM. The base
//                 is the engine's tempo while the fader is centred, and any tempo change made by someone else (a
//                 scene's or a song row's BPM, the BPM screen, TAP, a project open) becomes the new base.
//   TEMPO RANGE   ±6, ±10, ±16 and ±50 % in turn (our choice: the DJS-1000's own ranges are not known)
//   TEMPO RESET   toggles: while lit (its lamp) the fader is ignored and the base tempo plays
//   SYNC          follows an external tempo: midi.syncSource MIDI CLOCK (with midi.in.clock), lit while it follows.
//                 A live switch: not saved, UTILITY's SYNC SOURCE stays the setting at start. The fader does nothing
//                 while it follows.
//   MASTER        leads the external tempo: midi.out.clock (the MIDI clock sent), lit while it is sent
//   FX ON         the MASTER FX on or off (its bypass), lit while an effect plays. Without an effect it opens the
//                 MASTER/SEND FX page, as FX does
//   FX            the MASTER/SEND FX page (mixer.busFx)
//   FX LEVEL      the MASTER FX's MIX (its `mix` parameter, 0–1023 over its whole range)
//   LIVE SAMPLING the LIVE SAMPLING screen (as SHIFT + REC), lit while it is open
//   HOT SLICE     not assigned yet: the DJS-1000's SLICE key already selects the slice pad mode
//
// Message thread. The shell and the engine are reached through Hooks, so tests can stand in for them.
#pragma once

#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "io/controllers/LampRules.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace ui
{
class ParamAccess;
namespace fx
{
class FxSlotAccess;
}
} // namespace ui

namespace ui::input::deck
{
class DeckKeys
{
public:
    struct Hooks
    {
        std::function<void(double bpm)> setTempo;      // the engine's tempo only (TransportCmd SetTempo)
        std::function<void(std::string_view id)> open; // a screen or popup (ShellApi::open)
        std::function<int()> scene;                    // the current scene, 1–16
    };

    static constexpr int kRanges[] = {6, 10, 16, 50}; // TEMPO RANGE, ± percent
    static constexpr int kFaderCentre = 512, kFaderDeadZone = 8, kFaderHalf = 511;
    // Blocks the engine needs to show a SetTempo in its feed (as TransportController::kSettleBlocks).
    static constexpr std::uint64_t kSettleBlocks = 32;

    DeckKeys(ParamAccess&, fx::FxSlotAccess&, Hooks, const core::ParamRegistry& = core::builtinParams());

    // True when the event was one of the DJ section's.
    bool handle(const io::ControlEvent&);
    // Once per UI frame with the engine's playhead: follows tempo changes made by others.
    void frame(const engine::PlayheadFeed&);
    // SYNC, MASTER, TEMPO RESET, FX ON and LIVE SAMPLING.
    void lamps(io::LampFrame&) const;

    // The fader's position, −1 (minus end) … 1, 0 in the centre's dead zone. `raw` 0–1023.
    static double faderOffset(int raw) noexcept;
    // The tempo the fader plays while it moves the tempo away from the base, else 0.
    double liveTempo() const noexcept;
    double baseTempo() const noexcept { return base_; }
    int rangePercent() const noexcept { return kRanges[range_]; }
    bool tempoReset() const noexcept { return reset_; }
    bool following() const; // SYNC: an external MIDI clock sets the tempo

private:
    double offset() const noexcept; // the fader's offset in effect (0 with TEMPO RESET)
    double target() const noexcept;
    void apply(bool force);
    float value(std::string_view id, float fallback) const;
    void set(std::string_view id, float v);
    bool masterFxOn() const;

    ParamAccess& values_;
    fx::FxSlotAccess& fx_;
    Hooks hooks_;
    const core::ParamRegistry& params_;
    int raw_ = kFaderCentre, range_ = 0;
    bool reset_ = false;
    double base_ = 120.0, posted_ = 0.0;
    std::uint64_t feedBlock_ = 0, postedAt_ = 0, handBackAt_ = 0;
    bool started_ = false, followed_ = false;
};
} // namespace ui::input::deck
