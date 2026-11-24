// OUTPUT SETTING (07-ui-design/screens/mixer.md "Layout — OUTPUT SETTING", UID-558…563; 03-ui/screens.md
// OUTPUT SETTING UI-275…278; MIX-030/031/040, FX-012). Template rail+content+meters2, context bar menu (MIXER lit),
// breadcrumb HOME › MIXER › OUTPUT SETTING. The routing values are the routing.* parameters, in the document model
// (ui/shell/ModelParamAccess): every edit is undoable, saved, and reaches the engine through the bus hook.
//   - 16 ChannelStrips in the output variant (OUT ASSIGN badge at y 152; no S ring or toggles, so the P ring, meter and
//     chip keep MIXER's y — UID-551); the HEADPHONES meter (fader marker = mixer.headphoneVolume) and MASTER at the
//     right. The permanent note in rail slot 2 says bus FX process the master only (UI-278, not focusable); slot 1 is
//     the free extension slot (UID-023).
//   - ASSIGN MODE (routing.assignMode, per scene) and HP CUE (routing.headphoneCue) open ListPopups (UI-275); the cue
//     list holds only the entries valid in the current mode (UI-276); a mode change that invalidates the cue sets it
//     to MASTER with a Toast `HP CUE set to MASTER` (MIX-040, UID-562).
//   - Knobs VOLUME · PAN · OUT ASSIGN | HEADPHONES · — · MASTER (UID-558). OUT ASSIGN walks the mode's list (UI-277:
//     M + 6CH `--`, OUT3-4 … 8; 8CH OUT1-2 … 8, where `--` reads as OUT1-2). On a THRU track knob 5 binds the
//     THRU BYPASS (routing.thruBypass, MIX-051) — the only panel path to it; elsewhere it stays empty as drawn.
//   - Rotary: the strips 1 → 16 (no wrap); turning back from strip 1 hands the focus to the rail (HP CUE, ASSIGN
//     MODE), past the rail's end it returns to strip 1. A tap selects a strip; a drag on its meter is its fader.
//   - While a step is held, columns 4–6 are disabled (UID-563; the shell's lock rules do columns 1–2).
#include "core/util/GainLaw.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/ChannelStrip.h"
#include "ui/widgets/Meter.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;

