// TOUCH STRIP SETTING (USER1 / USER2) (07-ui-design/screens/utility.md "Layout — TOUCH STRIP SETTING"; UI-331…333,
// UID-881…885 as constraints; RIB-004/006/011). Template rail+content, context bar menu (UTILITY lit),
// breadcrumb HOME › UTILITY › TOUCH STRIP SETTING (USER1). Rail DELETE; body: PanelTitle `USER1` + `n of 8 assigned`,
// the column header (# · PARAMETER · BOTTOM VALUE · TOP VALUE), the eight places (`MODULE › NAME`, BOTTOM, TOP, ›; an
// empty place says "— empty · push to assign"), and the StripPreview of the focused place. Strip tab `USER1 · n`,
// columns 1–2 BOTTOM VALUE / TOP VALUE of the focused place in the target parameter's range and format (disabled on
// an empty place).
//   - The rotary moves over the eight places (UI-331); past place 1 it reaches DELETE, past DELETE the places again.
//   - Push (or a tap on the focused place) opens SELECT PARAMETER, generated from ParamRegistry::stripTargets()
//     (ui/screens/lfo/Targets.h; NONE first, groups with headers, FX groups for the effects in the selected track's
//     slots, current marked and focused; an empty place opens on the first PLAYBACK row). Choosing assigns the target
//     (core::commands::ribbon::assignItem: a place past the list's end appends); NONE removes it (deleteItem).
//   - DELETE (disabled on an empty place, place kept) asks with a destructive Message "Remove FX1 › CUT OFF from
//     USER1?" (UI-332, UI-296) and removes the item (deleteItem; the later items move up).
//   - Encoders 1–2: setItemRange in the target's steps (coarse / SHIFT fine) over its range; BOTTOM may exceed TOP
//     (an inverted strip).
// The project's RibbonSettings are read and edited through the ui::m2 session's bus (the app's document), every edit
// one undo step; io/controllers/StripSource forwards "ribbon" changes to the engine.
#include "core/commands/ribbon/RibbonCommands.h"
#include "ui/descriptors/SettingsSpec.h"
#include "ui/screens/lfo/Targets.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/screens/touchstrip/StripLive.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/StripPreview.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace rb = ::core::commands::ribbon;
using Kind = io::ControlEvent::Kind;

constexpr float kListX = 224, kListW = 808, kRowsY = 236, kRowH = 44;

#define APP_PLACE(SET, N) StripSpec{"USER" #SET " \xC2\xB7 " #N, {"ribbon.userBottom", "ribbon.userTop", "", "", "", ""}}
constexpr StripSpec kPlaceStrips[2][8] = {
    {APP_PLACE(1, 1), APP_PLACE(1, 2), APP_PLACE(1, 3), APP_PLACE(1, 4), APP_PLACE(1, 5), APP_PLACE(1, 6),
     APP_PLACE(1, 7), APP_PLACE(1, 8)},
    {APP_PLACE(2, 1), APP_PLACE(2, 2), APP_PLACE(2, 3), APP_PLACE(2, 4), APP_PLACE(2, 5), APP_PLACE(2, 6),
     APP_PLACE(2, 7), APP_PLACE(2, 8)}};
#undef APP_PLACE

