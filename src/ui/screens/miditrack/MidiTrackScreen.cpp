// MIDI track screens (07-ui-design/screens/midi-track.md, UID-800…811; 03-ui/screens.md MIDI NOTE / CC1 /
// CC2; MID-040…050). Screen ids `track.note`, `track.cc1`, `track.cc2` and the popups `track.midiChannel` (SELECT CH),
// `midi.bankSelect`, `midi.progChange` (the ids TRACK MENU's MIDI cards and header actions open), all over
// the midi.* parameters. The knob strips bind at run time (ShellApi::bindStrip with the midi.* ids): until
// the MIDI catalogue is registered those columns stay empty, the bodies draw the catalogue's defaults, and the
// descriptors validate either way.
//   - MIDI NOTE: the PianoKeyboard (two octaves in bands, the overview C0–B9 with the shown window framed, ‹ ›); NOTE
//     in the track colour, NOTE2–4 at 60 % with their index. Knobs NOTE, VELOCITY, LENGTH, NOTE2, NOTE3, NOTE4 —
//     NOTE2–4 shown and stepped as offsets from NOTE (`+7`; 0 = OFF, ±24; our choice). Rotary: the window by
//     octaves; a tap on a key sets NOTE, SHIFT + tap puts the key on the first free chord note (NOTE2–4).
//   - MIDI CC1 / CC2: six CcCards (CC1: Pitch Bend, After Touch, CC #7, #1, #2, #10 — numbers 3–6 programmable; CC2:
//     #71…#76) with ACTIVE toggles; knobs = the six values (an inactive card's column `text-3`, still editable).
//     Rotary: the card focus; push toggles ACTIVE; SHIFT + push (or a second tap) opens the CC NUMBER list; SHIFT +
//     turn on a column steps that card's CC number (our choice).
//   - SELECT CH: ListPopup CH.1 … CH.16 on the track's send channel (midi.out.channel, project value per track).
//   - BANK SELECT / PROG CHANGE: SettingPopups `BANK SELECT · SCENE nn` (BANK MSB, BANK LSB) and `PROG CHANGE · SCENE
//     nn` per scene and track (MID-050).
#include "ui/descriptors/PopupSpec.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/PopupFactory.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/StripOwner.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/CcCard.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/PianoKeyboard.h"
#include "ui/widgets/SettingPopup.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui
{
namespace
{
namespace L = layout;
using Kind = io::ControlEvent::Kind;

constexpr StripSpec kNoteStrip {
    "NOTE", {"midi.note1", "midi.velocity", "midi.length", "midi.note2", "midi.note3", "midi.note4"}};
constexpr StripSpec kCc1Strip {"CC1",
                               {"midi.pitchBend", "midi.aftertouch", "midi.cc1.value3", "midi.cc1.value4",
                                "midi.cc1.value5", "midi.cc1.value6"}};
constexpr StripSpec kCc2Strip {"CC2",
                               {"midi.cc2.value1", "midi.cc2.value2", "midi.cc2.value3", "midi.cc2.value4",
                                "midi.cc2.value5", "midi.cc2.value6"}};
constexpr std::string_view kCc1Num[6] = {{}, {}, "midi.cc1.cc3", "midi.cc1.cc4", "midi.cc1.cc5", "midi.cc1.cc6"};
constexpr std::string_view kCc1Act[6] = {"midi.pitchBendActive", "midi.aftertouchActive", "midi.cc1.active3",
                                         "midi.cc1.active4",     "midi.cc1.active5",      "midi.cc1.active6"};
constexpr std::string_view kCc2Num[6] = {"midi.cc2.cc1", "midi.cc2.cc2", "midi.cc2.cc3",
                                         "midi.cc2.cc4", "midi.cc2.cc5", "midi.cc2.cc6"};
constexpr std::string_view kCc2Act[6] = {"midi.cc2.active1", "midi.cc2.active2", "midi.cc2.active3",
                                         "midi.cc2.active4", "midi.cc2.active5", "midi.cc2.active6"};
constexpr int kCc1Defaults[6] = {-1, -2, 7, 1, 2, 10};          // MID-041 (−1 Pitch Bend, −2 After Touch)
constexpr int kCc2Defaults[6] = {71, 72, 73, 74, 75, 76};       // MID-042
constexpr std::string_view kNoteIds[4] = {"midi.note1", "midi.note2", "midi.note3", "midi.note4"};

// The value of a track parameter of the selected track, `fallback` while it is not registered.
float value(ShellApi& sh, std::string_view id, float fallback)
{
    return m1::trackParam(sh, id, sh.state().selectedTrack, fallback);
}
void setValue(ShellApi& sh, std::string_view id, float v)
{
    m1::setTrackParam(sh, id, sh.state().selectedTrack, v);
}

// ---- MIDI NOTE ------------------------------------------------------------------------------------------------------
class MidiNoteBody final : public Widget, public StripOwner
{
public:
    explicit MidiNoteBody(const WidgetArgs& a) : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    PianoKeyboardModel model() const
    {
        PianoKeyboardModel m;
        for (int k = 0; k < 4; ++k)
            m.notes[static_cast<std::size_t>(k)] =
                static_cast<int>(std::lround(value(*shell_, kNoteIds[k], k == 0 ? 60.0f : -1.0f)));
        const int octave = m.notes[0] / 12 - 1;
        if (window_ < 0 || octave < window_ || octave > window_ + 1) // the window follows NOTE
            window_ = std::clamp(octave - 1, 0, 8);
        m.firstOctave = window_;
        m.focusedNote = m.notes[0];
        m.colour = theme::trackColour(m1::padColour(*shell_, state_->selectedTrack));
        return m;
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        if (! bound_)
        {
            shell_->bindStrip(&kNoteStrip);
            bound_ = true;
        }
        PianoKeyboard::draw(dl, ctx, model());
    }
    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            (void)model();
            window_ = std::clamp(window_ + e.value, 0, 8); // the shown octaves (‹ ›)
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
            return true;
        if (! touch::isTouch(e) || e.value != touch::kDown)
            return false;
        const auto m = model();
        const int note = PianoKeyboard::noteAt(m, touch::x(e), touch::y(e));
        if (note < 0)
            return false;
        if (! state_->shift)
            setValue(*shell_, kNoteIds[0], static_cast<float>(note));
        else // SHIFT + tap: the first free chord note (UID-802)
            for (int k = 1; k < 4; ++k)
                if (m.notes[static_cast<std::size_t>(k)] < 0)
                {
                    setValue(*shell_, kNoteIds[k], static_cast<float>(note));
                    break;
                }
        return true;
    }

    // NOTE2–4 as offsets from NOTE (our choice: the catalogue stores absolute notes, −1 = OFF).
    void stripColumn(int column0, ParamColumnModel& c) const override
    {
        if (column0 < 3 || c.spec == nullptr)
            return;
        const int note = static_cast<int>(std::lround(value(*shell_, kNoteIds[0], 60))), v = static_cast<int>(std::lround(c.value));
        auto& t = text_[static_cast<std::size_t>(column0 - 3)];
        if (v < 0)
            t.set("OFF");
        else
            t.format("%+d", v - note);
        c.valueText = t.view();
        c.stepped = true;
    }
    bool stripTurn(int column0, const core::ParamSpec& spec, int detents, bool) override
    {
        if (column0 < 3)
            return false;
        const int note = static_cast<int>(std::lround(value(*shell_, kNoteIds[0], 60)));
        const int v = static_cast<int>(std::lround(value(*shell_, spec.id, -1)));
        int offset = v < 0 ? 0 : v - note;
        offset = std::clamp(offset + detents, -24, 24);
        setValue(*shell_, spec.id, offset == 0 ? -1.0f : static_cast<float>(std::clamp(note + offset, 0, 127)));
        return true;
    }

private:
    ShellApi* shell_;
    UiState* state_;
    mutable int window_ = -1;
    mutable bool bound_ = false;
    mutable std::array<FixedText<8>, 3> text_ {};
};

// ---- MIDI CC1 / CC2 -------------------------------------------------------------------------------------------------
class MidiCcBody final : public Widget, public StripOwner
{
public:
    MidiCcBody(const WidgetArgs& a, int page) : shell_(a.shell), state_(a.state), page_(page) {}
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }
    int focusCard() const noexcept { return focus_; }

    int ccOf(int i) const
    {
        const auto& ids = page_ == 1 ? kCc1Num : kCc2Num;
        const int def = page_ == 1 ? kCc1Defaults[i] : kCc2Defaults[i];
        if (ids[i].empty())
            return def;
        return static_cast<int>(std::lround(value(*shell_, ids[i], static_cast<float>(def))));
    }
    bool activeOf(int i) const { return value(*shell_, (page_ == 1 ? kCc1Act : kCc2Act)[i], 0) >= 0.5f; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        if (! bound_)
        {
            shell_->bindStrip(page_ == 1 ? &kCc1Strip : &kCc2Strip);
            bound_ = true;
        }
        for (int i = 0; i < 6; ++i)
        {
            CcCardModel m;
            m.index = i;
            m.cc = ccOf(i);
            CcCard::nameOf(m.cc, m.name1, m.name2);
            if (m.cc < 0)
                m.cc = -1; // Pitch Bend / After Touch: no number
            m.active = activeOf(i);
            m.focused = i == focus_;
            CcCard::draw(dl, ctx, m);
        }
    }
    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            focus_ = std::clamp(focus_ + e.value, 0, 5);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            if (state_->shift)
                ccList(focus_);
            else
                toggleActive(focus_);
            return true;
        }
        if (! touch::isTouch(e) || e.value != touch::kDown)
            return false;
        const float x = touch::x(e), y = touch::y(e);
        for (int i = 0; i < 6; ++i)
        {
            if (contains(CcCard::toggle(i), x, y))
            {
                toggleActive(i);
                return true;
            }
            if (contains(CcCard::card(i), x, y))
            {
                if (focus_ == i)
                    ccList(i); // the second tap: CC NUMBER (UID-806)
                focus_ = i;
                return true;
            }
        }
        return false;
    }

    void stripColumn(int column0, ParamColumnModel& c) const override
    {
        if (c.spec == nullptr)
            return;
        if (! activeOf(column0))
            c.inactive = true; // UID-803: text-3, still editable
    }
    bool stripTurn(int column0, const core::ParamSpec&, int detents, bool) override
    {
        if (! state_->shift)
            return false; // the value: the shell's default write
        const auto& ids = page_ == 1 ? kCc1Num : kCc2Num;
        if (ids[column0].empty())
            return true;
        setValue(*shell_, ids[column0], static_cast<float>(std::clamp(ccOf(column0) + detents, 0, 127)));
        return true;
    }

