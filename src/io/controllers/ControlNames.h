// The logical control map (03-ui/control-surface.md §2.1, UI-100…102; Part 3 lamps): what kind of control each
// io::Control is, whether and how it lights, and a compact text form of a ControlEvent for logs (the app's INPUT
// lines). ControlSurface.h holds the enum and the spec-name parser; this file adds the facts the
// sources, the lamp rules and the tests need about each control.
//
// Lamp-only extension (our addition, a gap in the control contract): UI-101 gives every `bar.n` "button + lamp +
// position lamp", but io::Control has one id per BAR key. The four playback-bar position lamps are addressed with
// barPositionLamp(n), ids just past Control::Count; LampSinks accept them, sources never produce them.
#pragma once

#include "io/controllers/ControlSurface.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace io
{
enum class ControlKind : std::uint8_t
{
    Pad,      // velocity pad + pressure: Press (velocity) / Pressure / Release
    Button,   // Press / Release
    Encoder,  // Turn (signed detents)
    Absolute, // Value 0…1023 (afs.*, main.volume)
    Strip,    // Touch (contact, position) / Value (move) / Release; position 0…127
    Screen,   // Touch on Control::Screen: value 1 = down, 2 = drag, 0 = up; x, y in 800 × 480 points (ui/shell)
    LampOnly, // produces no events
};
enum class LampKind : std::uint8_t { None, Mono, Rgb };

struct ControlInfo
{
    ControlKind kind;
    LampKind lamp;
    std::int16_t maxValue;  // largest event value (velocity 127, absolute 1023, strip 127), 0 = unbounded / n.a.
    std::uint8_t group;     // one-hot lamp group: 1 = pad modes, 2 = strip mode indicators, 0 = none
    bool reference;         // part of the reference set of UI-101 (false: extension controls of UI-102)
};

constexpr int kBarPositionLamps = 4;
constexpr Control barPositionLamp(int n1Based) noexcept
{
    return static_cast<Control>(static_cast<int>(Control::Count) + n1Based - 1);
}
constexpr bool isBarPositionLamp(Control c) noexcept
{
    return c >= barPositionLamp(1) && c <= barPositionLamp(kBarPositionLamps);
}
// Every lamp id: the controls plus the bar position lamps (array size for per-lamp state).
constexpr int kLampSlots = static_cast<int>(Control::Count) + kBarPositionLamps;
constexpr int lampSlot(Control c) noexcept { return static_cast<int>(c); }

ControlInfo controlInfo(Control) noexcept;
// The input controls of UI-101 (every reference control that produces events), in §2.1 table order.
std::span<const Control> referenceInputControls() noexcept;
// Every control that lights (UI-101 lamps, including the bar position lamps).
std::span<const Control> lampControls() noexcept;

// Spec name, also for the bar position lamps ("bar.position.1"); empty for an unknown id.
std::string_view lampName(Control) noexcept;
std::string_view kindName(ControlEvent::Kind) noexcept; // "press", "release", "turn", "value", "touch", "pressure"

// "play press 0", "enc.3 turn -2", "screen touch 1 @412,96": the part of an INPUT log line after the source.
// Writes at most n - 1 characters plus the terminator; returns the length written.
std::size_t describe(const ControlEvent&, char* out, std::size_t n) noexcept;
} // namespace io
