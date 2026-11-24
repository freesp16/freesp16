// ChannelStrip (07-ui-design/screens/mixer.md "ChannelStrip"; UI-270…274, UID-550…563): one track's strip on MIXER /
// OUTPUT SETTING, 58 wide on a 62 pitch at x = 168 + 62·(n−1), y 152–608: OUT ASSIGN text (badge in the output
// variant), SEND and PAN MiniRings (r 12, 3 lpx arc over 270° in the track colour), MUTE / SOLO toggles 52 × 44
// (LED accent / warn), a two-bar meter centred in the strip over y 356–560 with the fader marker drawn across the
// bars (hardware feedback: wider strips — the rail and the bus pair are compacted for them — meters aligned under
// the toggles and chip), and the track chip 44 × 32 at
// y 572. States: selected (one 1.6 lpx track-colour outline inside the strip), muted (rings, meter, chip
// 40 %, ✕ glyph on the chip), soloed, dimmed by another strip's solo, empty (chip dim), MIDI (no send ring).
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
struct ChannelStripModel
{
    enum class Variant : std::uint8_t
    {
        Mixer,
        Output
    };
    int track = 1;
    float levelDb[2] = {-120.0f, -120.0f}, peakDb[2] = {-120.0f, -120.0f};
    float faderDb = 0; // track volume in dB (fader marker)
    float pan = 0;     // −1 … +1
    float send01 = 0;  // 0 … 1
    bool mute = false, solo = false, selected = false;
    bool hasContent = true, otherSolo = false, midi = false;
    std::uint32_t colour = 0; // pad colour (0xAARRGGBB)
    std::uint32_t colourDim = 0;
    std::string_view outAssign = "--";
    Variant variant = Variant::Mixer;
};

class ChannelStrip
{
public:
    static constexpr float kX0 = 168, kPitch = 62, kW = 58;
    static constexpr float kBarsW = 18; // two 8 lpx bars on a 10 lpx pitch, centred in the strip
    static constexpr float kMeterTop = 356, kMeterBottom = 560;
    static constexpr RectF rect(int track1Based) noexcept
    {
        return {kX0 + kPitch * static_cast<float>(track1Based - 1), 152, kW, 456};
    }
    static constexpr RectF muteRect(int t) noexcept { return {rect(t).x, 252, kW, 44}; }
    static constexpr RectF soloRect(int t) noexcept { return {rect(t).x, 300, kW, 44}; }
    static constexpr RectF meterRect(int t) noexcept { return {rect(t).x, kMeterTop, kW, kMeterBottom - kMeterTop}; }

    static void draw(DrawList&, const UiContext&, const ChannelStripModel&);
    // Read-only position ring (MiniRing): 24 lpx surface-3 disc, 3 lpx arc from 135° over 270° (bipolar: from 12
    // o'clock), a caption letter in the centre.
    static void miniRing(DrawList&, const UiContext&, float cx, float cy, float value01, bool bipolar,
                         std::uint32_t colour, std::string_view letter, bool dim);
    // A circular arc stroke from angle a0 to a1 (radians, SVG orientation: 0 = +x, clockwise), as overlapping dots.
    static void arc(DrawList&, float cx, float cy, float r, float a0, float a1, float width, std::uint32_t argb);
};
} // namespace ui
