// MIXER and OUTPUT SETTING (07-ui-design/screens/mixer.md; UI-270…274, UID-550…563). Template rail+content+meters2,
// context bar menu (MIXER selected): 16 ChannelStrips at 224 + 52·(n−1), SEND and MASTER meters at x 1080 / 1176 and
// the rail ANALOG FILTER SETTING (OptionSelector) · FX SETTING (RailCard over slots 2–3: MODE and TYPE zones) ·
// OUTPUT SETTING. The rail is the shell's (ui/shell/RailOwner.h): the FX SETTING card is two rail items (MODE, TYPE)
// with their own rects, drawn by this body as one card; the selectors run this screen's popups. The rotary walks the
// rail, then past its end the strips (railFocusEnter); turning back from strip 1 returns to the rail's last item.
// Selection = the rotary focus on this screen: strip columns 1–3 (VOLUME, PAN, SEND AMOUNT) follow the selected
// track, 4–6 (HEADPHONES, SEND, MASTER) are global (separator before column 4). MUTE/SOLO toggle per strip; a drag on
// a strip's meter moves its fader (UID-561, UI-209). ANALOG FILTER SETTING and the FX zones show their first rows /
// values; the FX parts are the effect screens'.
// ANALOG FILTER SETTING (UID-556/557): the popup's rows are the afs.* parameters — INSERT TRACK, INSERT ORDER (disabled
// while INSERT TRACK = MASTER), ACTIVE, PROCESSOR (EXTERNAL opens the PROCESSOR page `mixer.afs`), DRIVE, LP CUTOFF,
// RESONANCE, HP CUTOFF; the rail value reads the insert track (` · OFF` while ACTIVE is off). OUTPUT SETTING is
// ui/screens/output/.
#include "core/util/GainLaw.h"
#include "ui/screens/effects/FxCatalogue.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/ChannelStrip.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/OptionSelector.h"

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

// The FX SETTING card over rail slots 2–3 and its MODE / TYPE zones; OUTPUT SETTING's info box (rail slot 2).
constexpr RectF kFxCard{16, 270, 192, 220}, kFxMode{16, 296, 192, 72}, kFxType{16, 372, 192, 118};
constexpr std::string_view kFxModes[] = {"BOTH", "SEND", "MASTER"};
constexpr std::string_view kAnalogValues[] = {"MASTER",   "TRACK 1",  "TRACK 2",  "TRACK 3",  "TRACK 4",  "TRACK 5",
                                              "TRACK 6",  "TRACK 7",  "TRACK 8",  "TRACK 9",  "TRACK 10", "TRACK 11",
                                              "TRACK 12", "TRACK 13", "TRACK 14", "TRACK 15", "TRACK 16"};
constexpr std::string_view kAnalogOff[] = {
    "MASTER \xC2\xB7 OFF",   "TRACK 1 \xC2\xB7 OFF",  "TRACK 2 \xC2\xB7 OFF",  "TRACK 3 \xC2\xB7 OFF",
    "TRACK 4 \xC2\xB7 OFF",  "TRACK 5 \xC2\xB7 OFF",  "TRACK 6 \xC2\xB7 OFF",  "TRACK 7 \xC2\xB7 OFF",
    "TRACK 8 \xC2\xB7 OFF",  "TRACK 9 \xC2\xB7 OFF",  "TRACK 10 \xC2\xB7 OFF", "TRACK 11 \xC2\xB7 OFF",
    "TRACK 12 \xC2\xB7 OFF", "TRACK 13 \xC2\xB7 OFF", "TRACK 14 \xC2\xB7 OFF", "TRACK 15 \xC2\xB7 OFF",
    "TRACK 16 \xC2\xB7 OFF"};
enum Zone : int
{
    AnalogFilter = 0,
    FxMode,
    FxType,
    OutputSetting,
    Strips
};

float volumeDb(float index) noexcept
{
    const double db = core::GainLaw::decibels(static_cast<int>(std::lround(index)));
    return std::isinf(db) ? -120.0f : static_cast<float>(db);
}

