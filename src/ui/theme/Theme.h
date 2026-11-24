// The active theme: generated token constants (tools/gen_theme.py <- resources/design/tokens.json) plus the dark/light
// choice and the motion helpers. Components reference token names only (UID-050, UID-230); no colour, size or duration
// is written in a component. Dark is the default palette, light is generated from the same file (UID-051, UID-017);
// the choice reaches every widget through UiContext::palette.
#pragma once

#include "ui/ThemeTokens.h"
#include "ui/theme/Skin.h"
#include "ui/render/DrawList.h"

#include <algorithm>
#include <cstdint>

namespace ui::theme
{
namespace tk = ::theme::tokens;

inline const tk::Palette& palette(bool dark = true) noexcept
{
    return dark ? tk::dark : tk::light;
}

inline const tk::Palette& palette(Skin skin) noexcept
{
    return skin == Skin::Classic ? tk::classic : palette(skin == Skin::Dark);
}

inline const tk::TextStyle& textStyle(TextStyleId id) noexcept
{
    static constexpr const tk::TextStyle* styles[] = {&tk::caption,    &tk::label,  &tk::body,
                                                      &tk::bodyStrong, &tk::title,  &tk::valueS,
                                                      &tk::value,      &tk::valueL, &tk::display};
    return *styles[static_cast<int>(id)];
}

// Track palette (UID-056): base colour, or the dim variant for "has content but inactive" / empty chips.
inline std::uint32_t trackColour(int track1Based, bool dim = false) noexcept
{
    const auto& t = tk::track[(track1Based - 1) & 15];
    return dim ? t.dim : t.base;
}

// Saturated strips identify the same sixteen pad assignments on the Classic screen.
inline std::uint32_t trackColour(const tk::Palette& p, int track1Based, bool dim = false) noexcept
{
    if (! p.classic)
        return trackColour(track1Based, dim);
    static constexpr std::uint32_t colours[] = {
        0xf04a3a, 0xf2672b, 0xf5822a, 0xf8a82c, 0xf6cd36, 0xe8e64b, 0xaeed4a, 0x60e05a,
        0x24db98, 0x22ced7, 0x329eef, 0x657bef, 0x9365eb, 0xce52eb, 0xf14baf, 0xf33e75};
    const auto c = colours[(track1Based - 1) & 15];
    return (p.bg & 0xff000000u) | (dim ? ((c >> 1) & 0x007f7f7fu) : c);
}

// The panel's RGB LEDs (requested: "different colors for each pad"): the 16 pastel track colours are 6–40° apart
// in hue and wash out to near-white on the LEDs, so tracks 1–6 looked alike. A lamp in track colour n (base or dim)
// shows LED colour n instead: fully saturated, hand-spaced for RGB LEDs (green is the brightest die), in the track
// colours' order (red → orange → yellow → green → cyan → blue → violet → pink). Any other colour passes unchanged.
inline constexpr std::uint32_t kLedColours[16] = {0xFF0000, 0xFF3000, 0xFF7000, 0xFFC000, 0x80FF00, 0x00FF00,
                                                  0x00FF50, 0x00FFB0, 0x00C0FF, 0x0070FF, 0x0000FF, 0x5000FF,
                                                  0xA000FF, 0xFF00FF, 0xFF0080, 0xFF80A0};
inline std::uint32_t ledColour(std::uint32_t rgb) noexcept
{
    rgb &= 0x00FFFFFFu;
    for (int k = 0; k < 16; ++k)
        if (rgb == (tk::track[k].base & 0x00FFFFFFu) || rgb == (tk::track[k].dim & 0x00FFFFFFu))
            return kLedColours[k];
    return rgb;
}

// Scene n defaults to track colour n (tokens.json color.scene, UID-057).
inline std::uint32_t sceneColour(int scene1Based) noexcept
{
    return trackColour(scene1Based);
}

// Colour with its alpha multiplied by `opacity` (0..1).
inline std::uint32_t withOpacity(std::uint32_t argb, float opacity) noexcept
{
    const auto a = static_cast<std::uint32_t>(static_cast<float>(argb >> 24) * std::clamp(opacity, 0.0f, 1.0f) + 0.5f);
    return (a << 24) | (argb & 0x00FFFFFFu);
}

// Straight mix of two opaque colours: a*(1-f) + b*f.
inline std::uint32_t mix(std::uint32_t a, std::uint32_t b, float f) noexcept
{
    auto ch = [&](int shift)
    {
        const float x = static_cast<float>((a >> shift) & 255u), y = static_cast<float>((b >> shift) & 255u);
        return static_cast<std::uint32_t>(x + (y - x) * f + 0.5f) << shift;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

// A step with p-locks (requested: "slightly different colour, to see that it is modified"): the track colour
// lightened 45 % towards white — on the screen's step grid and the step keys (where it is no longer an exact track
// colour, so the LEDs show this pastel tint instead of the saturated ledColour of a plain step).
inline std::uint32_t lockedTint(std::uint32_t rgb) noexcept
{
    return mix(rgb, 0xFFFFFFFFu, 0.45f) & 0x00FFFFFFu;
}

// The palette with every colour's alpha multiplied by `opacity`: popups and screens fade in and out by building with a
// faded copy (the DrawList has no layer opacity). Cheap: 21 words.
inline tk::Palette faded(const tk::Palette& p, float opacity) noexcept
{
    tk::Palette out = p;
    for (auto* c :
         {&out.bg,    &out.surface1, &out.surface2, &out.surface3, &out.overlay,    &out.border,   &out.borderStrong,
          &out.text1, &out.text2,    &out.text3,    &out.accent,   &out.accentWeak, &out.onAccent, &out.mod,
          &out.play,  &out.rec,      &out.warn,     &out.playhead, &out.meterLow,   &out.meterMid, &out.meterHigh})
        *c = withOpacity(*c, opacity);
    return out;
}

// The tokens' standard curve cubic-bezier(x1, y1, x2, y2) (tokens.json motion.*.easing) evaluated at t in 0..1.
inline float ease(float t) noexcept
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    const float x1 = tk::motionEase[0], y1 = tk::motionEase[1], x2 = tk::motionEase[2], y2 = tk::motionEase[3];
    auto bez = [](float a, float b, float u)
    { return 3 * a * u * (1 - u) * (1 - u) + 3 * b * u * u * (1 - u) + u * u * u; };
    float lo = 0, hi = 1, u = t; // x(u) is monotonic: bisection, 16 steps is below 1e-4
    for (int i = 0; i < 16; ++i)
    {
        u = 0.5f * (lo + hi);
        (bez(x1, x2, u) < t ? lo : hi) = u;
    }
    return bez(y1, y2, u);
}

// Progress 0..1 of an animation of `durationMs` that started at `startMs` (1 when the duration is 0: reduce motion,
// UID-066, or an interrupted animation, UID-065).
inline float progress(double nowMs, double startMs, double durationMs) noexcept
{
    if (durationMs <= 0)
        return 1.0f;
    return static_cast<float>(std::clamp((nowMs - startMs) / durationMs, 0.0, 1.0));
}

// 50 % duty blink of the given period (motion.blink-slow / blink-fast): true in the "on" half.
inline bool blinkOn(double nowMs, int periodMs) noexcept
{
    if (periodMs <= 0)
        return true;
    const auto phase = static_cast<long long>(nowMs < 0 ? 0 : nowMs) % periodMs;
    return phase < periodMs / 2;
}
} // namespace ui::theme
