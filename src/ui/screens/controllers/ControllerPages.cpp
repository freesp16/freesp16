// UTILITY › CONTROLLERS and UTILITY › KEYBOARD MAP (03-ui/screens.md UI-330, control-surface.md UI-120,
// UI-130; 07-ui-design/screens/utility.md "Sub-pages": the UTILITY list layout, UID-880). Screen ids
// `utility.controllers` / `utility.keyboardMap` (fixed); the UTILITY rows open them.
//
// CONTROLLERS: one header per MIDI input port the hub knows (io::controllers::hub(): connected, or remembered in
// <data dir>/Controllers/settings.json), then ROLE (AUTO / CONTROLLER / INSTRUMENT, UI-136, row: SET-010's role
// lives here until MIDI PORTS has it), PROFILE (list popup), PAD CURVE, PAD THRESHOLD, PAD SENSITIVITY, FIXED
// VELOCITY, VELOCITY TEST (the last hit, live), PAD TRIM › (PAD 1…16, −20…+20 velocity; our choice) and MIDI LEARN ›
// (move a control → its message is shown → pick the logical control → saved as the port's user profile).
// KEYBOARD MAP: one row per logical control with a key (UI-120 order), value = its key; push waits for a key
// ("Press a key…", Esc cancels; reserved keys refuse), SHIFT + push or CLEAR (ext.clear) = back to the default.
// Rotary = focus (value rows: push enters editing, the rotary steps the value, push leaves), touch = focus / activate.
#include "io/controllers/ControlNames.h"
#include "io/controllers/profiles/ControllerHub.h"
#include "io/controllers/profiles/KeyRemap.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/ListRow.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace ctl = io::controllers;
using Kind = io::ControlEvent::Kind;
constexpr float kX = 16, kW = 1248, kListY = 200, kRowH = 44;
constexpr int kVisible = 9;

struct Row
{
    std::string label, value;
    bool header = false, opens = false, info = false;
    int id = 0;       // what the row does (page-specific)
    int port = -1;    // CONTROLLERS: the port index
    int arg = 0;
};

// The UTILITY list layout shared by both pages: title band, rows, scrollbar, focus, editing, touch.
class ListPage : public Widget
{
public:
    explicit ListPage(const WidgetArgs& a, std::string title) : shell_(a.shell), state_(a.state), title_(std::move(title)) {}
    RectF bounds() const noexcept override { return {kX, L::contentTop, kW, L::contentH}; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        refresh(ctx.nowMs);
        const auto& p = ctx.palette;
        dl.fill({kX, 152, kW, 48}, p.surface1, tk::radiusM);
        dl.fill({kX, 192, kW, 8}, p.surface1);
        draw::hline(dl, kX, kX + kW, 200, p.border);
        dl.text(kX + 24, 176, Align::Left, TextStyleId::Title, p.text1, title_);
        dl.text(kX + kW - 24, 176, Align::Right, TextStyleId::Body, p.text2, subtitle_);
        const int n = std::min(static_cast<int>(rows_.size()) - first_, kVisible);
        for (int i = 0; i < n; ++i)
        {
            const auto& row = rows_[static_cast<std::size_t>(first_ + i)];
            ListRowModel m;
            m.label = row.label;
            m.value = row.value;
            m.zebra = first_ + i;
            m.header = row.header;
            m.focused = first_ + i == focus_;
            m.editing = m.focused && editing_;
            m.opens = row.opens;
            m.valueX = 640;
            m.labelX = 40;
            ListRow::draw(dl, ctx, {kX, kListY + kRowH * static_cast<float>(i), kW - 8, kRowH}, m);
        }
        if (static_cast<int>(rows_.size()) > kVisible)
        {
            const float h = 400.0f * kVisible / static_cast<float>(rows_.size());
            const float y = 204 + (400.0f - h) * static_cast<float>(first_)
                                      / static_cast<float>(std::max(1, static_cast<int>(rows_.size()) - kVisible));
            dl.fill({1256, 204, 4, 400}, p.surface2, 2);
            dl.fill({1256, y, 4, h}, p.surface3, 2);
        }
    }
    bool dirty(double) const override { return true; } // live values (VELOCITY TEST, learn, capture)