private:
    void toggleActive(int i)
    {
        setValue(*shell_, (page_ == 1 ? kCc1Act : kCc2Act)[i], activeOf(i) ? 0.0f : 1.0f);
    }
    void ccList(int i)
    {
        const auto& ids = page_ == 1 ? kCc1Num : kCc2Num;
        if (ids[i].empty())
            return; // Pitch Bend / After Touch have no number
        auto& labels = labels_();
        ListRequest req;
        req.title = "CC NUMBER";
        for (int cc = 0; cc < 128; ++cc)
            req.items.push_back({labels[static_cast<std::size_t>(cc)], {}, {}, cc});
        req.current = ccOf(i);
        const std::string_view id = ids[i];
        req.onChoose = [this, id](int, const ListItem& it) { setValue(*shell_, id, static_cast<float>(it.value)); };
        shell_->showList(std::move(req));
    }
    // "CC #7 Channel Volume" … built once (static storage for the ListItems' views).
    static std::array<std::string, 128>& labels_()
    {
        static std::array<std::string, 128> l = []
        {
            std::array<std::string, 128> out;
            for (int cc = 0; cc < 128; ++cc)
            {
                std::string_view a, b;
                CcCard::nameOf(cc, a, b);
                out[static_cast<std::size_t>(cc)] =
                    "CC #" + std::to_string(cc) + " " + std::string(a) + (b.empty() ? "" : " ") + std::string(b);
            }
            return out;
        }();
        return l;
    }

    ShellApi* shell_;
    UiState* state_;
    int page_;
    int focus_ = 0;
    mutable bool bound_ = false;
};

