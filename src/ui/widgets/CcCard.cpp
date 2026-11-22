#include "ui/widgets/CcCard.h"

#include "ui/theme/Draw.h"
#include "ui/widgets/ModuleCard.h"

#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
struct Name
{
    int cc;
    std::string_view a, b;
};
// The MIDI 1.0 control change names the cards show (a subset; others read `Controller`; instrument profiles
// replace them later, UID-810).
constexpr Name kNames[] = {{0, "Bank", "Select"},        {1, "Modulation", {}},       {2, "Breath", "Controller"},
                           {4, "Foot", "Controller"},    {5, "Portamento", "Time"},   {6, "Data", "Entry"},
                           {7, "Channel", "Volume"},     {8, "Balance", {}},          {10, "Pan", {}},
                           {11, "Expression", {}},       {12, "Effect", "Control 1"}, {13, "Effect", "Control 2"},
                           {64, "Sustain", "Pedal"},     {65, "Portamento", {}},      {66, "Sostenuto", {}},
                           {67, "Soft", "Pedal"},        {68, "Legato", {}},          {69, "Hold 2", {}},
                           {70, "Sound", "Variation"},   {71, "Harmonic", "Intensity"}, {72, "Release", "Time"},
                           {73, "Attack", "Time"},       {74, "Brightness", {}},      {75, "Decay", "Time"},
                           {76, "Vibrato", "Rate"},      {77, "Vibrato", "Depth"},    {78, "Vibrato", "Delay"},
                           {84, "Portamento", "Control"}, {91, "Reverb", "Send"},     {92, "Tremolo", "Depth"},
                           {93, "Chorus", "Send"},       {94, "Detune", {}},          {95, "Phaser", "Depth"}};
} // namespace

void CcCard::nameOf(int cc, std::string_view& line1, std::string_view& line2) noexcept
{
    line1 = {};
    line2 = {};
    if (cc == -1)
    {
        line1 = "Pitch";
        line2 = "Bend";
        return;
    }
    if (cc == -2)
    {
        line1 = "After";
        line2 = "Touch";
        return;
    }
    for (const auto& n : kNames)
        if (n.cc == cc)
        {
            line1 = n.a;
            line2 = n.b;
            return;
        }
    line1 = "Controller";
}

void CcCard::draw(DrawList& dl, const UiContext& ctx, const CcCardModel& m)
{
    const auto& p = ctx.palette;
    const auto r = card(m.index);
    dl.fill(r, p.surface2, tk::radiusM);
    char text[24];
    std::snprintf(text, sizeof text, "PARAMETER%d", (m.index & 7) + 1);
    dl.text(r.x + 100, r.y + 20, Align::Centre, TextStyleId::Caption, p.text2, text);
    dl.fill({r.x + 12, r.y + 40, 176, 268}, p.surface1, tk::radiusS);
    if (m.cc >= 0)
    {
        std::snprintf(text, sizeof text, "CC #%d", m.cc & 127);
        dl.text(r.x + 100, r.y + 96, Align::Centre, TextStyleId::ValueS, p.text1, text);
    }
    dl.text(r.x + 100, r.y + 164, Align::Centre, TextStyleId::Title, p.text1, m.name1);
    if (! m.name2.empty())
        dl.text(r.x + 100, r.y + 188, Align::Centre, TextStyleId::Title, p.text1, m.name2);
    if (m.locked)
        dl.stroke(r, p.mod, 1.5f, tk::radiusM);
    if (m.focused)
    {
        draw::focusRing(dl, p, r, tk::radiusM);
        ModuleCard::cornerTriangle(dl, r, p.accent);
    }
    // ACTIVE toggle (controls.md Toggle): surface-2, LED 168 × 4 at y + 8, label centred.
    const auto t = toggle(m.index);
    dl.fill(t, p.surface2, tk::radiusS);
    dl.fill({t.x + 16, t.y + 8, t.w - 32, 4}, m.active ? p.accent : p.borderStrong, 2);
    dl.text(t.x + t.w / 2, t.y + t.h / 2 + 4, Align::Centre, TextStyleId::Label, p.text1, "ACTIVE");
}
} // namespace ui