    bool handle(const io::ControlEvent& e, UiContext& ctx) override
    {
        refresh(ctx.nowMs);
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (editing_ && focus_ >= 0)
                step(rows_[static_cast<std::size_t>(focus_)], e.value);
            else
                move(e.value);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            push(state_ != nullptr && state_->shift); // SHIFT is the shell's (UiState)
            return true;
        }
        if (e.control == io::Control::ExtClear && e.kind == Kind::Press && focus_ >= 0)
        {
            push(true);
            return true;
        }
        if (touch::isTouch(e) && e.value == touch::kDown)
        {
            const float y = touch::y(e);
            const int i = first_ + static_cast<int>((y - kListY) / kRowH);
            if (y < kListY || i >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(i)].header)
                return false;
            if (i == focus_)
                push(false);
            else
            {
                editing_ = false;
                focus_ = i;
            }
            return true;
        }
        return false;
    }

    // Tests.
    int focus() const noexcept { return focus_; }
    const std::vector<Row>& rows() const noexcept { return rows_; }

protected:
    // Rebuilds rows_ (keeps the focus on the same row index when possible). Called every frame and before input.
    virtual void refresh(double nowMs) const = 0;
    virtual void activate(Row& row, bool shift) = 0;  // push on a non-value row (or a value row when no editing)
    virtual bool editable(const Row&) const { return false; }
    virtual void step(Row&, int) {}
    virtual bool back() { return false; } // the "‹ …" row of a sub-mode (BACK itself is the shell's: leaves the page)
    void fixFocus() const
    {
        if (rows_.empty())
        {
            focus_ = -1;
            return;
        }
        focus_ = std::clamp(focus_, 0, static_cast<int>(rows_.size()) - 1);
        if (rows_[static_cast<std::size_t>(focus_)].header)
            for (std::size_t i = 0; i < rows_.size(); ++i)
                if (! rows_[i].header)
                {
                    focus_ = static_cast<int>(i);
                    break;
                }
        if (focus_ < first_ + 1)
            first_ = std::max(0, focus_ - 1);
        if (focus_ > first_ + kVisible - 2)
            first_ = std::max(0, std::min(focus_ - kVisible + 2, static_cast<int>(rows_.size()) - kVisible));
        first_ = std::clamp(first_, 0, std::max(0, static_cast<int>(rows_.size()) - kVisible));
    }
    void move(int detents)
    {
        int f = focus_;
        for (int k = 0; k < std::abs(detents); ++k)
        {
            int next = f + (detents > 0 ? 1 : -1);
            while (next >= 0 && next < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(next)].header)
                next += detents > 0 ? 1 : -1;
            if (next < 0 || next >= static_cast<int>(rows_.size()))
                break;
            f = next;
        }
        focus_ = f;
        fixFocus();
    }
    void push(bool shift)
    {
        if (focus_ < 0 || focus_ >= static_cast<int>(rows_.size()))
            return;
        auto& row = rows_[static_cast<std::size_t>(focus_)];
        if (! shift && editable(row))
        {
            editing_ = ! editing_;
            return;
        }
        editing_ = false;
        activate(row, shift);
    }

    ShellApi* shell_;
    UiState* state_;
    std::string title_;
    mutable std::string subtitle_;
    mutable std::vector<Row> rows_;
    mutable int focus_ = 0, first_ = 0;
    bool editing_ = false;
};

// ---- CONTROLLERS ----

constexpr std::string_view kCurves[] = {"HARD 3", "HARD 2", "HARD 1", "LINEAR", "SOFT 1", "SOFT 2", "SOFT 3", "FIXED"};
int curveIndex(io::PadCurveShape s) noexcept
{
    return s == io::PadCurveShape::Fixed ? 7 : static_cast<int>(s) + 3;
}
io::PadCurveShape curveOf(int i) noexcept
{
    return i >= 7 ? io::PadCurveShape::Fixed : static_cast<io::PadCurveShape>(std::clamp(i, 0, 6) - 3);
}

enum RowId { kReturn = 100, kRole = 1, kProfile, kCurve, kThreshold, kSensitivity, kFixed, kVelocity, kTrim, kLearn, kTrimPad, kLearnInfo,
             kLearnPick, kNone };

