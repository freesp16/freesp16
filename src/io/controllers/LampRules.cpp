// Lamp rules (03-ui/control-surface.md Part 3). See LampRules.h.
#include "io/controllers/LampRules.h"

#include <algorithm>
#include <cmath>

namespace io
{
namespace
{
constexpr std::uint8_t brightnessScale(LampBrightness b) noexcept // UI-159, out of 255
{
    switch (b)
    {
    case LampBrightness::Normal: return 255;
    case LampBrightness::Dimmed1: return 128;
    case LampBrightness::Dimmed2: return 84;
    case LampBrightness::Dimmed3: return 64;
    }
    return 255;
}

std::uint32_t blend(std::uint32_t from, std::uint32_t to, double t) noexcept // t 0 = from, 1 = to
{
    std::uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8)
    {
        const double a = (from >> shift) & 0xFF, b = (to >> shift) & 0xFF;
        out |= static_cast<std::uint32_t>(std::lround(a + (b - a) * t)) << shift;
    }
    return out;
}

bool blinkOn(double nowMs, double sinceMs, double hz) noexcept
{
    const double periodMs = 1000.0 / hz;
    const double phase = std::fmod(std::max(0.0, nowMs - sinceMs), periodMs);
    return phase < periodMs * 0.5;
}

constexpr Control padModeKey(PadMode m) noexcept
{
    switch (m)
    {
    case PadMode::Track: return Control::PadModeTrack;
    case PadMode::Mute: return Control::PadModeMute;
    case PadMode::Slice: return Control::PadModeSlice;
    case PadMode::Scale: return Control::PadModeScale;
    case PadMode::None:
    case PadMode::Solo: break;
    }
    return Control::Count;
}

LampState on(std::uint32_t rgb) noexcept { return {LampMode::On, rgb, 0}; }
LampState dim(std::uint32_t rgb) noexcept { return {LampMode::Dim, rgb, 0}; }
LampState faint(std::uint32_t rgb) noexcept { return {LampMode::Faint, rgb, 0}; }
LampState lit(bool b, std::uint32_t rgb) noexcept { return b ? on(rgb) : LampState {}; }
} // namespace

namespace
{
double pulse(double nowMs) noexcept // 0 … 1 … 0 over kPulseMs, the same phase for every lamp
{
    constexpr double kTwoPi = 6.283185307179586;
    return 0.5 - 0.5 * std::cos(kTwoPi * std::fmod(std::max(0.0, nowMs), kPulseMs) / kPulseMs);
}

int steadyLevel(const LampState& s, double nowMs) noexcept // the unscaled level of every state but Flash
{
    switch (s.mode)
    {
    case LampMode::Off: return 0;
    case LampMode::On: return 255;
    case LampMode::Dim: return kDimLevel;
    case LampMode::Faint: return kFaintLevel;
    case LampMode::Level: return s.level;
    case LampMode::Pulse: return static_cast<int>(std::lround(s.level + (255 - s.level) * pulse(nowMs)));
    case LampMode::BlinkSlow: return blinkOn(nowMs, s.sinceMs, kBlinkSlowHz) ? 255 : 0;
    case LampMode::BlinkFast: return blinkOn(nowMs, s.sinceMs, kBlinkFastHz) ? 255 : 0;
    case LampMode::Flash: break;
    }
    return 255;
}
} // namespace

LampOutput resolve(const LampState& s, double nowMs, LampBrightness b) noexcept
{
    int level = 0;
    std::uint32_t rgb = s.rgb;
    LampMode shape = s.mode; // Faint keeps at least 1 under the brightness preference
    if (s.mode == LampMode::Flash)
    {
        // Full white at the hit, eased back to the settle state: e = (1 − t)⁴ of the hit's light is left.
        const double t = std::clamp((nowMs - s.sinceMs) / kFlashMs, 0.0, 1.0);
        const double e = (1.0 - t) * (1.0 - t) * (1.0 - t) * (1.0 - t);
        const LampState base {s.settle == LampMode::Flash ? LampMode::On : s.settle, s.rgb, s.sinceMs, s.level};
        const int settled = s.rgb == 0 ? 0 : steadyLevel(base, nowMs);
        rgb = blend(s.rgb, lampcolour::white, e);
        level = static_cast<int>(std::lround(settled + (255 - settled) * e));
        shape = e > 0.0 ? LampMode::On : base.mode;
    }
    else
        level = steadyLevel(s, nowMs);
    if (rgb == 0)
        level = 0;
    level = shape == LampMode::Faint && level > 0 ? std::max(1, level * brightnessScale(b) / 255)
                                                   : level * brightnessScale(b) / 255;
    return {level == 0 ? 0u : rgb, static_cast<std::uint8_t>(level)};
}

void apply(const LampFrame& frame, double nowMs, LampBrightness b, LampSink& sink)
{
    for (const auto c : lampControls())
    {
        const auto out = resolve(frame[c], nowMs, b);
        sink.setLamp(c, out.rgb, out.level);
    }
    sink.setAnalogFilter(frame.analogFilter.inUse, frame.analogFilter.hpCutoff);
    sink.flush();
}