// Rail indices (the descriptor below).
constexpr int kRailAnalog = 0, kRailMode = 1, kRailType = 2, kRailOutput = 3;

class MixerBody final : public Widget, public RailOwner
{
public:
    explicit MixerBody(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    ChannelStripModel strip(int t) const noexcept
    {
        ChannelStripModel m;
        m.track = t;
        m.variant = ChannelStripModel::Variant::Mixer;
        if (shell_ == nullptr || state_ == nullptr)
            return m;
        auto& sh = *shell_;
        const auto& s = state_->tracks[static_cast<std::size_t>(t - 1)];
        const int colour = m1::padColour(sh, t);
        m.colour = theme::trackColour(colour);
        m.colourDim = theme::trackColour(colour, true);
        m.hasContent = s.hasContent;
        m.selected = t == state_->selectedTrack;
        m.faderDb = volumeDb(m1::trackParam(sh, "track.volume", t, 100));
        m.pan = m1::trackParam(sh, "track.pan", t) / 64.0f;
        m.send01 = m1::trackParam(sh, "track.sendAmount", t) / 127.0f;
        m.mute = m1::trackFlag(sh, "track.mute", t) || s.muted;
        m.solo = m1::trackFlag(sh, "track.solo", t) || s.soloed;
        m.otherSolo = anySolo_ && ! m.solo;
        const int mode = m1::trackMode(sh, t);
        m.midi = mode == 1 || mode == 3;
        auto out = m1::trackLabel(sh, "routing.outAssign", t); // the short form (`3-4`) on the strip
        if (out.substr(0, 3) == "OUT")
            out.remove_prefix(3);
        m.outAssign = out.empty() ? std::string_view("--") : out;
        const auto& lv = m1::feedsOf(shell_).track(t);
        for (int ch = 0; ch < 2; ++ch)
        {
            m.levelDb[ch] = lv.db[ch];
            m.peakDb[ch] = lv.peakDb[ch];
        }
        return m;
    }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        anySolo_ = false;
        if (shell_ != nullptr)
            for (int t = 1; t <= 16; ++t)
                anySolo_ |=
                    m1::trackFlag(*shell_, "track.solo", t) || state_->tracks[static_cast<std::size_t>(t - 1)].soloed;
        for (int t = 1; t <= 16; ++t)
            ChannelStrip::draw(dl, ctx, strip(t));
        drawBus(dl, ctx, 1080, "SEND", "send");
        drawBus(dl, ctx, 1176, "MASTER", "master");
    }

    bool dirty(double nowMs) const override
    {
        (void)nowMs;
        const auto& f = m1::feedsOf(shell_);
        if (f.master().db[0] > -120.0f || f.send().db[0] > -120.0f)
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
        // The body holds the focus on the strips (the shell's rail holds it otherwise): the rotary walks the strips,
        // turning back from strip 1 hands the focus to the rail's last item (the shell, on false).
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            const int t = state_->selectedTrack + e.value;
            if (t < 1)
                return false;
            shell_->selectTrack(std::min(t, 16));
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
            return true;
        if (touch::isTouch(e))
            return touchAt(e);
        return false;
    }

