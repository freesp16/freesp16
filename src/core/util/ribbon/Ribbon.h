// The touch strip's shared vocabulary (spec 02-features/touch-strip.md RIB-001…006, 03-ui/control-surface.md
// UI-157): the four modes, the position laws of PITCH / REPEAT / USER and the 8-LED mask — constexpr, used by the
// engine (engine/ribbon/RibbonEngine), the message thread (io/controllers/StripSource) and the platforms' strip
// sources, so the sound, the LEDs and the screens agree. No state, no allocation.
//
// The live strip and pad-pressure stream reaches the engine as TriggerNow messages with reserved pad codes (our design:
// the engine contract has no strip channel): the device's strip reader pushes them into the engine's TriggerSink from
// its reader thread (one-block latency, like a pad hit, UI-118), every other strip (keyboard, script, controller)
// through EngineBridge::post from the message thread.
//   strip      {track 0, velocity = position 0–127, note = contact 0/1, pad = kStripPad}
//   pressure   {track = pad 0–15, velocity = pressure 0–127, note 0, pad = kPressurePad}
// Engines without the ribbon would read a pad code < −1 as a MIDI note, so only engine::Engine (which intercepts them
// before the trigger router) may receive them.
#pragma once

#include <cstdint>

namespace core::ribbon
{
enum class Mode : std::uint8_t
{
    Pitch,
    Repeat,
    User1,
    User2
}; // MODE cycles in this order (RIB-001), PITCH at start-up
constexpr int kModes = 4;
constexpr int kUserSets = 2, kUserItems = 8; // USER1/USER2 × 8 items (RIB-004, M p.43)
constexpr int kPositions = 128;              // 0 bottom … 127 top
constexpr int kCentre = 64;                  // PITCH: no bend
constexpr int kLeds = 8;

constexpr Mode nextMode(Mode m) noexcept { return static_cast<Mode>((static_cast<int>(m) + 1) % kModes); }
constexpr const char* modeName(Mode m) noexcept
{
    return m == Mode::Pitch ? "PITCH" : m == Mode::Repeat ? "REPEAT" : m == Mode::User1 ? "USER1" : "USER2";
}
constexpr int clampPosition(int p) noexcept { return p < 0 ? 0 : (p > kPositions - 1 ? kPositions - 1 : p); }

// TriggerNow pad codes of the strip stream (see above).
constexpr std::int8_t kStripPad = -16, kPressurePad = -17;

// RIB-002: centre 64 = 0 cents, linear to topCents at 127 and to bottomCents at 0 (default +200 / −200).
constexpr float pitchCents(int position, int bottomCents, int topCents) noexcept
{
    const int p = clampPosition(position);
    return p >= kCentre ? static_cast<float>(topCents) * static_cast<float>(p - kCentre) / 63.0f
                        : static_cast<float>(bottomCents) * static_cast<float>(kCentre - p) / 64.0f;
}
// Incoming MIDI pitch bend (RIB-007): 14-bit value, 8192 = centre, onto the same range.
constexpr float bendCents(int value14, int bottomCents, int topCents) noexcept
{
    const int v = value14 < 0 ? 0 : (value14 > 16383 ? 16383 : value14);
    return v >= 8192 ? static_cast<float>(topCents) * static_cast<float>(v - 8192) / 8191.0f
                     : static_cast<float>(bottomCents) * static_cast<float>(8192 - v) / 8192.0f;
}

// RIB-003 / OQ-21 (our choice): four equal zones from the bottom, 0 = 1/4 … 3 = 1/32 note.
constexpr int repeatZone(int position) noexcept { return clampPosition(position) / 32; }
constexpr double repeatQuarters(int zone) noexcept // the interval in quarter notes
{
    return zone <= 0 ? 1.0 : zone == 1 ? 0.5 : zone == 2 ? 0.25 : 0.125;
}
constexpr const char* repeatName(int zone) noexcept
{
    return zone <= 0 ? "1/4" : zone == 1 ? "1/8" : zone == 2 ? "1/16" : "1/32";
}

// RIB-004: an item maps the position linearly from BOTTOM VALUE (0) to TOP VALUE (127), normalised 0–1.
constexpr float userValue(int position, float bottom01, float top01) noexcept
{
    return bottom01 + (top01 - bottom01) * static_cast<float>(clampPosition(position)) / 127.0f;
}

// RIB-006 / UI-157: bit i = LED i (bottom → top). PITCH and USER light up to the touched segment (position / 16);
// REPEAT lights its rate zone (two LEDs). shown = touched, or HOLD keeps a value.
constexpr int ledSegment(int position) noexcept { return clampPosition(position) * kLeds / kPositions; }
constexpr std::uint8_t ledMask(Mode m, int position, bool shown) noexcept
{
    if (! shown)
        return 0;
    if (m == Mode::Repeat)
        return static_cast<std::uint8_t>(3u << (2 * repeatZone(position)));
    return static_cast<std::uint8_t>((2u << ledSegment(position)) - 1u);
}

// Where PITCH and USER act (our preference ribbon.applyTo, RIB-002): the held pads' tracks (the reference), the
// selected track or every track. REPEAT always repeats the held pads.
enum class ApplyTo : std::uint8_t
{
    HeldPads,
    SelectedTrack,
    AllTracks
};
} // namespace core::ribbon