class ControllersBody final : public ListPage
{
public:
    explicit ControllersBody(const WidgetArgs& a) : ListPage(a, "CONTROLLERS") {}
    ~ControllersBody() override
    {
        if (mode_ == Mode::Learn) // leaving the page (BACK, HOME) ends MIDI learn
            ctl::hub().cancelLearn();
    }

protected:
    void refresh(double) const override
    {
        auto& h = ctl::hub();
        ports_ = h.ports();
        rows_.clear();
        char buf[64];
        if (mode_ == Mode::Trim && trimPort_ < static_cast<int>(ports_.size()))
        {
            const auto& pt = ports_[static_cast<std::size_t>(trimPort_)];
            subtitle_ = "PAD TRIM \xE2\x80\xBA " + pt.name;
            rows_.push_back({"\xE2\x80\xB9 CONTROLLERS", "", false, false, false, kReturn});
            for (int i = 0; i < 16; ++i)
            {
                std::snprintf(buf, sizeof buf, "%+d", pt.settings.trim[static_cast<std::size_t>(i)]);
                rows_.push_back({"PAD " + std::to_string(i + 1), buf, false, false, false, kTrimPad, trimPort_, i});
            }
            fixFocus();
            return;
        }
        if (mode_ == Mode::Learn)
        {
            subtitle_ = "MIDI LEARN \xE2\x80\xBA " + h.learnPort();
            h.pollLearn();
            rows_.push_back({"\xE2\x80\xB9 CONTROLLERS", "", false, false, false, kReturn});
            const auto st = h.learnState();
            std::string what = "\xE2\x80\x94";
            if (st == ctl::LearnState::Captured)
            {
                const auto m = h.learned();
                const int hi = m.status & 0xF0;
                if (m.status >= 0xFA)
                    what = m.status == 0xFA ? "START" : m.status == 0xFB ? "CONTINUE" : "STOP";
                else if (m.status == 0xF0)
                    what = "MMC " + std::to_string(m.number);
                else
                {
                    std::snprintf(buf, sizeof buf, "%s %d  CH %d",
                                  hi == 0x90 ? "NOTE" : hi == 0xB0 ? "CC" : hi == 0xE0 ? "PITCH BEND" : "PRESSURE",
                                  m.number, (m.status & 15) + 1);
                    what = hi == 0xE0 || hi == 0xD0 ? std::string(hi == 0xE0 ? "PITCH BEND" : "PRESSURE") + "  CH "
                                                          + std::to_string((m.status & 15) + 1)
                                                    : std::string(buf);
                }
            }
            rows_.push_back({"MESSAGE", st == ctl::LearnState::Waiting ? "Move a control\xE2\x80\xA6" : what, false,
                             false, true, kLearnInfo});
            rows_.push_back({"LOGICAL CONTROL", st == ctl::LearnState::Captured ? "push to choose" : "\xE2\x80\x94",
                             false, st == ctl::LearnState::Captured, false, kLearnPick});
            if (! note_.empty())
                rows_.push_back({"", note_, false, false, true, kNone});
            if (st == ctl::LearnState::Captured && lastLearn_ != st)
                focus_ = 2; // a message arrived: on to LOGICAL CONTROL
            lastLearn_ = st;
            fixFocus();
            return;
        }
        subtitle_ = std::to_string(h.connectedCount()) + " connected";
        if (ports_.empty())
            rows_.push_back({"No MIDI input ports", "", false, false, true, kNone});
        for (std::size_t i = 0; i < ports_.size(); ++i)
        {
            const auto& pt = ports_[i];
            const int pi = static_cast<int>(i);
            rows_.push_back({pt.name + (pt.connected ? "" : "  (not connected)"), "", true, false, false, kNone, pi});
            const auto role = pt.settings.role;
            rows_.push_back({"ROLE",
                             std::string(role == ctl::PortRole::Auto ? "AUTO" : role == ctl::PortRole::Controller
                                                                                    ? "CONTROLLER"
                                                                                    : "INSTRUMENT")
                                 + (role == ctl::PortRole::Auto ? (pt.controller ? " (CONTROLLER)" : " (INSTRUMENT)") : ""),
                             false, false, false, kRole, pi});
            rows_.push_back({"PROFILE", pt.profileName + (pt.settings.profile.empty() ? " (AUTO)" : ""), false, false,
                             false, kProfile, pi});
            const auto& r = pt.settings.response;
            rows_.push_back({"PAD CURVE", std::string(kCurves[curveIndex(r.shape)]), false, false, false, kCurve, pi});
            rows_.push_back({"PAD THRESHOLD", std::to_string(r.threshold), false, false, false, kThreshold, pi});
            rows_.push_back({"PAD SENSITIVITY", std::to_string(r.sensitivity), false, false, false, kSensitivity, pi});
            rows_.push_back({"FIXED VELOCITY", std::to_string(r.fixedVelocity), false, false, false, kFixed, pi});
            rows_.push_back({"VELOCITY TEST", pt.controller && pt.lastVelocity > 0 ? std::to_string(pt.lastVelocity)
                                                                                   : std::string("\xE2\x80\x94"),
                             false, false, true, kVelocity, pi});
            rows_.push_back({"PAD TRIM", "", false, true, false, kTrim, pi});
            rows_.push_back({"MIDI LEARN", pt.connected && pt.controller ? "" : "connect as a controller", false, true,
                             false, kLearn, pi});
        }
        fixFocus();
    }

