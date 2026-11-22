#include "ui/screens/virtualpanel/PanelView.h"

#include "io/controllers/profiles/KeyRemap.h"
#include "ui/shell/Popup.h"
#include "ui/theme/Draw.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace ui::virtualpanel
{
namespace
{
namespace tk = ::theme::tokens;
using io::Control;
using io::controllers::LampStyle;
using io::controllers::PanelItem;
using io::controllers::PanelItemKind;
using io::controllers::VirtualPanel;

std::uint32_t opaque(std::uint32_t rgb) noexcept
{
    return 0xFF000000u | (rgb & 0xFFFFFFu);
}

// A lamp as drawn: its colour at its level over the unlit surface; `off` when dark.
std::uint32_t lampColour(const VirtualPanel::Lamp& l, std::uint32_t off) noexcept
{
    if (l.level == 0)
        return off;
    return theme::mix(off, opaque(l.rgb), static_cast<float>(l.level) / 255.0f);
}

bool lit(const VirtualPanel::Lamp& l) noexcept { return l.level > 40; }

RectF at(const io::controllers::PanelRect& r, float ox, float oy) noexcept { return {r.x + ox, r.y + oy, r.w, r.h}; }

// The keycap of a control with KEYS on (UID-934; virtual-panel-keys.svg): a 18 lpx cap, 8 lpx per character + 10.
void keycap(DrawList& dl, const tk::Palette& p, const PanelItem& it, float ox, float oy)
{
    std::string name;
    if (it.kind == PanelItemKind::Encoder)
        name = "\xE2\x86\x91\xE2\x86\x93"; // ↑↓ on the focused column (←/→ or Control + 1…6 focus it)
    else if (it.kind == PanelItemKind::Rotary)
        name = ", . \xE2\x8F\x8E";           // , . ⏎
    else if (it.control == Control::Shift)
        name = "\xE2\x87\xA7";                 // ⇧
    else if (it.kind == PanelItemKind::Knob || it.kind == PanelItemKind::Strip)
        return; // no keyboard map (UI-124)
    else
    {
        const auto k = io::controllers::keyRemap().keyFor(it.control);
        name = io::controllers::keyName(k.code, k.cls);
        if (it.control == Control::ExtCue || it.control == Control::ExtErase || it.control == Control::ExtAccent)
            name += " (HOLD)";
    }
    int chars = 0;
    for (const char c : name)
        chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    const float w = std::max(20.0f, 8.0f * static_cast<float>(chars) + 10.0f);
    const auto& r = it.rect;
    float x = r.cx() - w / 2, y = r.y + 20;
    switch (it.kind)
    {
    case PanelItemKind::Pad: x = r.x + r.w - 6 - w; y = r.y + 26; break;
    case PanelItemKind::Step: y = r.y + 14; break;
    case PanelItemKind::Encoder: x = r.cx() - 20; y = 94; break;
    case PanelItemKind::Rotary: x = r.cx() - 44; y = r.cy() + 40; break;
    default:
        if (it.lamp == LampStyle::Fill) // BAR keys
            y = r.y + 22;
        break;
    }
    const RectF cap {x + ox, y + oy, w, 18};
    dl.fill(cap, p.surface3, tk::radiusXs);
    dl.stroke(cap, p.borderStrong, 1, tk::radiusXs);
    dl.text(cap.x + w / 2, cap.y + 10, Align::Centre, TextStyleId::Caption, p.text1, name);
}
} // namespace

// A 270° knob arc of radius 24 (−135° … +135° from the top) as 3 lpx dots: the DrawList has no arcs.
void arc(DrawList& dl, float cx, float cy, float from, float to, std::uint32_t c)
{
    constexpr float kStep = 0.08f; // ≈ 2 lpx along r 24: reads as a stroke
    for (float a = from; a <= to + 1e-4f; a += kStep)
        dl.fill({cx + 24 * std::sin(a) - 1.5f, cy - 24 * std::cos(a) - 1.5f, 3, 3}, c, 1.5f);
}

void drawPanel(DrawList& dl, const VirtualPanel& panel, const tk::Palette& p, float ox, float oy, double nowMs)
{
    using S = io::controllers::PanelState;
    constexpr float kPi = 3.14159265f;
    if (panel.state() == S::Hidden)
        return;
    const bool expanded = panel.state() == S::Expanded;
    if (expanded)
        dl.fill({ox, oy, VirtualPanel::kWidth, VirtualPanel::kHeight}, p.bg);
    // Drawer bar.
    dl.fill({ox, oy, VirtualPanel::kWidth, VirtualPanel::kBarHeight}, p.surface1);
    draw::hline(dl, ox, ox + VirtualPanel::kWidth, oy + 32, p.border);
    dl.text(ox + 16, oy + 16, Align::Left, TextStyleId::Caption, p.text2, "CONTROL PANEL");
    for (int i = 0; i < 3; ++i)
        dl.fill({ox + 628, oy + 10 + 5.0f * static_cast<float>(i), 24, 2}, p.borderStrong, 1);
    const bool shift = panel.shiftShown();
    for (const auto& it : panel.items())
    {
        if (it.kind != PanelItemKind::PanelButton || it.button == io::controllers::PanelButton::None)
            continue;
        const bool on = it.button == io::controllers::PanelButton::Keys && panel.keycaps();
        dl.fill(at(it.rect, ox, oy), on ? p.accentWeak : p.surface2, tk::radiusXs);
        dl.text(it.rect.cx() + ox, it.rect.cy() + oy, Align::Centre, TextStyleId::Caption, p.text2, it.label);
    }
    if (! expanded)
        return;

    draw::hline(dl, ox + 16, ox + 1264, oy + 116, p.border);
    // Analog-filter box (behind its knobs), its caption on the top edge, the divider before HP CUTOFF.
    dl.stroke({ox + 680, oy + 124, 440, 76}, p.borderStrong, 1, tk::radiusM);
    dl.fill({ox + 850, oy + 118, 100, 12}, p.bg);
    dl.text(ox + 900, oy + 124, Align::Centre, TextStyleId::Caption, p.text2, "ANALOG FILTER");
    draw::vline(dl, ox + 984, oy + 134, oy + 190, p.border);
    dl.text(ox + 608, oy + 206, Align::Left, TextStyleId::Caption, p.text3, "SIXTEEN KEYS");

    char buf[16];
    for (const auto& it : panel.items())
    {
        const RectF r = at(it.rect, ox, oy);
        const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        const bool down = it.control != Control::Count && panel.pressed(it.control);
        const auto lamp = it.control != Control::Count ? panel.lamp(it.control) : VirtualPanel::Lamp {};
        const bool recent = it.control != Control::Count && panel.lastTurnMs(it.control) > 0
                         && nowMs - panel.lastTurnMs(it.control) < VirtualPanel::kRingMs;
        switch (it.kind)
        {
        case PanelItemKind::Encoder:
        {
            dl.fill(r, p.surface3, r.w / 2);
            dl.stroke(r, recent ? p.accent : p.borderStrong, 2, r.w / 2);
            const float a = static_cast<float>(panel.encoderAngle(it.control)) * 15.0f * kPi / 180.0f;
            dl.fill({cx + 14 * std::sin(a) - 3, cy - 14 * std::cos(a) - 3, 6, 6}, p.text1, 3);
            dl.text(cx + 40, oy + 94, Align::Left, TextStyleId::Caption, p.text3, it.label);
            break;
        }
        case PanelItemKind::Rotary:
        {
            dl.fill(r, p.surface3, r.w / 2);
            dl.stroke(r, recent ? p.accent : p.borderStrong, 2, r.w / 2);
            const RectF in {cx - 26, cy - 26, 52, 52};
            dl.fill(in, panel.pressed(Control::SelectorPush) ? p.surface3 : p.surface2, 26);
            dl.text(cx, cy + 1, Align::Centre, TextStyleId::Caption, p.text3, it.label);
            break;
        }
        case PanelItemKind::Knob:
        {
            dl.fill(r, p.surface3, r.w / 2);
            const float f = static_cast<float>(panel.knobValue(it.control)) / 1023.0f;
            arc(dl, cx, cy, -0.75f * kPi, 0.75f * kPi, p.borderStrong);
            if (f > 0)
                arc(dl, cx, cy, -0.75f * kPi, -0.75f * kPi + 1.5f * kPi * f, recent ? p.accent : p.text2);
            const float a = -0.75f * kPi + 1.5f * kPi * f; // the pointer: a 14 lpx line from the centre, as dots
            for (float d = 0; d <= 14.0f; d += 1.5f)
                dl.fill({cx + d * std::sin(a) - 1, cy - d * std::cos(a) - 1, 2, 2}, p.text1, 1);
            dl.text(cx, cy + 34, Align::Centre, TextStyleId::Caption, p.text2, it.label);
            break;
        }
        case PanelItemKind::Strip:
        {
            dl.fill(r, p.surface2, tk::radiusM);
            if (const int pos = panel.stripPosition(); pos >= 0)
                dl.fill({r.x + 8, r.y + r.h - 4 - static_cast<float>(pos) / 127.0f * (r.h - 8) - 2, 28, 4}, p.text2, 2);
            break;
        }
        case PanelItemKind::Lamp:
        {
            const bool dot = it.rect.w == it.rect.h;
            dl.fill(r, lampColour(lamp, p.surface3), dot ? r.w / 2 : std::min(r.w, r.h) / 2);
            if (it.control == Control::AfsOver)
                dl.text(cx, oy + 178, Align::Centre, TextStyleId::Caption, lit(lamp) ? p.warn : p.text3, it.label);
            else if (! it.label.empty())
                dl.text(ox + 110, cy, Align::Left, TextStyleId::Caption, lit(lamp) ? p.text1 : p.text3, it.label);
            break;
        }
        case PanelItemKind::Pad:
        case PanelItemKind::Step:
        case PanelItemKind::Key:
        {
            const bool fill = it.lamp == LampStyle::Fill;
            const float radius = it.kind == PanelItemKind::Pad ? tk::radiusM : tk::radiusS;
            const std::uint32_t base = down ? p.surface3 : p.surface2;
            dl.fill(r, fill ? lampColour(lamp, base) : base, radius);
            if (panel.latched(it.control))
            {
                dl.stroke(r, p.accent, 2, radius);
                dl.fill({r.x + r.w - 14, r.y + 4, 10, 10}, p.accent, 5); // the pin
            }
            if (it.kind == PanelItemKind::Pad)
                dl.text(r.x + 10, r.y + 12, Align::Left, TextStyleId::Caption, lit(lamp) ? p.onAccent : p.text3,
                        it.label);
            else if (it.kind == PanelItemKind::Step)
            {
                const int n = io::indexOf(it.control);
                std::snprintf(buf, sizeof buf, "%d", n);
                if ((n - 1) % 4 == 0)
                    dl.stroke({cx - 14, oy + 404, 28, 16}, p.text3, 1, 3);
                dl.text(cx, oy + 412, Align::Centre, TextStyleId::Caption, p.text2, buf);
            }
            else if (fill) // BAR keys
                dl.text(cx, cy + 1, Align::Centre, TextStyleId::Caption, lit(lamp) ? p.onAccent : p.text2, it.label);
            else
            {
                const bool legend = ! it.shiftLegend.empty() && (it.control == Control::Pattern || shift)
                                 && ! panel.keycaps();
                if (panel.keycaps()) // labels move to the top, the keycap takes the bottom (virtual-panel-keys.svg)
                    dl.text(cx, r.y + 15, Align::Centre, TextStyleId::Caption, p.text2, it.label);
                else
                    dl.text(cx, legend ? cy - 2 : cy + 3, Align::Centre, TextStyleId::Caption, p.text1, it.label);
                if (legend)
                    dl.text(cx, cy + 11, Align::Centre, TextStyleId::Caption, shift ? p.accent : p.text3,
                            it.shiftLegend);
            }
            if (it.lamp == LampStyle::Bar)
                dl.fill({r.x + 12, r.y + 5, r.w - 24, 3}, lampColour(lamp, p.borderStrong), 1.5f);
            break;
        }
        default: break;
        }
        if (panel.keycaps() && it.kind != PanelItemKind::Lamp && it.kind != PanelItemKind::PanelButton)
            keycap(dl, p, it, ox, oy);
    }
}

bool PanelHost::route(const io::ControlEvent& e, double nowMs)
{
    if (! touch::isTouch(e))
    {
        panel_.observe(e);
        return false;
    }
    const float x = touch::x(e), y = touch::y(e) - originY();
    const bool onPanel = panel_.height() > 0 && y >= 0;
    if (e.value == touch::kDown)
    {
        if (! onPanel)
            return false;
        tracking_ = panel_.pointerDown(0, x, y, {}, nowMs);
        return tracking_ || onPanel;
    }
    if (! tracking_)
        return false;
    if (e.value == touch::kDrag)
        panel_.pointerMove(0, x, y, nowMs);
    else
    {
        panel_.pointerUp(0, x, y, nowMs);
        tracking_ = false;
    }
    return true;
}

void PanelHost::frame(DrawList& dl, const theme::tk::Palette& p, double nowMs)
{
    const int toggles = io::controllers::keyRemap().panelToggles();
    if (toggles_ >= 0 && toggles != toggles_)
        panel_.toggle();
    toggles_ = toggles;
    panel_.tick(nowMs);
    drawPanel(dl, panel_, p, 0, originY(), nowMs);
}
} // namespace ui::virtualpanel