    // --- RailOwner ---------------------------------------------------------------------------------------------------
    bool railActivate(int index, const RailItemSpec&, float, float y) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (zoneOf(index) == FxType && y >= 0) // a tap on a TYPE row opens the bus page on that row's slot
        {
            openBusPage(busRowAt(y));
            return true;
        }
        activate(zoneOf(index));
        return true;
    }
    // FX SETTING (FX-008, UI-267): the scene's FX mode and the bus slots, through the FX slot access (the project).
    int fxMode() const
    {
        return state_ != nullptr ? static_cast<int>(fx::environment().slotAccess().mode(state_->scene)) : 0;
    }
    std::string_view busEffectName(fx::Slot slot) const
    {
        const auto& env = fx::environment();
        const auto id = env.slotAccess().effect(slot, state_->scene, state_->selectedTrack);
        const auto* d = fx::isNone(id) ? nullptr : fx::findEffect(env.effectRegistry(), id);
        return d != nullptr ? d->name : std::string_view("NONE");
    }
    // The bus a TYPE row stands for: SEND / MASTER in mode BOTH (top / bottom row), the running one otherwise.
    fx::Slot busRowAt(float y) const
    {
        const int mode = fxMode();
        if (mode == 1)
            return fx::Slot::Send;
        if (mode == 2)
            return fx::Slot::Master;
        return y < kFxCard.y + 130 + 42 ? fx::Slot::Send : fx::Slot::Master;
    }
    void openBusPage(fx::Slot slot)
    {
        const int mode = fxMode(); // a parity mode shows its running bus (the page's segment switches both)
        fx::busPage() = mode == 1 ? fx::Slot::Send : mode == 2 ? fx::Slot::Master : slot;
        shell_->open("mixer.busFx");
    }
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        switch (zoneOf(index))
        {
        case AnalogFilter:
        {
            const int k = std::clamp(static_cast<int>(std::lround(afs("afs.insertTrack"))), 0, 16);
            st.value = afs("afs.active") >= 0.5f ? kAnalogValues[k] : kAnalogOff[k];
            break;
        }
        default:
            break;
        }
    }
    bool railRect(int index, const RailItemSpec&, RectF& r) const override
    {
        if (index == kRailMode)
            r = kFxMode;
        else if (index == kRailType)
            r = kFxType;
        else
            return false;
        return true;
    }
    bool railDraw(int index, const RailItemSpec&, const RailItemState& st, DrawList& dl,
                  const UiContext& ctx) const override
    {
        if (index != kRailMode && index != kRailType)
            return false;
        if (index == kRailMode)
            drawFxCard(dl, ctx);
        if (st.focused)
            draw::focusRing(dl, ctx.palette, index == kRailMode ? kFxMode : kFxType, tk::radiusM);
        return true;
    }
    bool railFocusEnter(int detents) override
    {
        if (shell_ == nullptr)
            return false;
        shell_->selectTrack(std::clamp(detents, 1, 16)); // past the rail's end: strip 1, then on
        return true;
    }