std::unique_ptr<Widget> makeNote(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<MidiNoteBody>(a);
}
std::unique_ptr<Widget> makeCc(const ScreenSpec& s, const WidgetArgs& a)
{
    return std::make_unique<MidiCcBody>(a, s.id == "track.cc2" ? 2 : 1);
}

// ---- popups ---------------------------------------------------------------------------------------------------------
constexpr std::string_view kChannels[] = {"CH.1", "CH.2",  "CH.3",  "CH.4",  "CH.5",  "CH.6",  "CH.7",  "CH.8",
                                          "CH.9", "CH.10", "CH.11", "CH.12", "CH.13", "CH.14", "CH.15", "CH.16"};

std::unique_ptr<PopupWidget> makeSelectCh(const PopupSpec&, ShellApi& sh)
{
    const auto* spec = sh.params().find("midi.out.channel");
    const auto track0 = static_cast<std::uint16_t>((sh.state().selectedTrack - 1) & 15);
    ListRequest req;
    req.title = "SELECT CH";
    for (int c = 0; c < 16; ++c)
        req.items.push_back({kChannels[c], {}, {}, c + 1});
    // midi.out.channel is a project value per track (engine scope = the track, MID-011); default channel n = track n.
    const int ch = spec != nullptr ? static_cast<int>(std::lround(sh.values().get(*spec, track0))) : track0 + 1;
    req.current = std::clamp(ch, 1, 16) - 1;
    req.onChoose = [&sh, spec, track0](int, const ListItem& it)
    {
        if (spec != nullptr)
            sh.values().set(*spec, track0, static_cast<float>(it.value));
    };
    return std::make_unique<ListPopup>(std::move(req));
}