class TouchStripBody final : public Widget, public RailOwner, public StripOwner
{
public:
    TouchStripBody(const WidgetArgs& a, int set) noexcept : shell_(a.shell), state_(a.state), set_(set)
    {
        bind();
    }
    RectF bounds() const noexcept override { return {kListX, L::contentTop, 1040, L::contentH}; }
    int focusedPlace() const noexcept { return place_; }
    int set() const noexcept { return set_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        const auto& r = ribbon();
        const int count = rb::itemCount(r, set_);
        // PanelTitle (project.md): USER1, right "n of 8 assigned".
        dl.fill({kListX, 152, kListW, 48}, p.surface1, tk::radiusM);
        dl.fill({kListX, 192, kListW, 8}, p.surface1);
        draw::hline(dl, kListX, kListX + kListW, 200, p.border);
        dl.text(kListX + 24, 176, Align::Left, TextStyleId::Title, p.text1, set_ == 0 ? "USER1" : "USER2");
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d of 8 assigned", count);
        dl.text(kListX + kListW - 24, 176, Align::Right, TextStyleId::Body, p.text2, buf);
        dl.fill({kListX, 200, kListW, 36}, p.surface3);
        dl.text(kListX + 24, 218, Align::Left, TextStyleId::Caption, p.text2, "#");
        dl.text(kListX + 72, 218, Align::Left, TextStyleId::Caption, p.text2, "PARAMETER");
        dl.text(kListX + 520, 218, Align::Left, TextStyleId::Caption, p.text2, "BOTTOM VALUE");
        dl.text(kListX + 672, 218, Align::Left, TextStyleId::Caption, p.text2, "TOP VALUE");
        const bool bodyFocus = ctx.focus.focused < 0;
        for (int i = 0; i < 8; ++i)
        {
            const RectF row{kListX, kRowsY + kRowH * static_cast<float>(i), kListW, kRowH};
            const float cy = row.y + kRowH / 2;
            dl.fill(row, i % 2 == 0 ? p.surface2 : p.surface1);
            const bool focused = bodyFocus && i == place_;
            if (focused)
            {
                dl.fill(row, p.accentWeak);
                dl.fill({row.x, row.y, 3, kRowH}, p.accent);
            }
            char n[4];
            std::snprintf(n, sizeof n, "%d", i + 1);
            dl.text(row.x + 28, cy, Align::Centre, TextStyleId::Body, p.text2, n);
            const auto* spec = targetOf(i);
            if (spec != nullptr)
            {
                const auto& item = r.user[static_cast<std::size_t>(set_)][static_cast<std::size_t>(i)];
                dl.text(row.x + 72, cy, Align::Left, focused ? TextStyleId::BodyStrong : TextStyleId::Body, p.text1,
                        m4::targetPath(shell_->params(), *shell_->params().idOf(spec->id)));
                format(*spec, item.bottom, buf, sizeof buf);
                dl.text(row.x + 520, cy, Align::Left, TextStyleId::Body, focused ? p.text1 : p.text2, buf);
                format(*spec, item.top, buf, sizeof buf);
                dl.text(row.x + 672, cy, Align::Left, TextStyleId::Body, focused ? p.text1 : p.text2, buf);
            }
            else
                dl.text(row.x + 72, cy, Align::Left, TextStyleId::Body, p.text3,
                        "\xE2\x80\x94 empty \xC2\xB7 push to assign");
            dl.text(row.x + kListW - 24, cy, Align::Centre, TextStyleId::Title, p.text3, "\xE2\x80\xBA");
        }
        // StripPreview of the focused place.
        StripPreviewModel pm;
        char top[24] = "\xE2\x80\x94", bottom[24] = "\xE2\x80\x94";
        if (const auto* spec = targetOf(place_))
        {
            const auto& item = r.user[static_cast<std::size_t>(set_)][static_cast<std::size_t>(place_)];
            format(*spec, item.top, top, sizeof top);
            format(*spec, item.bottom, bottom, sizeof bottom);
            pm.top01 = static_cast<float>(item.top);
            pm.bottom01 = static_cast<float>(item.bottom);
            pm.empty = false;
        }
        // The finger on the strip, whatever the strip's mode and whether the place has a target (hardware feedback:
        // the preview stayed dark while the strip was in PITCH mode or the place was empty).
        if (const auto& live = m4::stripLive(); live.contact)
            pm.livePosition = live.position;
        shownContact_ = pm.livePosition >= 0;
        pm.topText = top;
        pm.bottomText = bottom;
        StripPreview::draw(dl, ctx, pm);
    }

    bool dirty(double) const override { return m4::stripLive().contact || shownContact_; } // + the frame after release

    bool handle(const io::ControlEvent& e, UiContext& ctx) override
    {
        if (shell_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (ctx.focus.focused >= 0)
                return false; // the rail holds the focus (DELETE): the shell moves it
            if (place_ == 0 && e.value < 0)
                return false; // past place 1: the rail (DELETE)
            place_ = std::clamp(place_ + e.value, 0, 7);
            bind();
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            openSelect();
            return true;
        }
        if (touch::isTouch(e) && e.value == touch::kDown)
        {
            const float x = touch::x(e), y = touch::y(e);
            if (x < kListX || x >= kListX + kListW || y < kRowsY || y >= kRowsY + 8 * kRowH)
                return false;
            const int i = static_cast<int>((y - kRowsY) / kRowH);
            ctx.focus.focused = -1;
            if (i == place_)
                openSelect(); // a tap on the focused place = push
            place_ = i;
            bind();
            return true;
        }
        return false;
    }

