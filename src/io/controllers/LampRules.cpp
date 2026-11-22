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

LampOutput resolve(const LampState& s, double nowMs, LampBrightness b) noexcept
{
    int level = 0;
    std::uint32_t rgb = s.rgb;
    switch (s.mode)
    {
    case LampMode::Off: break;
    case LampMode::On: level = 255; break;
    case LampMode::Dim: level = kDimLevel; break;
    case LampMode::Faint: level = kFaintLevel; break;
    case LampMode::BlinkSlow: level = blinkOn(nowMs, s.sinceMs, kBlinkSlowHz) ? 255 : 0; break;
    case LampMode::BlinkFast: level = blinkOn(nowMs, s.sinceMs, kBlinkFastHz) ? 255 : 0; break;
    case LampMode::Flash:
    {
        const double t = std::clamp((nowMs - s.sinceMs) / kFlashMs, 0.0, 1.0);
        rgb = blend(lampcolour::white, s.rgb, t);
        level = rgb == 0 ? 0 : 255;
        break;
    }
    }
    if (rgb == 0)
        level = 0;
    level = s.mode == LampMode::Faint ? std::max(1, level * brightnessScale(b) / 255) : level * brightnessScale(b) / 255;
    return {level == 0 ? 0u : rgb, static_cast<std::uint8_t>(level)};
}

void apply(const LampFrame& frame, double nowMs, LampBrightness b, LampSink& sink)
{
    for (const auto c : lampControls())
    {
        const auto out = resolve(frame[c], nowMs, b);
        sink.setLamp(c, out.rgb, out.level);
    }
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
    if (! p.hasContent && mode != PadMode::Slice && mode != PadMode::Scale)
        return {};
    LampState base;
    switch (mode)
    {
    // A muted track's pad glows faintly in every mode that shows the tracks (hardware feedback: "just a small light").
    case PadMode::None: base = p.muted ? faint(p.rgb) : on(p.rgb); break;
    case PadMode::Track: base = p.muted ? faint(p.rgb) : p.selected ? on(p.rgb) : dim(p.rgb); break; // selected brighter
    case PadMode::Mute: base = p.muted ? faint(p.rgb) : on(p.rgb); break;
    case PadMode::Slice:
        if (! p.inSlices)
            return {};
        base = p.playing ? LampState {LampMode::Flash, p.rgb, p.lastHitMs} : on(p.rgb);
        return base;
    case PadMode::Scale: base = p.root ? on(p.rgb) : dim(p.rgb); break;
    // MUTE held: a soloed track full, a track silenced by another's solo or by its mute faint, no
    // solo set = dim. No hit flash: the pads do not sound meanwhile.
    case PadMode::Solo: return p.muted ? faint(p.rgb) : p.soloed ? on(p.rgb) : p.anySolo ? faint(p.rgb) : dim(p.rgb);
    }
    if (base.mode == LampMode::On && p.lastHitMs > -1e11)
        return {LampMode::Flash, p.rgb, p.lastHitMs}; // resolve() fades it back to the colour
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