void modeLamps(const ModeLamps& m, LampFrame& f) noexcept
{
    for (auto mode : {PadMode::Track, PadMode::Mute, PadMode::Slice, PadMode::Scale})
        f[padModeKey(mode)] = lit(m.padMode == mode, lampcolour::white);
    const LampState recBlink {LampMode::BlinkSlow, lampcolour::red, 0}, playBlink {LampMode::BlinkSlow, lampcolour::green, 0};
    f[Control::Rec] = m.recWaiting ? recBlink : lit(m.recArmed, lampcolour::red);
    f[Control::Play] = m.paused ? playBlink : lit(m.playing, lampcolour::green);
    f[Control::Stop] = {};
    f[Control::Scene] = lit(m.sceneMode, lampcolour::white);
    f[Control::Pattern] = lit(m.patternMode, lampcolour::white);
    f[Control::StripHold] = lit(m.hold, lampcolour::white);
    f[Control::AfsActive] = lit(m.afsActive, lampcolour::red);
    f[Control::AfsOver] = lit(m.over, lampcolour::amber);
    const Control modes[] = {Control::StripModePitch, Control::StripModeRepeat, Control::StripModeUser1,
                             Control::StripModeUser2};
    for (int i = 0; i < 4; ++i)
        f[modes[i]] = lit(static_cast<int>(m.stripMode) == i, lampcolour::white);
}

int stripSegment(int position) noexcept { return std::clamp(position, 0, 127) * 8 / 128; }

void stripLamps(int position, bool contact, bool hold, StripMode mode, LampFrame& f) noexcept
{
    const bool show = contact || hold;
    const int seg = stripSegment(position);
    for (int i = 0; i < 8; ++i)
    {
        const bool l = show && (mode == StripMode::Repeat ? i == seg : i <= seg);
        f[static_cast<Control>(static_cast<int>(Control::StripLed1) + i)] = lit(l, lampcolour::white);
    }
}

LampState padLamp(PadMode mode, const PadLamp& p) noexcept
{
    // SLICE: a pad without a slice is dark (the slice map decides, an empty track shows the default grid); SCALE
    // shows the layout of the selected track whatever it holds. An empty track lights like any other, so its mute
    // shows on the pad too.
    if (mode == PadMode::Slice && ! p.inSlices)
        return {};
    // Silent: muted, or another track holds the solo (the engine's audible = unmuted and, when any solo is set,
    // soloed). No hit flash: the pad does not sound. A muted pad is switched off, soloed or not; a pad silenced only
    // by another track's solo keeps the smallest light.
    if (p.muted)
        return {};
    if (p.anySolo && ! p.soloed) // silenced by a solo elsewhere: faint
        return faint(p.rgb);
    LampState base;
    switch (mode)
    {
    // The pad's own track: the selected one full, the others dim.
    case PadMode::None:
    case PadMode::Track:
    case PadMode::Mute:
    case PadMode::Solo: base = p.selected ? on(p.rgb) : dim(p.rgb); break;
    case PadMode::Slice: base = dim(p.rgb); break;                      // every slice of the selected track
    case PadMode::Scale: base = p.root ? on(p.rgb) : dim(p.rgb); break; // the selected track's notes
    }
    const bool tracks = mode != PadMode::Slice && mode != PadMode::Scale;
    if (tracks && p.soloed) // breathes from its own level (a selected pad stays near full) up to full
        base = {LampMode::Pulse, p.rgb, 0, base.mode == LampMode::On ? kPulseSelectedLevel : kDimLevel};
    // resolve() eases the white hit back to the base state within kFlashMs. An empty track's pad does not flash in
    // the track modes: pressing it plays nothing.
    if (p.lastHitMs > -1e11 && (p.hasContent || ! tracks))
        return {LampMode::Flash, p.rgb, p.lastHitMs, base.mode == LampMode::Pulse ? base.level : std::uint8_t {255},
                base.mode};
    return base;
}

void sequenceStepLamps(std::span<const std::uint8_t, 16> trig, std::uint32_t trackRgb, int playStep,
                       LampFrame& f) noexcept
{
    for (int i = 0; i < 16; ++i)
    {
        LampState s;
        switch (trig[static_cast<std::size_t>(i)])
        {
        case 1: s = on(trackRgb); break;
        case 2: s = dim(trackRgb); break;
        case 3: s = dim(lampcolour::white); break;
        default: break;
        }
        if (playStep == i + 1)
            s = on(lampcolour::white);
        f[step(i + 1)] = s;
    }
}

void slotStepLamps(std::span<const SlotState, 16> slots, std::span<const std::uint32_t, 16> rgb, double pendingSinceMs,
                   LampFrame& f) noexcept
{
    for (std::size_t i = 0; i < 16; ++i)
    {
        LampState s;
        switch (slots[i])
        {
        case SlotState::Empty: break;
        case SlotState::Recorded: s = dim(rgb[i]); break;
        case SlotState::Current: s = on(rgb[i]); break;
        case SlotState::Pending: s = {LampMode::BlinkFast, rgb[i], pendingSinceMs}; break;
        }
        f[step(static_cast<int>(i) + 1)] = s;
    }
}

void lengthModeLamps(int bars, LampFrame& f) noexcept
{
    for (int i = 1; i <= 16; ++i)
        f[step(i)] = on(lampcolour::white);
    const Control keys[] = {Control::Bar1, Control::Bar2, Control::Bar3, Control::Bar4};
    for (int i = 0; i < 4; ++i)
        f[keys[i]] = lit(i < bars, lampcolour::blue);
}

void barLamps(int lengthBars, int shownBar, int playingBar, bool playing, double beatPhase, LampFrame& f) noexcept
{
    const Control keys[] = {Control::Bar1, Control::Bar2, Control::Bar3, Control::Bar4};
    for (int i = 0; i < 4; ++i)
    {
        const int bar = i + 1;
        const LampState key = bar == shownBar ? on(lampcolour::white) : dim(lampcolour::white);
        f[keys[i]] = bar > lengthBars ? LampState {} : key;
        f[barPositionLamp(bar)] = lit(playing && bar == playingBar && beatPhase < 0.5, lampcolour::green);
    }
}
} // namespace io