    // --- rail: DELETE ---
    bool railFocusEnter(int) override
    {
        return true; // past DELETE: the places
    }
    void railState(int, const RailItemSpec&, RailItemState& st) const override { st.disabled = targetOf(place_) == nullptr; }
    bool railActivate(int, const RailItemSpec&, float, float) override
    {
        const auto* spec = targetOf(place_);
        if (spec == nullptr || shell_ == nullptr)
            return true; // disabled on an empty place
        MessageRequest r;
        r.icon = glyph::Kind::Caution;
        r.title = "DELETE";
        r.lines = {"Remove " + std::string(m4::targetPath(shell_->params(), *shell_->params().idOf(spec->id))) +
                   " from USER" + std::to_string(set_ + 1) + "?"};
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {"DELETE", ButtonStyle::Destructive}};
        r.defaultButton = 0;
        const int userSet = set_, place = place_;
        r.onResult = [userSet, place](int button)
        {
            if (button != 1)
                return;
            auto& s = m2::Session::instance();
            if (auto cmd = rb::deleteItem(s.project(), userSet, place))
                s.execute(std::move(cmd));
        };
        shell_->showMessage(std::move(r));
        return true;
    }

    // --- strip: BOTTOM / TOP VALUE in the target's format ---
    void stripColumn(int column, ParamColumnModel& c) const override
    {
        if (column > 1)
            return;
        const auto* spec = targetOf(place_);
        if (spec == nullptr)
        {
            c.disabled = true;
            c.valueText = "\xE2\x80\x94";
            return;
        }
        const auto& item = ribbon().user[static_cast<std::size_t>(set_)][static_cast<std::size_t>(place_)];
        const double v = column == 0 ? item.bottom : item.top;
        c.value = static_cast<float>(v);
        format(*spec, v, colText_[column], sizeof colText_[column]);
        c.valueText = colText_[column];
        c.locked = false;
        c.disabled = false;
    }
    long long stripSteps(int column, const core::ParamSpec&, bool fine) const override
    {
        if (column > 1)
            return -1;
        const auto* spec = targetOf(place_);
        return spec != nullptr ? EncoderAccel::stepsOf(*spec, fine) : 0; // BOTTOM / TOP step in the target's steps
    }
    bool stripTurn(int column, const core::ParamSpec&, int detents, bool fine) override
    {
        if (column > 1)
            return false;
        const auto* spec = targetOf(place_);
        if (spec == nullptr)
            return true; // disabled
        auto& s = m2::Session::instance();
        const auto& item = s.project().ribbon.user[static_cast<std::size_t>(set_)][static_cast<std::size_t>(place_)];
        const double span = std::max(1e-6, static_cast<double>(spec->maxValue - spec->minValue));
        const double step = static_cast<double>(fine ? spec->fineStep : spec->coarseStep) / span * detents;
        double bottom = item.bottom, top = item.top;
        (column == 0 ? bottom : top) = std::clamp((column == 0 ? bottom : top) + step, 0.0, 1.0);
        if (auto cmd = rb::setItemRange(s.project(), set_, place_, bottom, top))
            s.execute(std::move(cmd));
        return true;
    }