    bool editable(const Row& r) const override
    {
        return r.id == kCurve || r.id == kThreshold || r.id == kSensitivity || r.id == kFixed || r.id == kTrimPad;
    }

    void step(Row& row, int d) override
    {
        if (row.port < 0 || row.port >= static_cast<int>(ports_.size()))
            return;
        const auto& pt = ports_[static_cast<std::size_t>(row.port)];
        auto r = pt.settings.response;
        switch (row.id)
        {
        case kCurve: r.shape = curveOf(std::clamp(curveIndex(r.shape) + d, 0, 7)); break;
        case kThreshold: r.threshold = std::clamp(r.threshold + d, 1, 16); break;
        case kSensitivity: r.sensitivity = std::clamp(r.sensitivity + d, 1, 16); break;
        case kFixed: r.fixedVelocity = std::clamp(r.fixedVelocity + d, 1, 127); break;
        case kTrimPad:
            ctl::hub().setTrim(pt.name, row.arg, pt.settings.trim[static_cast<std::size_t>(row.arg)] + d);
            return;
        default: return;
        }
        ctl::hub().setResponse(pt.name, r);
    }

    void activate(Row& row, bool) override
    {
        auto& h = ctl::hub();
        if (row.id == kReturn)
        {
            back();
            return;
        }
        if (row.id == kLearnPick && h.learnState() == ctl::LearnState::Captured)
        {
            pickControl();
            return;
        }
        if (row.port < 0 || row.port >= static_cast<int>(ports_.size()))
            return;
        const std::string name = ports_[static_cast<std::size_t>(row.port)].name;
        const auto& pt = ports_[static_cast<std::size_t>(row.port)];
        switch (row.id)
        {
        case kRole:
            h.setRole(name, pt.settings.role == ctl::PortRole::Auto        ? ctl::PortRole::Controller
                            : pt.settings.role == ctl::PortRole::Controller ? ctl::PortRole::Instrument
                                                                            : ctl::PortRole::Auto);
            break;
        case kProfile: pickProfile(name, pt.settings.profile); break;
        case kTrim:
            mode_ = Mode::Trim;
            trimPort_ = row.port;
            focus_ = 1;
            first_ = 0;
            break;
        case kLearn:
            note_.clear();
            if (h.startLearn(name))
            {
                mode_ = Mode::Learn;
                focus_ = 1;
                first_ = 0;
            }
            break;
        default: break;
        }
    }

    bool back() override
    {
        if (mode_ == Mode::List)
            return false;
        if (mode_ == Mode::Learn)
            ctl::hub().cancelLearn();
        mode_ = Mode::List;
        focus_ = 0;
        first_ = 0;
        return true;
    }

private:
    enum class Mode { List, Trim, Learn };
    void pickProfile(const std::string& port, const std::string& current)
    {
        if (shell_ == nullptr)
            return;
        profileIds_.clear();
        profileNames_.clear();
        profileIds_.push_back("");
        profileNames_.push_back("AUTO (by port name)");
        for (const auto* p : ctl::hub().profiles())
        {
            profileIds_.push_back(p->id);
            profileNames_.push_back(p->name);
        }
        ListRequest req;
        req.title = "PROFILE";
        for (std::size_t i = 0; i < profileNames_.size(); ++i)
        {
            req.items.push_back({profileNames_[i], {}, {}, static_cast<int>(i)});
            if (profileIds_[i] == current)
                req.current = static_cast<int>(i);
        }
        req.onChoose = [this, port](int index, const ListItem&) {
            if (index >= 0 && index < static_cast<int>(profileIds_.size()))
                ctl::hub().setProfile(port, profileIds_[static_cast<std::size_t>(index)]);
        };
        shell_->showList(std::move(req));
    }
    void pickControl()
    {
        if (shell_ == nullptr)
            return;
        controls_.clear();
        for (const auto c : io::referenceInputControls())
            if (c != io::Control::Screen)
                controls_.push_back(c);
        for (int c = static_cast<int>(io::Control::ExtCopy); c <= static_cast<int>(io::Control::ExtProject); ++c)
            controls_.push_back(static_cast<io::Control>(c));
        ListRequest req;
        req.title = "LOGICAL CONTROL";
        for (std::size_t i = 0; i < controls_.size(); ++i)
            req.items.push_back({io::nameOf(controls_[i]), {}, {}, static_cast<int>(i)});
        req.onChoose = [this](int index, const ListItem&) {
            if (index < 0 || index >= static_cast<int>(controls_.size()))
                return;
            std::string why;
            if (ctl::hub().assignLearned(controls_[static_cast<std::size_t>(index)], why))
            {
                note_ = "Saved: " + std::string(io::nameOf(controls_[static_cast<std::size_t>(index)]))
                      + ". Move another control.";
                ctl::hub().startLearn(ctl::hub().learnPort()); // keep learning on the same port
            }
            else
                note_ = "Not saved: " + why;
        };
        shell_->showList(std::move(req));
    }

