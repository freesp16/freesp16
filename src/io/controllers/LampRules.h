// Lamp and colour feedback rules (03-ui/control-surface.md Part 3, UI-150…161): the lamp states (off, on, dim,
// blink-slow 2 Hz, blink-fast 6 Hz, flash = on then fade), the fixed signal colours, the brightness preference, and
// the per-group rules that turn transport/mode/sequence facts into lamp states. The LampModel of UI-150 (message
// thread, 60 Hz, reads the model and transport) fills a LampFrame with these rules and calls apply(), which resolves
// every state at `nowMs` and sends it to a LampSink (the panel's TX frames, the virtual panel, controller profiles).
//
// Device-independent and allocation-free; resolve() is a few multiplies per lamp.
#pragma once

#include "io/controllers/ControlNames.h"
#include "io/controllers/ControlSurface.h"

#include <array>
#include <cstdint>
#include <span>

namespace io
{
enum class LampMode : std::uint8_t { Off, On, Dim, BlinkSlow, BlinkFast, Flash, Faint };

struct LampState
{
    LampMode mode = LampMode::Off;
    std::uint32_t rgb = 0xFFFFFF; // colour (mono lamps: any non-zero colour = lit)
    double sinceMs = 0;           // flash start; blink phase origin
};

struct LampOutput
{
    std::uint32_t rgb;  // 0xRRGGBB
    std::uint8_t level; // 0 = off … 255 = full (LampSink::setLamp's level)
};

enum class LampBrightness : std::uint8_t { Normal, Dimmed1, Dimmed2, Dimmed3 }; // UI-159: ≈ 100, 50, 33, 25 %

namespace lampcolour // UI-160 fixed signal colours (our values; the reference's were not recovered)
{
inline constexpr std::uint32_t white = 0xFFFFFF, blue = 0x2050FF, red = 0xFF1E28, green = 0x28F03C, amber = 0xFFB020;
}

inline constexpr double kBlinkSlowHz = 2.0, kBlinkFastHz = 6.0; // UI-150
inline constexpr double kFlashMs = 1000.0;                      // UI-151: back to the base colour over about 1 s
inline constexpr std::uint8_t kDimLevel = 90;                   // "dim" ≈ 35 % of full (ours)
// "faint": the smallest light that still shows (hardware feedback: muted pads "just a small light"); 6/255 is 2 of the
// panel's 74 RGB steps and stays ≥ 1 step at the lowest brightness preference (PanelCodec never rounds a lit channel
// down to 0).
inline constexpr std::uint8_t kFaintLevel = 6;

// The level of a lamp state at `nowMs` (UI-150), scaled by the brightness preference (UI-159). Flash fades from
// white to the state's colour over kFlashMs; blinks are 50 % duty from sinceMs.
LampOutput resolve(const LampState&, double nowMs, LampBrightness = LampBrightness::Normal) noexcept;

// One state per lamp id (controls + bar position lamps).
struct LampFrame
{
    std::array<LampState, kLampSlots> lamps {};
    struct AnalogFilter
    {
        bool inUse = false; // afs.active on with afs.processor EXTERNAL (the unit's own filter, not the model)
        int hpCutoff = 0;   // afs.hpCutoff 0–1023 (the HP CUTOFF pot or the popup)
    } analogFilter;         // not a lamp: the filter board's select line (LampSink::setAnalogFilter)
    LampState& operator[](Control c) noexcept { return lamps[static_cast<std::size_t>(lampSlot(c))]; }
    const LampState& operator[](Control c) const noexcept { return lamps[static_cast<std::size_t>(lampSlot(c))]; }
};

// Resolves every lamp of lampControls() and sends it to the sink, then flush() (message thread).
void apply(const LampFrame&, double nowMs, LampBrightness, LampSink&);

// ---- rules ----

enum class PadMode : std::uint8_t { None, Track, Mute, Slice, Scale, Solo }; // Solo: MUTE held (no mode key of its own)
enum class StripMode : std::uint8_t { Pitch, Repeat, User1, User2 };

// UI-156: pad-mode keys white while on (one-hot or none); REC red while armed, blinking while waiting for the
// pre-count; PLAY green while running, blinking while paused; SCENE/PATTERN white while on; HOLD lit; ACTIVE red;
// MODE indicators one-hot; UI-158 OVER amber.
struct ModeLamps
{
    PadMode padMode = PadMode::None;
    bool recArmed = false, recWaiting = false, playing = false, paused = false;
    bool sceneMode = false, patternMode = false, hold = false, afsActive = false, over = false;
    StripMode stripMode = StripMode::Pitch;
};
void modeLamps(const ModeLamps&, LampFrame&) noexcept;

// UI-157: the 8 strip LEDs (bottom → top) show the touched position, or the held one while HOLD is on: lit up to the
// segment in PITCH and USER modes, one segment in REPEAT. position 0…127; dark without contact and without hold.
void stripLamps(int position, bool contact, bool hold, StripMode, LampFrame&) noexcept;
int stripSegment(int position) noexcept; // 0…7

// UI-151/152: one pad. hasContent false = dark; a hit within kFlashMs flashes white back to the colour.
struct PadLamp
{
    bool hasContent = false;
    std::uint32_t rgb = 0;     // track (pad) colour
    double lastHitMs = -1e12;  // most recent hit
    bool selected = false;     // TRACK mode: the selected track
    bool muted = false;        // MUTE mode
    bool inSlices = true;      // SLICE mode: a slice of the selected track
    bool playing = false;      // SLICE mode: the playing slice
    bool root = true;          // SCALE mode: a root note
    bool soloed = false;       // SOLO view (MUTE held): the track is soloed
    bool anySolo = false;      // SOLO view: some track of the scene is soloed
};
LampState padLamp(PadMode, const PadLamp&) noexcept;

// UI-153: step lamps of the shown bar on the selected track (step entry). trig per step: 0 none, 1 FULL, 2 HALF,
// 3 PARAM; playStep = 1…16 of the shown bar while playing, else 0 (white).
void sequenceStepLamps(std::span<const std::uint8_t, 16> trig, std::uint32_t trackRgb, int playStep,
                       LampFrame&) noexcept;
// UI-154/161: PATTERN mode (patterns of the current scene) and SCENE mode (scenes): item state per key.
enum class SlotState : std::uint8_t { Empty, Recorded, Current, Pending };
void slotStepLamps(std::span<const SlotState, 16> slots, std::span<const std::uint32_t, 16> rgb, double pendingSinceMs,
                   LampFrame&) noexcept;
// UI-154 pattern-length mode: every step key white, BAR keys 1…bars blue.
void lengthModeLamps(int bars, LampFrame&) noexcept;
// UI-155: BAR keys lit inside the pattern length, the shown bar brighter; the position lamp shows the playing bar and
// pulses on the beat while playing (beatPhase 0…1 within the beat; on for the first half).
void barLamps(int lengthBars, int shownBar, int playingBar, bool playing, double beatPhase, LampFrame&) noexcept;
} // namespace io