private:
    const core::model::RibbonSettings& ribbon() const { return m2::Session::instance().project().ribbon; }
    const core::ParamSpec* targetOf(int place) const
    {
        if (shell_ == nullptr || place < 0 || place > 7)
            return nullptr;
        const auto& item = ribbon().user[static_cast<std::size_t>(set_)][static_cast<std::size_t>(place)];
        return item.target.empty() ? nullptr : shell_->params().find(item.target);
    }
    // A normalised item value in the target's units and format.
    static void format(const core::ParamSpec& spec, double v01, char* buf, int n)
    {
        const float v = spec.minValue + static_cast<float>(v01) * (spec.maxValue - spec.minValue);
        int len = spec.formatValue(spec.type == core::ParamType::Continuous ? v : std::round(v), buf, n);
        if (len <= 0)
            buf[0] = 0, len = 0;
        if (! spec.unit.empty() && len > 0 && len + 1 + static_cast<int>(spec.unit.size()) < n) // "0.0 st"
            std::snprintf(buf + len, static_cast<std::size_t>(n - len), " %.*s", static_cast<int>(spec.unit.size()),
                          spec.unit.data());
    }
    void bind()
    {
        if (shell_ != nullptr)
            shell_->bindStrip(&kPlaceStrips[set_ & 1][place_ & 7]);
    }
    void openSelect()
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        std::vector<ListItem> items;
        m4::buildTargets(shell_->params(), m4::TargetKind::Strip, state_->scene - 1, state_->selectedTrack - 1, items);
        ListRequest req;
        req.title = "SELECT PARAMETER";
        req.grouped = true;
        req.current = 0;
        const auto* spec = targetOf(place_);
        const int want = spec != nullptr ? static_cast<int>(shell_->params().idOf(spec->id)->value) : -2;
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].value == want || (want == -2 && items[i].group == "PLAYBACK"))
            {
                req.current = want == -2 ? -1 : static_cast<int>(i);
                if (want == -2)
                    focusRow_ = static_cast<int>(i);
                break;
            }
        req.items = std::move(items);
        const int userSet = set_, place = place_;
        const auto* params = &shell_->params();
        req.onChoose = [userSet, place, params](int, const ListItem& it)
        {
            auto& s = m2::Session::instance();
            std::unique_ptr<core::Command> cmd;
            if (it.value < 0)
                cmd = rb::deleteItem(s.project(), userSet, place);
            else
                cmd = rb::assignItem(s.project(), userSet, place,
                                     (*params)[core::ParamId{static_cast<std::uint16_t>(it.value)}].id, *params);
            if (cmd)
                s.execute(std::move(cmd));
        };
        // An empty place opens on the first PLAYBACK row (utility.md): the list focuses `current`, so it is that row
        // without the ✓ (our choice: ListRequest has no separate focus index; NONE keeps the ✓ off).
        if (req.current < 0 && focusRow_ >= 0)
            req.current = focusRow_;
        shell_->showList(std::move(req));
    }

    ShellApi* shell_;
    UiState* state_;
    int set_ = 0, place_ = 0, focusRow_ = -1;
    mutable char colText_[2][24] {};
    mutable bool shownContact_ = false; // the last frame drew a finger (one more frame after release clears it)
};

std::unique_ptr<Widget> makeTouchStrip(const ScreenSpec& s, const WidgetArgs& a)
{
    return std::make_unique<TouchStripBody>(a, s.id == "utility.touchStrip.user2" ? 1 : 0);
}

constexpr RailItemSpec kStripRail[] = {{RailItemSpec::Kind::Button, "DELETE"}};
} // namespace

// Test access: the focused place of a TOUCH STRIP SETTING body (−1 for another body).
int touchStripPlace(const Widget* body) noexcept
{
    const auto* b = dynamic_cast<const TouchStripBody*>(body);
    return b != nullptr ? b->focusedPlace() : -1;
}
} // namespace ui

APP_REGISTER_WIDGET("TouchStripSetting", &ui::makeTouchStrip)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "utility.touchStrip.user1",
                                    .title = "TOUCH STRIP SETTING (USER1)",
                                    .parent = "utility",
                                    .body = ui::BodyTemplate::RailContent,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = ui::kStripRail,
                                    .bodyComponent = "TouchStripSetting",
                                    .strip = {"USER1", {"ribbon.userBottom", "ribbon.userTop", "", "", "", ""}}}))
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "utility.touchStrip.user2",
                                    .title = "TOUCH STRIP SETTING (USER2)",
                                    .parent = "utility",
                                    .body = ui::BodyTemplate::RailContent,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = ui::kStripRail,
                                    .bodyComponent = "TouchStripSetting",
                                    .strip = {"USER2", {"ribbon.userBottom", "ribbon.userTop", "", "", "", ""}}}))
// UTILITY rows (SettingsSpec, UID-220; utility.md: section PROJECT SETTING (COMMON), rows 1–2 open the pages). The
// UTILITY list itself is ui/screens/utility's; until it draws these rows the pages are reached from the shell's router.
APP_REGISTER_SETTINGS((ui::SettingsRowEntry{"utility", "projectCommon", 10,
                                            {ui::SettingsRowSpec::Kind::Action, "TOUCH STRIP SETTING (USER1)", {},
                                             "utility.touchStrip.user1"}}))
APP_REGISTER_SETTINGS((ui::SettingsRowEntry{"utility", "projectCommon", 20,
                                            {ui::SettingsRowSpec::Kind::Action, "TOUCH STRIP SETTING (USER2)", {},
                                             "utility.touchStrip.user2"}}))