    mutable std::vector<ctl::ControllerPort> ports_;
    mutable ctl::LearnState lastLearn_ = ctl::LearnState::Idle;
    Mode mode_ = Mode::List;
    int trimPort_ = 0;
    std::string note_;
    std::vector<std::string> profileIds_, profileNames_;
    std::vector<io::Control> controls_;
};

// ---- KEYBOARD MAP ----

class KeyboardMapBody final : public ListPage
{
public:
    explicit KeyboardMapBody(const WidgetArgs& a) : ListPage(a, "KEYBOARD MAP")
    {
        for (const auto& b : ctl::defaultKeyBindings())
            if (std::find(controls_.begin(), controls_.end(), b.control) == controls_.end())
                controls_.push_back(b.control);
    }
    ~KeyboardMapBody() override { ctl::keyRemap().disarm(); }

protected:
    void refresh(double) const override
    {
        auto& remap = ctl::keyRemap();
        ctl::KeyBinding got;
        bool cancelled = false;
        if (capturing_ >= 0 && remap.takeCapture(got, cancelled))
        {
            if (! cancelled)
                remap.assign(controls_[static_cast<std::size_t>(capturing_)], got.code, got.cls);
            capturing_ = -1;
        }
        else if (capturing_ >= 0 && ! remap.armed())
            capturing_ = -1;
        rows_.clear();
        subtitle_ = std::to_string(remap.overrides().size()) + " changed";
        for (std::size_t i = 0; i < controls_.size(); ++i)
        {
            const auto k = remap.keyFor(controls_[i]);
            rows_.push_back({std::string(io::nameOf(controls_[i])),
                             capturing_ == static_cast<int>(i) ? std::string("Press a key\xE2\x80\xA6")
                                                               : ctl::keyName(k.code, k.cls),
                             false, false, false, 1, -1, static_cast<int>(i)});
        }
        fixFocus();
    }
    void activate(Row& row, bool shift) override
    {
        auto& remap = ctl::keyRemap();
        if (shift)
        {
            remap.reset(controls_[static_cast<std::size_t>(row.arg)]);
            return;
        }
        capturing_ = row.arg;
        remap.arm();
    }
    bool back() override
    {
        if (capturing_ < 0)
            return false;
        ctl::keyRemap().disarm(); // BACK from the panel / a controller cancels too
        capturing_ = -1;
        return true;
    }

private:
    std::vector<io::Control> controls_;
    mutable int capturing_ = -1;
};

std::unique_ptr<Widget> makeControllers(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<ControllersBody>(a);
}
std::unique_ptr<Widget> makeKeyboardMap(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<KeyboardMapBody>(a);
}
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("ControllersPage", &ui::makeControllers)
APP_REGISTER_WIDGET("KeyboardMapPage", &ui::makeKeyboardMap)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "utility.controllers",
                                    .title = "CONTROLLERS",
                                    .parent = "utility",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "ControllersPage",
                                    .strip = {"CONTROLLERS", {}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "utility.keyboardMap",
                                    .title = "KEYBOARD MAP",
                                    .parent = "utility",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "KeyboardMapPage",
                                    .strip = {"KEYBOARD MAP", {}}}))