private:
    float afs(std::string_view id) const noexcept
    {
        return shell_ != nullptr ? m1::trackParam(*shell_, id, 1) : 0.0f;
    }
    int zoneOf(int railIndex) const noexcept
    {
        switch (railIndex)
        {
        case kRailAnalog:
            return AnalogFilter;
        case kRailMode:
            return FxMode;
        case kRailType:
            return FxType;
        case kRailOutput:
            return OutputSetting;
        default:
            return Strips;
        }
    }

    void drawFxCard(DrawList& dl, const UiContext& ctx) const
    {
        const auto& p = ctx.palette;
        // FX SETTING RailCard (mixer.md "RailCard"): MODE zone + TYPE zone with one row per running bus slot.
        const auto c = kFxCard;
        dl.fill(c, p.surface2, tk::radiusM);
        dl.text(c.x + 16, c.y + 18, Align::Left, TextStyleId::Caption, p.text2, "FX SETTING");
        dl.text(c.x + 16, c.y + 46, Align::Left, TextStyleId::Caption, p.text2, "MODE");
        dl.fill({c.x + 12, c.y + 56, c.w - 24, 36}, p.surface1, tk::radiusS);
        const int mode = fxMode();
        dl.text(c.x + 24, c.y + 74, Align::Left, TextStyleId::BodyStrong, p.text1, kFxModes[mode]);
        glyph::draw(dl, glyph::Kind::ChevronDown, c.x + c.w - 36, c.y + 66, tk::sizeIconS, p.text3);
        draw::hline(dl, c.x + 12, c.x + c.w - 12, c.y + 102, p.border);
        dl.text(c.x + 16, c.y + 116, Align::Left, TextStyleId::Caption, p.text2, "TYPE");
        dl.text(c.x + c.w - 20, c.y + 116, Align::Centre, TextStyleId::Title, p.text3, "\xE2\x80\xBA");
        // The bus slots' effects (hardware feedback: the rows were fixed text and TYPE opened no page).
        const struct
        {
            std::string_view slot, fx;
        } rows[] = {{"SEND", busEffectName(fx::Slot::Send)}, {"MASTER", busEffectName(fx::Slot::Master)}};
        int row = 0;
        for (int i = 0; i < 2; ++i)
        {
            if ((mode == 1 && i == 1) || (mode == 2 && i == 0))
                continue;
            const float yy = c.y + 130 + 42.0f * static_cast<float>(row++);
            dl.fill({c.x + 12, yy, c.w - 24, 36}, p.surface1, tk::radiusS);
            dl.text(c.x + 24, yy + 18, Align::Left, TextStyleId::Caption, p.text3, rows[i].slot);
            dl.text(c.x + 92, yy + 18, Align::Left, TextStyleId::BodyStrong, rows[i].fx == "NONE" ? p.text2 : p.text1,
                    rows[i].fx);
        }
    }

    void drawBus(DrawList& dl, const UiContext& ctx, float x, std::string_view caption, std::string_view binds) const
    {
        const auto& p = ctx.palette;
        constexpr float top = 184, bottom = 600;
        dl.text(x + 44, 160, Align::Centre, TextStyleId::Caption, p.text2, caption);
        static constexpr struct
        {
            float db;
            const char* text;
        } labels[] = {{3, "3"}, {0, "0"}, {-10, "-10"}, {-20, "-20"}, {-30, "-30"}, {-40, "-40"}, {-50, "-50"}};
        for (const auto& l : labels)
            dl.text(x + 86, meterY(l.db, top, bottom), Align::Right, TextStyleId::Caption, p.text3, l.text);
        dl.text(x + 86, bottom + 6, Align::Right, TextStyleId::Caption, p.text3, "\xE2\x88\x92\xE2\x88\x9E");
        auto m = Meter::bus(binds == "headphones" ? std::string_view("master") : binds, &m1::feedsOf(shell_), state_,
                            ctx.nowMs);
        const char* id = binds == "send"     ? "mixer.sendReturnVolume"
                         : binds == "master" ? "mixer.masterVolume"
                                             : "mixer.headphoneVolume";
        m.faderDb = shell_ != nullptr ? volumeDb(m1::trackParam(*shell_, id, 1, 100)) : 0.0f;
        Meter::drawBars(dl, ctx, x + 12, top, bottom, 10, m);
    }

    // Focus order (the shell's rail, then the strips): ANALOG FILTER → MODE → TYPE → OUTPUT SETTING → strips 1 … 16
    // (mixer.md "RailCard").
    void activate(int zone)
    {
        switch (zone)
        {
        case AnalogFilter:
            analogFilterPopup();
            break;
        case FxMode:
        {
            ListRequest req;
            req.title = "FX MODE";
            for (int i = 0; i < 3; ++i)
                req.items.push_back({kFxModes[i], {}, {}, i});
            req.current = fxMode();
            req.onChoose = [this](int, const ListItem& it) // SetFxMode (FX-008): the project's FX mode, undoable
            {
                fx::environment().slotAccess().setMode(state_->scene, static_cast<fx::FxMode>(it.value));
            };
            shell_->showList(std::move(req));
            break;
        }
        case FxType:
            openBusPage(fx::busPage());
            break;
        case OutputSetting:
            shell_->open("mixer.output");
            break;
        default:
            break;
        }
    }

    // ANALOG FILTER SETTING (mixer.md "Popups"; UID-556/557): the afs.* parameters. Enum rows
    // carry the ParamSpec's labels; DRIVE … HP CUTOFF are numeric rows stepped by their ParamSpecs. Each confirmed row
    // is written through the shell's values (the engine follows); PROCESSOR › EXTERNAL opens the PROCESSOR page
    // (`mixer.afs`: the external loop's MEASURE and latency, AFS-005).
    void analogFilterPopup()
    {
        static constexpr std::string_view kRows[] = {"afs.insertTrack", "afs.insertOrder", "afs.active",
                                                     "afs.processor",   "afs.drive",       "afs.lpCutoff",
                                                     "afs.resonance",   "afs.hpCutoff"};
        SettingRequest req;
        req.title = "ANALOG FILTER SETTING";
        const auto& params = shell_->params();
        const bool master = std::lround(afs("afs.insertTrack")) == 0;
        for (const auto id : kRows)
        {
            const auto* spec = params.find(id);
            if (spec == nullptr)
                continue;
            SettingRow row;
            row.label = spec->label;
            const float v = afs(id);
            if (spec->type == core::ParamType::Enum || spec->type == core::ParamType::Bool)
            {
                for (const auto l : spec->enumLabels)
                    row.options.push_back(l);
                if (row.options.empty())
                    row.options = {"OFF", "ON"};
                row.current = static_cast<int>(std::lround(v - spec->minValue));
            }
            else
            {
                row.spec = spec;
                row.value = v;
            }
            row.disabled = id == std::string_view("afs.insertOrder") && master;
            req.rows.push_back(std::move(row));
        }
        req.onCommit = [this](int row, int option, float value)
        {
            if (row < 0 || row >= static_cast<int>(std::size(kRows)) || shell_ == nullptr)
                return;
            const auto* spec = shell_->params().find(kRows[row]);
            if (spec == nullptr)
                return;
            const bool numeric = spec->type != core::ParamType::Enum && spec->type != core::ParamType::Bool;
            m1::setTrackParam(*shell_, kRows[row], 1, numeric ? value : spec->minValue + static_cast<float>(option));
            if (kRows[row] == std::string_view("afs.processor") && option == 1)
                shell_->open("mixer.afs"); // the external loop's page (AFS-005)
        };
        shell_->showSetting(std::move(req));
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
            if (contains(ChannelStrip::muteRect(t), x, y))
                m1::setTrackParam(*shell_, "track.mute", t, m1::trackFlag(*shell_, "track.mute", t) ? 0.0f : 1.0f);
            else if (contains(ChannelStrip::soloRect(t), x, y))
                m1::setTrackParam(*shell_, "track.solo", t, m1::trackFlag(*shell_, "track.solo", t) ? 0.0f : 1.0f);
            else if (contains(ChannelStrip::meterRect(t), x, y))
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
    mutable bool anySolo_ = false;
};

std::unique_ptr<Widget> makeMixer(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<MixerBody>(a);
}

constexpr StripSpec kMixerStrip{"TRACK {track}",
                                {"track.volume", "track.pan", "track.sendAmount", "mixer.headphoneVolume",
                                 "mixer.sendReturnVolume", "mixer.masterVolume"},
                                1u << 3};
// The rail: the selectors run this screen's popups (railActivate); FX SETTING's MODE and TYPE zones are two items
// drawn as one card.
constexpr RailItemSpec kMixerRail[] = {{RailItemSpec::Kind::Option, "ANALOG FILTER SETTING"},
                                       {RailItemSpec::Kind::Option, "FX MODE"},
                                       {RailItemSpec::Kind::Button, "FX TYPE"},
                                       {RailItemSpec::Kind::Button, "OUTPUT SETTING", "mixer.output"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("ChannelStrip", &ui::makeMixer)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "mixer",
                                    .title = "MIXER",
                                    .parent = "home",
                                    .body = ui::BodyTemplate::RailContentMeters2,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = ui::kMixerRail,
                                    .bodyComponent = "ChannelStrip",
                                    .strip = ui::kMixerStrip}))
// OUTPUT SETTING (mixer.output): ui/screens/output/OutputScreen.cpp.
