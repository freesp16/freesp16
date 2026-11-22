// A controller profile compiled for the MIDI reader thread (control-surface.md UI-130…133, UI-118).
// One per controller port. map() turns one complete MIDI message into at most four logical
// ControlEvents and, for a pad hit, pushes engine::TriggerNow into the engine's TriggerSink at once (UI-118: the
// UI thread never re-triggers, PadController treats sources ≥ kSourceMidi as already sent).
//
//   lookup     flat 3 × 16 × 128 table (status class note / cc / other, channel, number) → binding index + 1:
//              O(1), 12 KB, no allocation; realtime (FA/FB/FC) and MMC commands have their own small tables
//   pads       note on → PadDown with the device's velocity curve (PadResponse: PAD CURVE, THRESHOLD, SENSITIVITY,
//              FIXED, UI-114) and the per-pad trim; poly aftertouch → PadPressure (UI-115); with the profile's
//              `ext.steps` held and "padBank" on, the pads are step.1…16 (UI-133), no trigger
//   encoders   relative CCs (two's complement, binary offset, sign-magnitude) → Turn × scale; absolute CCs, channel
//              pressure and pitch bend → Turn by the difference (the first value only sets the reference; a jump of
//              ≥ 64 — 127 → 0 — is a wrap and ignored, UI-132)
//   absolute   afs.* / main.volume: absolute CC 0–127 and pitch bend → Value 0–1023; relative CCs move an
//              accumulator (starts at 512; the consumer's pick-up mode, UI-116, keeps it from jumping)
//   strip      pitch bend 14 bit → position 0–127: Touch while off centre or while the `contact` CC is ≥ 64, Value
//              while it moves, Release when it returns to centre without contact or the contact CC drops
//   buttons    notes (on/off), CCs (press at ≥ 64), MMC and realtime (a Press and a Release)
// A message that matches a binding is consumed (UI-136: not passed to the MIDI-track input); others are not.
// Threads: map() on the port's reader thread only; setResponse()/setTrim() from the message thread (relaxed atomics,
// one byte each). No allocation, no lock, no logging in map().
#pragma once

#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "io/controllers/PadCurve.h"
#include "io/controllers/profiles/Profile.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace io::controllers
{
struct MapResult
{
    std::array<ControlEvent, 4> events {};
    int count = 0;
    bool consumed = false;
    bool triggered = false; // a TriggerNow went out
    void add(const ControlEvent& e) noexcept
    {
        if (count < static_cast<int>(events.size()))
            events[static_cast<std::size_t>(count++)] = e;
    }
};

// MIDI velocity 1–127 → hit velocity through the device's PadResponse (0 = below the threshold: no hit). Our own shapes
// (the reference's curves act on its sensor words, io/controllers/PadCurve.h): sensitivity scales the velocity by
// sensitivity / 8, Hard n / Soft n = x^(1 + n/2) / x^(1 / (1 + n/2)), THRESHOLD n ignores velocities below
// (n − 1) · 4, FIXED = the fixed velocity for every hit.
std::array<std::uint8_t, 128> velocityTable(const PadResponse&) noexcept;

class ControllerMapper
{
public:
    static constexpr int kClasses = 3, kChannels = 16, kNumbers = 128;

    ControllerMapper(const LoadedProfile& profile, std::uint8_t source);

    // One complete message: a channel message (1–3 bytes), a realtime byte, or an MMC SysEx (F0 7F dd 06 cc F7).
    // tMs = arrival on the steady clock (ControlEvent::timeMs).
    bool map(const std::uint8_t* msg, int len, double tMs, MapResult& out, engine::TriggerSink* triggers) noexcept;

    void setResponse(const PadResponse&) noexcept;
    void setTrim(int pad0, int trim) noexcept; // −20…+20 velocity, added after the curve (UI-330 PAD TRIM)
    int lastVelocity() const noexcept { return lastVelocity_.load(std::memory_order_relaxed); } // VELOCITY TEST
    bool bankHeld() const noexcept { return bankHeld_; }
    std::uint8_t source() const noexcept { return source_; }
    // The binding of a message (tests, MIDI learn: "already mapped to …"); null when unbound.
    const MidiBinding* bindingFor(std::uint8_t statusByte, std::uint8_t number) const noexcept;

private:
    struct State
    {
        std::int16_t last = -1;  // absolute reference for detents by difference
        std::int16_t acc = 512;  // relative → absolute accumulator
        bool down = false;       // CC buttons: pressed
    };
    static constexpr std::size_t slot(int cls, int ch, int n) noexcept
    {
        return static_cast<std::size_t>((cls * kChannels + ch) * kNumbers + n);
    }
    void put(int cls, int channel, int number, std::uint16_t index) noexcept;
    void control(const MidiBinding&, State&, int value, int bits, bool on, double t, MapResult&,
                 engine::TriggerSink*) noexcept;
    void pad(const MidiBinding&, int velocity, double t, MapResult&, engine::TriggerSink*) noexcept;
    void strip(int position, int contact, double t, MapResult&) noexcept; // contact −1 = unchanged
    ControlEvent event(ControlEvent::Kind k, Control c, int v, double t) const noexcept
    {
        return {k, c, static_cast<std::int16_t>(v), source_, t};
    }

    std::vector<MidiBinding> bindings_;
    std::vector<State> state_;
    std::array<std::uint16_t, kClasses * kChannels * kNumbers> table_ {};
    std::array<std::uint16_t, 3> realtime_ {}; // FA, FB, FC
    std::array<std::uint16_t, 128> mmc_ {};
    std::array<std::atomic<std::uint8_t>, 128> velocity_ {};
    std::array<std::atomic<std::int8_t>, 16> trim_ {};
    std::array<std::uint8_t, 16> padAs_ {}; // 0 up, 1 down as a pad, 2 down as a step (bank)
    std::atomic<int> lastVelocity_ {0};
    std::uint8_t source_;
    bool padBank_ = false, bankHeld_ = false;
    bool contact_ = false, touchCc_ = false;
    int position_ = 64;
};
} // namespace io::controllers