struct SceneTitles
{
    std::array<std::string, 16> bank, program;
    SceneTitles()
    {
        for (int s = 0; s < 16; ++s)
        {
            char b[48];
            std::snprintf(b, sizeof b, "BANK SELECT \xC2\xB7 SCENE %02d", s + 1);
            bank[static_cast<std::size_t>(s)] = b;
            std::snprintf(b, sizeof b, "PROG CHANGE \xC2\xB7 SCENE %02d", s + 1);
            program[static_cast<std::size_t>(s)] = b;
        }
    }
};
const SceneTitles& titles()
{
    static const SceneTitles t;
    return t;
}

// "---" (OFF, not sent) and "000" … "127": the rows' values as the reference shows them (UI-293/294).
const std::vector<std::string_view>& byteOptions()
{
    static const auto table = []
    {
        static std::array<char[4], 129> text {};
        std::vector<std::string_view> out;
        std::snprintf(text[0], 4, "---");
        out.emplace_back(text[0]);
        for (int v = 0; v < 128; ++v)
        {
            std::snprintf(text[static_cast<std::size_t>(v + 1)], 4, "%03d", v);
            out.emplace_back(text[static_cast<std::size_t>(v + 1)]);
        }
        return out;
    }();
    return table;
}

std::unique_ptr<PopupWidget> settingFor(ShellApi& sh, bool bank)
{
    const int scene = (sh.state().scene - 1) & 15;
    SettingRequest req;
    req.title = bank ? titles().bank[static_cast<std::size_t>(scene)] : titles().program[static_cast<std::size_t>(scene)];
    static constexpr std::string_view kBank[] = {"midi.bankMsb", "midi.bankLsb"}, kProg[] = {"midi.program"};
    static constexpr std::string_view kBankLabels[] = {"BANK MSB", "BANK LSB"}, kProgLabels[] = {"PROG CHANGE"};
    const std::span<const std::string_view> ids = bank ? std::span<const std::string_view>(kBank)
                                                       : std::span<const std::string_view>(kProg);
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        SettingRow r;
        r.label = bank ? kBankLabels[i] : kProgLabels[i];
        r.options = byteOptions(); // a long enum: push opens the nested list (−1 = OFF … 127)
        r.current = std::clamp(static_cast<int>(std::lround(value(sh, ids[i], 0))) + 1, 0, 128);
        req.rows.push_back(std::move(r));
    }
    std::vector<std::string_view> rowIds(ids.begin(), ids.end());
    req.onCommit = [&sh, rowIds](int row, int option, float)
    {
        if (row >= 0 && row < static_cast<int>(rowIds.size())) // MID-050: sent on the next scene switch
            setValue(sh, rowIds[static_cast<std::size_t>(row)], static_cast<float>(option - 1));
    };
    return std::make_unique<SettingPopup>(std::move(req));
}
std::unique_ptr<PopupWidget> makeBankSelect(const PopupSpec&, ShellApi& sh)
{
    return settingFor(sh, true);
}
std::unique_ptr<PopupWidget> makeProgChange(const PopupSpec&, ShellApi& sh)
{
    return settingFor(sh, false);
}
} // namespace

