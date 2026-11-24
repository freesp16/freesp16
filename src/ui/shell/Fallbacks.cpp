// Fallback screens: the reference navigation graph needs HOME and the fixed-area targets even when their screens are
// not built in. installFallbackScreens() registers a descriptor only for an id nobody registered, so a real screen's
// descriptor (ui/screens/home/, PROJECT …) wins and the fallback disappears without an edit. The fallback HOME body is
// the walking skeleton's track-tile grid + MASTER meter (07-ui-design/screens/home.md); the other fallbacks show their
// title on an empty content panel.
// Remove this file once every id below is registered by its screen.
#include "core/params/ParamRegistry.h"
#include "ui/shell/ColumnRules.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Shell.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Meter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;

// home.md "Knob strip": VOLUME, PAN, PITCH, TIME STRETCH, LOOP, TRIGGER MODE for the selected track.
constexpr StripSpec kHomeStrip{
    "TRACK {track}",
    {"track.volume", "track.pan", "playback.pitch", "playback.timeStretch", "playback.loop", "track.triggerMode"}};
constexpr ScreenSpec kFallbacks[] = {
    {.id = "home",
     .title = "HOME",
     .parent = {},
     .body = BodyTemplate::ContentMeter,
     .contextBar = ContextBarKind::Menu,
     .rail = {},
     .bodyComponent = "shell.FallbackHome",
     .strip = kHomeStrip},
    {.id = "track.menu",
     .title = "TRACK MENU",
     .parent = "home",
     .body = BodyTemplate::Full,
     .contextBar = ContextBarKind::TrackTypeBypass,
     .rail = {},
     .bodyComponent = "shell.Placeholder",
     .strip = {"TRACK MENU", {}}},
    {.id = "trackSetting",
     .title = "TRACK SETTING",
     .parent = "home",
     .body = BodyTemplate::Full,
     .contextBar = ContextBarKind::Menu,
     .rail = {},
     .bodyComponent = "shell.Placeholder",
     .strip = {"TRACK SETTING", {}}},
    {.id = "mixer",
     .title = "MIXER",
     .parent = "home",
     .body = BodyTemplate::Full,
     .contextBar = ContextBarKind::Menu,
     .rail = {},
     .bodyComponent = "shell.Placeholder",
     .strip = {"MIXER", {}}},
    {.id = "arranger",
     .title = "ARRANGER",
     .parent = "home",
     .body = BodyTemplate::Full,
     .contextBar = ContextBarKind::Menu,
     .rail = {},
     .bodyComponent = "shell.Placeholder",
     .strip = {"ARRANGER", {}}},
    {.id = "utility",
     .title = "UTILITY",
     .parent = "home",
     .body = BodyTemplate::Full,
     .contextBar = ContextBarKind::Menu,
     .rail = {},
     .bodyComponent = "shell.Placeholder",
     .strip = {"UTILITY", {}}},
    {.id = "project",
     .title = "PROJECT",
     .parent = "home",
     .body = BodyTemplate::Full,
     .contextBar = ContextBarKind::Menu,
     .rail = {},
     .bodyComponent = "shell.Placeholder",
     .strip = {"PROJECT", {}}},
};

// --- the skeleton's TrackTile and Meter (the real ones are ui/screens/home's; kept here only for the fallback HOME) -
void drawTile(DrawList& dl, const UiContext& ctx, RectF b, int t, const TrackSummary& s, bool selected, bool focused)
{
    const auto& p = ctx.palette;
    const auto colour = theme::trackColour(t);
    dl.fill(b, s.muted ? theme::withOpacity(p.surface2, 0.4f) : p.surface2, tk::radiusM);
    const double since = ctx.nowMs - s.lastHitMs;
    if (since >= 0 && since < tk::motionPadFlashMs) // hit: fill flashes to the track colour at 30 % and decays
        dl.fill(b, theme::withOpacity(colour, 0.3f * static_cast<float>(1.0 - since / tk::motionPadFlashMs)),
                tk::radiusM);
    if (s.soloed)
        dl.fill({b.x + 8, b.y, b.w - 16, 2}, p.warn, 1);
    char number[4];
    std::snprintf(number, sizeof number, "%d", t);
    dl.fill({b.x + 12, b.y + 12, 32, 24}, s.hasContent ? colour : theme::trackColour(t, true), tk::radiusXs);
    dl.text(b.x + 28, b.y + 25, Align::Centre, TextStyleId::Label, s.hasContent ? p.onAccent : p.text1, number);
    auto badge = [&](float x, std::string_view text, std::uint32_t fill, std::uint32_t ink, float w)
    {
        if (w <= 0)
            w = (ctx.measure ? ctx.measure(TextStyleId::Caption, text) : 8.0f * static_cast<float>(text.size())) + 16;
        dl.fill({x, b.y + 13, w, 22}, fill, tk::radiusXs);
        dl.text(x + w / 2, b.y + 25, Align::Centre, TextStyleId::Caption, ink, text);
        return x + w + 6;
    };
    if (! s.hasContent)
        badge(b.x + 52, "SMPL", p.surface1, p.text3, 0);
    else
    {
        float x = badge(b.x + 52, s.typeBadge, p.surface3, p.text1, 0);
        if (s.typeBadge == "SMPL")
        {
            x = badge(x, s.stretchBadge, p.surface1, p.text2, 0);
            badge(x, s.loopGlyph, p.surface1, colour, 32);
        }
        if (s.missing)
            glyph::draw(dl, glyph::Kind::Caution, b.x + 200, b.y + 16, tk::sizeIconS, p.warn);
        const int segs = s.levelDb <= -48 ? 0 : std::min(5, 1 + static_cast<int>((s.levelDb + 48.0f) / 10.0f));
        for (int k = 0; k < 5; ++k)
            dl.fill({b.x + 232 + 8.0f * static_cast<float>(k), b.y + 19, 5, 10}, k < segs ? colour : p.surface3, 1);
        dl.text(b.x + 16, b.y + 70, Align::Left, TextStyleId::BodyStrong, s.missing ? p.text3 : p.text1, s.name);
    }
    if (selected)
        dl.stroke({b.x + 1, b.y + 1, b.w - 2, b.h - 2}, colour, tk::borderTrackFrame, tk::radiusM);
    if (focused)
        draw::focusRing(dl, p, b, tk::radiusM);
}

