// ModuleCard (07-ui-design/screens/track-menu.md "Module slots"; 05-extensibility.md §3; UI-220…225, UID-650…666):
// one 184 × 152 card of the TRACK MENU chain at x = 48 + 200·(n−1), y 272 — title (`label`, centred at y + 22), a
// 152 × 88 thumbnail at (x + 16, y + 44) and the states rest / focused (accent ring + corner triangle) / disabled
// (surface-1, text-3) / empty (1 lpx dashed border outline) / locked (8 lpx mod corner badge, UID-658) / bypassed
// (thumbnail at 40 % + BYPASS badge). Thumbnails: WaveformThumb (min/max columns), EnvelopeThumb (AHR), EffectIcon
// (⊘ for NONE), LfoThumb (two cycles), KeyboardThumb, MidiPortThumb. Connectors: 16 × 2 border-strong at y 360.
#pragma once

#include "ui/widgets/EnvelopeEditor.h"
#include "ui/widgets/WaveformEditor.h"
#include "ui/widgets/Widget.h"

#include <cstdint>
#include <string_view>

namespace ui
{
struct ModuleCardModel
{
    enum class State : std::uint8_t
    {
        Rest,
        Focused,
        Disabled,
        Empty
    };
    enum class Thumb : std::uint8_t
    {
        None,
        Wave,
        Envelope,
        Effect,
        Lfo,
        Keys,
        Port
    };
    int slot = 0; // 0–5
    std::string_view title{};
    Thumb thumb = Thumb::None;
    std::string_view effect{}; // Thumb::Effect: the slot's effect id ("" / "none" = the ⊘ NONE glyph)
    State state = State::Rest;
    bool locked = false, bypassed = false, bypassable = false; // bypassable: BYPASS mode's dashed warn outline
    std::uint32_t colour = 0;
    const WaveOverview* wave = nullptr; // Wave: START…END of the sample
    double waveFrom = 0, waveTo = 0;
    float playhead = -1;      // Wave: 0–1 of the thumbnail, < 0 none
    EnvelopeModel envelope{}; // Envelope
    int port = 0;             // Port: CC page number
};

class ModuleCard
{
public:
    static constexpr float kY = 272, kW = 184, kH = 152;
    static constexpr RectF rect(int slot) noexcept { return {48 + 200.0f * static_cast<float>(slot), kY, kW, kH}; }
    static void draw(DrawList&, const UiContext&, const ModuleCardModel&);
    static void connector(DrawList&, const UiContext&, int slotLeft);
    // 1 lpx dashed outline of `r` (empty slots, BYPASS mode), dash 6 / gap 6.
    static void dashedRect(DrawList&, RectF r, std::uint32_t colour);
    // The accent corner triangle of a focused item that opens a screen.
    static void cornerTriangle(DrawList&, RectF r, std::uint32_t colour);
};
} // namespace ui