constexpr RectF kInfo {16, 270, L::railNarrowW, 102}; // rail slot 2 on the narrow rail
constexpr int kRailAssign = 2, kRailCue = 3;
// OUT ASSIGN / HP CUE entries per ASSIGN MODE (03-ui/enumerations.md "Routing and outputs"; catalogue order).
constexpr int kOutM6[] = {0, 2, 3, 4, 7, 8, 9, 10, 11, 12}, kOut8[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
constexpr int kCueM6[] = {0, 2, 3, 4, 7, 8, 9, 10, 11, 12}, kCue8[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

constexpr StripSpec kStrip {"TRACK {track}",
                            {"track.volume", "track.pan", "routing.outAssign", "mixer.headphoneVolume", "",
                             "mixer.masterVolume"},
                            1u << 3};
constexpr StripSpec kThruStrip {"TRACK {track}",
                                {"track.volume", "track.pan", "routing.outAssign", "mixer.headphoneVolume",
                                 "routing.thruBypass", "mixer.masterVolume"},
                                1u << 3};

float volumeDb(float index) noexcept
{
    const double db = core::GainLaw::decibels(static_cast<int>(std::lround(index)));
    return std::isinf(db) ? -120.0f : static_cast<float>(db);
}

class OutputBody final : public Widget, public RailOwner, public StripOwner
{
public:
    explicit OutputBody(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    int assignMode() const noexcept { return shell_ != nullptr ? iv("routing.assignMode") & 1 : 0; }
    static std::span<const int> outList(int mode) noexcept
    {
        return mode == 0 ? std::span<const int>(kOutM6) : std::span<const int>(kOut8);
    }
    static std::span<const int> cueList(int mode) noexcept
    {
        return mode == 0 ? std::span<const int>(kCueM6) : std::span<const int>(kCue8);
    }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        rebind();
        drawInfo(dl, ctx);
        for (int t = 1; t <= 16; ++t)
            ChannelStrip::draw(dl, ctx, strip(t));
        // HP (as in HP CUE): HEADPHONES / PHONES do not fit the compact pair's 48 lpx pitch (bars only, no dB scale).
        Meter::drawBusPair(dl, ctx, "HP", bus(ctx, "mixer.headphoneVolume"), "MASTER",
                           bus(ctx, "mixer.masterVolume"));
    }

    bool dirty(double) const override
    {
        const auto& f = m1::feedsOf(shell_);
        if (f.master().db[0] > -120.0f)
            return true;
        for (int t = 1; t <= 16; ++t)
            if (f.track(t).db[0] > -120.0f || f.track(t).db[1] > -120.0f)
                return true;
        return false;
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            const int t = state_->selectedTrack + e.value;
            if (t < 1)
                return false; // back into the rail (HP CUE)
            shell_->selectTrack(std::min(t, 16));
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
            return true; // no second level on a strip: a push never leaves the screen (mixer.md "Interactions")
        if (touch::isTouch(e))
            return touchAt(e);
        return false;
    }

    // --- RailOwner ---------------------------------------------------------------------------------------------------
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (shell_ == nullptr)
            return true;
        if (index == kRailAssign)
            assignModePopup();
        else if (index == kRailCue)
            cuePopup();
        return true;
    }
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        if (index == kRailAssign)
            st.value = label("routing.assignMode");
        else if (index == kRailCue)
            st.value = label("routing.headphoneCue");
    }
    bool railFocusEnter(int detents) override
    {
        if (shell_ == nullptr)
            return false;
        shell_->selectTrack(std::clamp(detents, 1, 16));
        return true;
    }

    // --- StripOwner --------------------------------------------------------------------------------------------------
    void stripColumn(int column0, ParamColumnModel& c) const override
    {
        if (state_ != nullptr && state_->stepHeld > 0 && column0 >= 3)
            c.disabled = true; // UID-563: the global columns are not lockable
        if (column0 == 2 && c.spec != nullptr)
        {
            c.stepped = true;
            const int v = iv("routing.outAssign", state_->selectedTrack);
            if (assignMode() == 1 && v == 0)
                c.valueText = "OUT1-2"; // 8CH: "--" is OUT1-2 (our choice)
        }
    }
    bool stripTurn(int column0, const core::ParamSpec&, int detents, bool) override
    {
        if (column0 != 2 || shell_ == nullptr || state_ == nullptr)
            return false;
        const int t = state_->selectedTrack;
        const auto list = outList(assignMode());
        int v = iv("routing.outAssign", t);
        if (assignMode() == 1 && v == 0)
            v = 1;
        int i = 0;
        while (i < static_cast<int>(list.size()) && list[static_cast<std::size_t>(i)] < v)
            ++i;
        i = std::clamp(i + detents, 0, static_cast<int>(list.size()) - 1);
        m1::setTrackParam(*shell_, "routing.outAssign", t, static_cast<float>(list[static_cast<std::size_t>(i)]));
        return true;
    }

private:
    int iv(std::string_view id, int track = 1) const noexcept
    {
        return static_cast<int>(std::lround(m1::trackParam(*shell_, id, track)));
    }
    std::string_view label(std::string_view id) const noexcept
    {
        return shell_ != nullptr ? m1::trackLabel(*shell_, id, state_ != nullptr ? state_->selectedTrack : 1)
                                 : std::string_view {};
    }
    void rebind() const
    {
        const bool thru = m1::trackMode(*shell_, state_->selectedTrack) == 2;
        if (thru == thruBound_ && bound_)
            return;
        thruBound_ = thru;
        bound_ = true;
        shell_->bindStrip(thru ? &kThruStrip : &kStrip);
    }

    ChannelStripModel strip(int t) const noexcept
    {
        ChannelStripModel m;
        m.track = t;
        m.variant = ChannelStripModel::Variant::Output;
        auto& sh = *shell_;
        const auto& s = state_->tracks[static_cast<std::size_t>(t - 1)];
        const int colour = m1::padColour(sh, t);
        m.colour = theme::trackColour(colour);
        m.colourDim = theme::trackColour(colour, true);
        m.hasContent = s.hasContent;
        m.selected = t == state_->selectedTrack;
        m.faderDb = volumeDb(m1::trackParam(sh, "track.volume", t, 100));
        m.pan = m1::trackParam(sh, "track.pan", t) / 64.0f;
        m.mute = m1::trackFlag(sh, "track.mute", t) || s.muted;
        const int mode = m1::trackMode(sh, t);
        m.midi = mode == 1 || mode == 3;
        // The badge carries the short form (`3-4`, `5`; mixer.md "ChannelStrip"), the knob the full one (`OUT3-4`).
        const int out = iv("routing.outAssign", t);
        std::string_view l = assignMode() == 1 && out == 0 ? std::string_view("OUT1-2")
                                                           : m1::trackLabel(sh, "routing.outAssign", t);
        if (l.substr(0, 3) == "OUT")
            l.remove_prefix(3);
        m.outAssign = l.empty() ? std::string_view("--") : l;
        const auto& lv = m1::feedsOf(shell_).track(t);
        for (int ch = 0; ch < 2; ++ch)
        {
            m.levelDb[ch] = lv.db[ch];
            m.peakDb[ch] = lv.peakDb[ch];
        }
        return m;
    }

    void drawInfo(DrawList& dl, const UiContext& ctx) const
    {
        const auto& p = ctx.palette;
        dl.fill(kInfo, p.surface1, tk::radiusM);
        dl.stroke(kInfo, p.border, tk::borderHairline, tk::radiusM);
        static constexpr std::string_view lines[] = {"Send and master", "FX process the", "master only.",
                                                     "Direct outputs", "are dry."};
        for (int i = 0; i < 5; ++i)
            dl.text(28, 285 + 18.0f * static_cast<float>(i), Align::Left, TextStyleId::Body, p.text2, lines[i]);
    }

    // The HEADPHONES meter shows the master feed (the engine publishes no headphone level; MeterFeed is frozen) with
    // the headphone level's fader marker; MASTER the master feed with mixer.masterVolume's.
    MeterModel bus(const UiContext& ctx, std::string_view level) const
    {
        auto m = Meter::bus("master", &m1::feedsOf(shell_), state_, ctx.nowMs);
        m.faderDb = volumeDb(m1::trackParam(*shell_, level, 1, 100));
        return m;
    }

    void assignModePopup()
    {
        ListRequest req;
        req.title = "ASSIGN MODE";
        const auto* spec = shell_->params().find("routing.assignMode");
        if (spec == nullptr)
            return;
        for (std::size_t k = 0; k < spec->enumLabels.size(); ++k)
            req.items.push_back({spec->enumLabels[k], {}, {}, static_cast<int>(k)});
        req.current = assignMode();
        req.onChoose = [this](int, const ListItem& it)
        {
            const int mode = it.value & 1;
            m1::setTrackParam(*shell_, "routing.assignMode", 1, static_cast<float>(mode));
            // MIX-040 / UI-276: a cue that the new mode does not offer becomes MASTER.
            const int cue = iv("routing.headphoneCue");
            const auto valid = cueList(mode);
            if (std::find(valid.begin(), valid.end(), cue) == valid.end())
            {
                m1::setTrackParam(*shell_, "routing.headphoneCue", 1, 0.0f);
                ToastRequest t;
                t.text = "HP CUE set to MASTER";
                shell_->showToast(std::move(t));
            }
        };
        shell_->showList(std::move(req));
    }

    void cuePopup()
    {
        const auto* spec = shell_->params().find("routing.headphoneCue");
        if (spec == nullptr)
            return;
        ListRequest req;
        req.title = "HP CUE";
        const int cue = iv("routing.headphoneCue");
        for (const int k : cueList(assignMode()))
        {
            if (k == cue)
                req.current = static_cast<int>(req.items.size());
            req.items.push_back({spec->enumLabels[static_cast<std::size_t>(k)], {}, {}, k});
        }
        req.onChoose = [this](int, const ListItem& it)
        { m1::setTrackParam(*shell_, "routing.headphoneCue", 1, static_cast<float>(it.value)); };
        shell_->showList(std::move(req));
    }

    bool touchAt(const io::ControlEvent& e)
    {
        const float x = touch::x(e), y = touch::y(e);
        if (e.value == touch::kUp)
        {
            dragTrack_ = 0;
            return true;
        }
        if (e.value == touch::kDrag && dragTrack_ > 0)
        {
            setFaderFromY(dragTrack_, y);
            return true;
        }
        if (e.value != touch::kDown)
            return false;
        for (int t = 1; t <= 16; ++t)
        {
            const auto r = ChannelStrip::rect(t);
            if (! contains({r.x - 2, r.y, ChannelStrip::kPitch, r.h}, x, y))
                continue;
            shell_->selectTrack(t);
            if (contains(ChannelStrip::meterRect(t), x, y))
            {
                dragTrack_ = t; // the fader follows the finger (UID-561)
                setFaderFromY(t, y);
            }
            return true;
        }
        return false;
    }
    void setFaderFromY(int t, float y)
    {
        int best = 100;
        float bestD = 1e9f;
        for (int i = 0; i <= 127; ++i)
        {
            const float d = std::fabs(
                meterY(volumeDb(static_cast<float>(i)), ChannelStrip::kMeterTop, ChannelStrip::kMeterBottom) - y);
            if (d < bestD)
            {
                bestD = d;
                best = i;
            }
        }
        m1::setTrackParam(*shell_, "track.volume", t, static_cast<float>(best));
    }

    ShellApi* shell_;
    UiState* state_;
    int dragTrack_ = 0;
    mutable bool thruBound_ = false, bound_ = false;
};

std::unique_ptr<Widget> makeOutput(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<OutputBody>(a);
}

// Slots 1–2 are holes (the extension slot and the info note the body draws); ASSIGN MODE and HP CUE at the bottom.
constexpr RailItemSpec kRail[] = {{RailItemSpec::Kind::Button, ""},
                                  {RailItemSpec::Kind::Button, ""},
                                  {RailItemSpec::Kind::Option, "ASSIGN MODE", {}, "routing.assignMode"},
                                  {RailItemSpec::Kind::Option, "HP CUE", {}, "routing.headphoneCue"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("OutputStrips", &ui::makeOutput)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "mixer.output",
                                    .title = "OUTPUT SETTING",
                                    .parent = "mixer",
                                    .body = ui::BodyTemplate::RailContentMeters2,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = ui::kRail,
                                    .bodyComponent = "OutputStrips",
                                    .strip = ui::kStrip}))