// The MASTER column (Meter::drawColumn) from the UI state's master level and gain.
void drawMeter(DrawList& dl, const UiContext& ctx, float x, const UiState& st)
{
    MeterModel m;
    for (int ch = 0; ch < 2; ++ch)
    {
        m.db[ch] = st.masterDb[ch];
        m.peakDb[ch] = st.masterPeakDb[ch];
    }
    m.faderDb = st.masterGain > 0 ? 20.0f * std::log10(st.masterGain) : -120.0f;
    Meter::drawColumn(dl, ctx, x, m);
}

class FallbackHome final : public Widget
{
public:
    FallbackHome(const WidgetArgs& a) : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return L::bodyRects(BodyTemplate::ContentMeter).content; }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (state_ == nullptr)
            return;
        for (int t = 1; t <= 16; ++t)
            drawTile(dl, ctx, L::trackTile(t), t, state_->tracks[static_cast<std::size_t>(t - 1)],
                     t == state_->selectedTrack, t == state_->selectedTrack && ctx.focus.focused < 0);
        drawMeter(dl, ctx, L::bodyRects(BodyTemplate::ContentMeter).meter.x, *state_);
    }
    bool dirty(double nowMs) const override
    {
        if (state_ == nullptr)
            return false;
        for (const auto& t : state_->tracks)
            if (nowMs - t.lastHitMs < tk::motionPadFlashMs + 20)
                return true;
        return state_->masterDb[0] > -120.0f || state_->masterDb[1] > -120.0f;
    }
    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn) // selection follows the rotary, wraps 1 → 16
        {
            shell_->selectTrack(((state_->selectedTrack - 1 + e.value) % 16 + 16) % 16 + 1);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
            return shell_->open("track.menu"), true; // push = TRACK MENU of the selected track (UI-401)
        if (touch::isTouch(e) && e.value == touch::kDown)
            for (int t = 1; t <= 16; ++t)
                if (contains(L::trackTile(t), touch::x(e), touch::y(e)))
                {
                    if (t == state_->selectedTrack) // two-tap: a tap on the selected tile opens it (UID-155)
                        shell_->open("track.menu");
                    else
                        shell_->selectTrack(t);
                    return true;
                }
        return false;
    }

private:
    ShellApi* shell_;
    const UiState* state_;
};

class Placeholder final : public Widget
{
public:
    explicit Placeholder(const ScreenSpec& s) : title_(s.title) {}
    RectF bounds() const noexcept override { return L::bodyRects(BodyTemplate::Full).content; }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto r = bounds();
        dl.fill(r, ctx.palette.surface1, tk::radiusM);
        dl.text(r.x + r.w / 2, r.y + r.h / 2, Align::Centre, TextStyleId::Title, ctx.palette.text3, title_);
    }

private:
    std::string_view title_;
};

std::unique_ptr<Widget> makeFallbackHome(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<FallbackHome>(a);
}
std::unique_ptr<Widget> makePlaceholder(const ScreenSpec& s, const WidgetArgs&)
{
    return std::make_unique<Placeholder>(s);
}

// PITCH is disabled while TIME STRETCH = RESAMPLE (home.md, UI-003). Interim home of the rule until it is
// registered next to the playback parameters.
bool pitchDisabled(const ParamAccess& values, const core::ParamRegistry& params, std::uint16_t scope)
{
    const auto* stretch = params.find("playback.timeStretch");
    return stretch != nullptr && std::lround(values.get(*stretch, scope)) == 1;
}
} // namespace

void installFallbackScreens(ScreenRegistry& registry)
{
    for (const auto& s : kFallbacks)
        if (registry.find(s.id) == nullptr)
            registry.add(s);
}
} // namespace ui

APP_REGISTER_WIDGET("shell.FallbackHome", &ui::makeFallbackHome)
APP_REGISTER_WIDGET("shell.Placeholder", &ui::makePlaceholder)
APP_REGISTER_COLUMN_RULE("playback.pitch", &ui::pitchDisabled)