// Tests: the CC screens' focused card (−1 = not a CC screen).
int midiCcFocus(const Widget* body) noexcept
{
    const auto* b = dynamic_cast<const MidiCcBody*>(body);
    return b != nullptr ? b->focusCard() : -1;
}
} // namespace ui

APP_REGISTER_WIDGET("PianoKeyboard", &ui::makeNote)
APP_REGISTER_WIDGET("CcCards", &ui::makeCc)
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "track.note",
                                     .title = "MIDI NOTE",
                                     .parent = "track.menu",
                                     .body = ui::BodyTemplate::Full,
                                     .contextBar = ui::ContextBarKind::TrackContext,
                                     .rail = {},
                                     .bodyComponent = "PianoKeyboard",
                                     .strip = {"NOTE", {}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "track.cc1",
                                     .title = "MIDI CC1",
                                     .parent = "track.menu",
                                     .body = ui::BodyTemplate::Full,
                                     .contextBar = ui::ContextBarKind::TrackContext,
                                     .rail = {},
                                     .bodyComponent = "CcCards",
                                     .strip = {"CC1", {}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "track.cc2",
                                     .title = "MIDI CC2",
                                     .parent = "track.menu",
                                     .body = ui::BodyTemplate::Full,
                                     .contextBar = ui::ContextBarKind::TrackContext,
                                     .rail = {},
                                     .bodyComponent = "CcCards",
                                     .strip = {"CC2", {}}}))
APP_REGISTER_POPUP((ui::PopupSpec {.kind = ui::PopupSpec::Kind::List, .id = "track.midiChannel", .title = "SELECT CH"}))
APP_REGISTER_POPUP((ui::PopupSpec {.kind = ui::PopupSpec::Kind::Setting, .id = "midi.bankSelect", .title = "BANK SELECT"}))
APP_REGISTER_POPUP((ui::PopupSpec {.kind = ui::PopupSpec::Kind::Setting, .id = "midi.progChange", .title = "PROG CHANGE"}))
APP_REGISTER_POPUP_FACTORY("track.midiChannel", &ui::makeSelectCh)
APP_REGISTER_POPUP_FACTORY("midi.bankSelect", &ui::makeBankSelect)
APP_REGISTER_POPUP_FACTORY("midi.progChange", &ui::makeProgChange)
