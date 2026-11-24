// The shell gaps the screens needed closed — engine feeds for body widgets, touch drags to the body that
// took the down (and to a strip column a body hands them to, UID-163), rail items running screen code / holes /
// custom-drawn items (RailOwner), custom popups and popup factories, GridPopup wrap / SHIFT row jump / palette hook,
// ListPopup track chips (UID-842), the BYPASS mode (UI-221) and the lamp model (UI-150, UID-175).
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/unit/ui/ShellHarness.h"
#include "ui/shell/Layout.h"
#include "ui/shell/PopupFactory.h"
#include "ui/shell/Root.h"
#include "ui/widgets/GridPopup.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/ModuleCard.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace L = ui::layout;
using Kind = io::ControlEvent::Kind;

namespace
{
// A body widget that records what reaches it and takes part in its rail and the lamps.
struct Probe final : ui::Widget, ui::RailOwner, ui::LampSource
{
    explicit Probe(const ui::WidgetArgs& a) : args(a) {}
    ui::RectF bounds() const noexcept override { return L::bodyRects(ui::BodyTemplate::RailContent).content; }
    void build(ui::DrawList& dl, const ui::UiContext&) const override
    {
        dl.text(600, 300, ui::Align::Left, ui::TextStyleId::Body, 0xFFFFFFFFu, "PROBE");
    }
    bool handle(const io::ControlEvent& e, ui::UiContext&) override
    {
        if (ui::touch::isTouch(e))
        {
            touches.push_back(e.value);
            if (e.value == ui::touch::kDown && dragToColumn >= 0)
                args.shell->dragColumn(dragToColumn);
            return true;
        }
        return false; // the rotary goes to the rail
    }
    // RailOwner
    bool railActivate(int index, const ui::RailItemSpec&, float x, float) override
    {
        if (index == 2)
            return false; // LOOP: the shell's default toggle
        activated.push_back(index);
        lastX = x;
        return true;
    }
    void railState(int index, const ui::RailItemSpec&, ui::RailItemState& st) const override
    {
        if (index == 3)
            st.value = "CUSTOM";
    }
    bool railDraw(int index, const ui::RailItemSpec&, const ui::RailItemState& st, ui::DrawList& dl,
                  const ui::UiContext&) const override
    {
        if (index != 3)
            return false;
        dl.text(32, 530, ui::Align::Left, ui::TextStyleId::Body, 0xFFFFFFFFu, st.focused ? "CARD*" : "CARD");
        return true;
    }
    bool railRect(int index, const ui::RailItemSpec&, ui::RectF& r) const override
    {
        if (index != 3)
            return false;
        r = {16, 506, 192, 60};
        return true;
    }
    bool railFocusEnter(int detents) override
    {
        entered = detents;
        return true;
    }
    // LampSource
    void lamps(io::LampFrame& f, double) const override { f[io::Control::Back] = {io::LampMode::On, 0x123456, 0}; }

    ui::WidgetArgs args;
    std::vector<int> touches, activated;
    int dragToColumn = -1, entered = 0;
    float lastX = 0;
};
Probe* g_probe = nullptr;

constexpr ui::RailItemSpec kProbeRail[] = {{ui::RailItemSpec::Kind::Button, ""},
                                           {ui::RailItemSpec::Kind::Button, "ACT"},
                                           {ui::RailItemSpec::Kind::Toggle, "LOOP", {}, "playback.loop"},
                                           {ui::RailItemSpec::Kind::Option, "CARD"}};
const ui::ScreenSpec kProbe{.id = "probe",
                            .title = "PROBE",
                            .parent = "home",
                            .body = ui::BodyTemplate::RailContent,
                            .contextBar = ui::ContextBarKind::TrackTypeBypass,
                            .rail = kProbeRail,
                            .bodyComponent = "probe",
                            .strip = {"PROBE", {"track.volume", "track.pan", "", "", "", ""}}};

struct Gaps
{
    ui::ScreenRegistry screens;
    fakes::FakeEngineBridge bridge;
    std::unique_ptr<ui::Shell> shell;
    ui::DrawList dl;
    double now = 1000;
    std::vector<std::string> log;

    explicit Gaps(bool withBridge = true)
    {
        screens.add(kProbe);
        ui::ShellConfig cfg{core::builtinParams(), uitest::fixedMeasure(), withBridge ? &bridge : nullptr};
        cfg.screens = &screens;
        cfg.log = [this](const std::string& l) { log.push_back(l); };
        const auto builtin = ui::WidgetRegistry::builtin().dispatcher();
        cfg.factory = [builtin](const ui::ScreenSpec& s, const ui::WidgetArgs& a) -> std::unique_ptr<ui::Widget>
        {
            if (s.bodyComponent != "probe")
                return builtin(s, a);
            auto p = std::make_unique<Probe>(a);
            g_probe = p.get();
            return p;
        };
        shell = std::make_unique<ui::Shell>(std::move(cfg));
        dl.reserve(4096, 32768);
        frame();
    }
    ui::DrawList& frame(double advanceMs = 500)
    {
        now += advanceMs;
        dl.clear();
        shell->frame(dl, now);
        return dl;
    }
    void send(Kind k, io::Control c, int v = 0)
    {
        shell->dispatch({k, c, static_cast<std::int16_t>(v), io::kSourceVirtual, now});
    }
    void touch(float cx, float cy, std::int16_t phase)
    {
        io::ControlEvent e{Kind::Touch, io::Control::Screen, phase, io::kSourceVirtual, now};
        e.x = static_cast<std::int16_t>(cx / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(cy / ui::touch::kToCanvas + 0.5f);
        shell->dispatch(e);
    }
    void tap(float cx, float cy)
    {
        touch(cx, cy, ui::touch::kDown);
        touch(cx, cy, ui::touch::kUp);
    }
    float value(std::string_view id)
    {
        const auto* p = core::builtinParams().find(id);
        return shell->values().get(*p, shell->state().trackScope());
    }
    bool drewText(std::string_view text) const
    {
        for (const auto& c : dl.commands())
            if (c.op == ui::DrawCmd::Op::Text && dl.textOf(c) == text)
                return true;
        return false;
    }
    void openProbe()
    {
        REQUIRE(shell->open("probe"));
        frame();
        REQUIRE(g_probe != nullptr);
    }
};

class TestPopup final : public ui::PopupWidget
{
public:
    TestPopup() : PopupWidget(ui::PopupSpec::Kind::Modal) {}
    ui::RectF bounds() const noexcept override { return place(L::modalPanel); }
    void build(ui::DrawList& dl, const ui::UiContext&) const override
    {
        dl.text(200, 200, ui::Align::Left, ui::TextStyleId::Body, 0xFFFFFFFFu, "CUSTOM POPUP");
    }
    bool handle(const io::ControlEvent& e, ui::UiContext&) override
    {
        if (e.control == io::Control::Back && e.kind == Kind::Press)
            cancel();
        return true;
    }
};
} // namespace

TEST_CASE("shell.gaps: body widgets reach the engine feeds and the frame's engine events through the shell",
          "[ui][shell]")
{
    Gaps g;
    g.openProbe();
    REQUIRE(g_probe->args.feeds == &g.shell->feeds());
    CHECK(&g_probe->args.shell->feeds() == &g.shell->feeds());
    g.bridge.meterFeed.trackPeak[2] = {0.5f, 0.5f}; // track 3 at −6 dB
    g.bridge.playheadFeed.trackPosition.fill(-1);
    g.bridge.playheadFeed.trackPosition[4] = 0.25f;
    g.bridge.events.push_back({engine::EngineEvent::Kind::RecordedHit, 3, 5, 100, 0});
    g.frame(16);
    const auto& f = g.shell->feeds();
    CHECK(f.track(3).db[0] > -6.1f);
    CHECK(f.lastHitMs(3) == g.now); // a rise ≥ 6 dB = a trigger flash (UID-503)
    CHECK(f.position(5) == 0.25f);
    REQUIRE(g.shell->engineEvents().size() == 1);
    CHECK(g.shell->engineEvents()[0].track == 3);
    CHECK(g.bridge.events.empty()); // drained by the shell: the composition root no longer pops them
    g.frame(16);
    CHECK(g.shell->engineEvents().empty()); // valid for one frame
    // More than a frame's capacity: the rest stays queued for the next frame (lossless).
    for (int i = 0; i < 300; ++i)
        g.bridge.events.push_back({engine::EngineEvent::Kind::ErasedStep, 1, static_cast<std::uint8_t>(i & 15), 0, 0});
    g.frame(16);
    CHECK(g.shell->engineEvents().size() == ui::EngineFeeds::kMaxEvents);
    g.frame(16);
    CHECK(g.shell->engineEvents().size() == 300 - ui::EngineFeeds::kMaxEvents);
    CHECK(f.eventsSeen() == 301);

    // Without a bridge, tests and the host preview inject snapshots and events.
    Gaps solo(false);
    solo.openProbe();
    engine::MeterFeed m{};
    m.masterPeak = {1.0f, 1.0f};
    solo.shell->feeds().inject(m, {});
    solo.shell->feeds().injectEvent({engine::EngineEvent::Kind::SwitchDone, 0, 0, 17, 0});
    solo.frame(16);
    CHECK(solo.shell->feeds().master().db[0] > -0.1f);
    REQUIRE(solo.shell->engineEvents().size() == 1);
    CHECK(solo.shell->engineEvents()[0].value == 17);
}

TEST_CASE("shell.gaps: a touch's drag and release reach the body that took its down; a body can hand the drag to a "
          "strip column (UI-209, UID-163)",
          "[ui][shell]")
{
    Gaps g;
    g.openProbe();
    g.touch(600, 300, ui::touch::kDown);
    g.touch(620, 320, ui::touch::kDrag);
    g.touch(640, 340, ui::touch::kDrag);
    g.touch(640, 340, ui::touch::kUp);
    CHECK(g_probe->touches == std::vector<int>{ui::touch::kDown, ui::touch::kDrag, ui::touch::kDrag, ui::touch::kUp});
    // A touch that started on the rail does not drag the body.
    g_probe->touches.clear();
    const auto act = L::railItem(1);
    g.touch(act.x + 20, act.y + 20, ui::touch::kDown);
    g.touch(act.x + 20, act.y + 200, ui::touch::kDrag);
    CHECK(g_probe->touches.empty());
    g.touch(act.x + 20, act.y + 200, ui::touch::kUp);
    // The body hands the drag to column 1 (a knob glyph): 8 logical px up per detent, the body sees no drag.
    g_probe->touches.clear();
    g_probe->dragToColumn = 0;
    const float before = g.value("track.volume");
    g.touch(600, 400, ui::touch::kDown);
    CHECK(g.shell->touchDragColumn() == 0);
    g.touch(600, 400 - 16 * ui::touch::kToCanvas, ui::touch::kDrag);
    CHECK(g.value("track.volume") == before + 2);
    g.touch(600, 400 - 16 * ui::touch::kToCanvas, ui::touch::kUp);
    CHECK(g.shell->touchDragColumn() == -1);
    CHECK(g_probe->touches == std::vector<int>{ui::touch::kDown, ui::touch::kUp});
    // dragColumn outside a body touch does nothing.
    g.shell->dragColumn(1);
    CHECK(g.shell->touchDragColumn() == -1);
    // A strip column drag keeps working.
    const float pan = g.value("track.pan");
    const float colX = L::columnX(1) + 20;
    g.touch(colX, 700, ui::touch::kDown);
    g.touch(colX, 700 - 8 * ui::touch::kToCanvas, ui::touch::kDrag);
    g.touch(colX, 700 - 8 * ui::touch::kToCanvas, ui::touch::kUp);
    CHECK(g.value("track.pan") == pan + 1);
}

TEST_CASE("shell.gaps: rail items run screen code, holes are skipped, items draw themselves (RailOwner)", "[ui][shell]")
{
    Gaps g;
    g.openProbe();
    CHECK(g.drewText("ACT"));
    CHECK(g.drewText("LOOP"));
    CHECK(g.drewText("CARD")); // drawn by the body (railDraw), not the OptionSelector
    CHECK_FALSE(g.drewText("CUSTOM"));
    CHECK(g.shell->railFocus() == -1); // the body has the focus
    // The rotary: the body declines, the rail takes the focus on its first item (the hole is skipped).
    g.send(Kind::Turn, io::Control::Selector, 1);
    CHECK(g.shell->railFocus() == 1);
    g.send(Kind::Turn, io::Control::Selector, -1);
    CHECK(g.shell->railFocus() == 1); // never onto the hole
    g.send(Kind::Turn, io::Control::Selector, 2);
    CHECK(g.shell->railFocus() == 3);
    g.frame();
    CHECK(g.drewText("CARD*"));                   // the custom item draws its focus
    g.send(Kind::Turn, io::Control::Selector, 2); // past the end: the body takes the focus, two detents in
    CHECK(g.shell->railFocus() == -1);
    CHECK(g_probe->entered == 2);
    // Push on a focused item runs the screen's action (x < 0 = push), a tap passes the point.
    g.send(Kind::Turn, io::Control::Selector, 1);
    g.send(Kind::Press, io::Control::SelectorPush, 100);
    CHECK(g_probe->activated == std::vector<int>{1});
    CHECK(g_probe->lastX < 0);
    const auto act = L::railItem(1);
    g.tap(act.x + 30, act.y + 30);
    CHECK(g_probe->activated == std::vector<int>{1, 1});
    CHECK(g_probe->lastX > 0);
    // The owner declines LOOP: the shell toggles its bound parameter.
    const auto loop = L::railItem(2);
    g.tap(loop.x + 30, loop.y + 30);
    CHECK(g.value("playback.loop") == 1.0f);
    // The custom rect is the hit area: inside the slot but outside the rect = nothing; a tap on the hole = nothing.
    g.tap(40, 590);
    g.tap(40, L::railItem(0).y + 30);
    CHECK(g_probe->activated == std::vector<int>{1, 1});
    g.tap(40, 520);
    CHECK(g_probe->activated == std::vector<int>{1, 1, 3});
}

TEST_CASE("shell.gaps: custom popup widgets and popup factories by id", "[ui][shell][popup]")
{
    Gaps g;
    g.openProbe();
    auto* p = g.shell->showPopup(std::make_unique<TestPopup>());
    REQUIRE(p != nullptr);
    CHECK(g.shell->topPopup() == p);
    g.frame();
    CHECK(g.drewText("CUSTOM POPUP"));
    g.send(Kind::Press, io::Control::Back, 100);
    CHECK(g.shell->topPopup() == nullptr);
    // Nesting depth (UID-914): the third modal popup is refused.
    CHECK(g.shell->showPopup(std::make_unique<TestPopup>()) != nullptr);
    CHECK(g.shell->showPopup(std::make_unique<TestPopup>()) != nullptr);
    CHECK(g.shell->showPopup(std::make_unique<TestPopup>()) == nullptr);
    g.shell->dismissAll();
    // A factory registered for a popup id builds the popup that open(id) shows.
    static int built = 0;
    ui::PopupRegistry::builtin().add({.kind = ui::PopupSpec::Kind::Grid, .id = "test.uifw3.factory", .title = "T"});
    ui::PopupFactoryRegistry::builtin().add(
        "test.uifw3.factory",
        [](const ui::PopupSpec& s, ui::ShellApi& sh) -> std::unique_ptr<ui::PopupWidget>
        {
            ++built;
            CHECK(s.id == "test.uifw3.factory");
            CHECK(sh.current().id == "probe");
            return std::make_unique<TestPopup>();
        });
    REQUIRE(g.shell->open("test.uifw3.factory"));
    CHECK(built == 1);
    CHECK(dynamic_cast<TestPopup*>(g.shell->topPopup()) != nullptr);
    CHECK(g.shell->router().current().id == "probe");
}

TEST_CASE("shell.gaps: GridPopup wraps around, SHIFT + turn jumps a row, the cell hook gets the palette", "[ui][popup]")
{
    Gaps g;
    std::vector<std::uint32_t> palettes; // the hook's palette background (the pointer is valid during the call)
    std::vector<int> cells;
    ui::GridRequest r;
    r.title = "GRID";
    for (int i = 0; i < 7; ++i)
        r.items.push_back({"ITEM", "icon"});
    r.current = 0;
    r.drawCell = [&](ui::DrawList&, const ui::GridCell& c)
    {
        palettes.push_back(c.palette != nullptr ? c.palette->bg : 0u);
        cells.push_back(c.item);
        CHECK(c.popup.w == L::gridPopup.w);
        CHECK(c.icon48.w == 48);
    };
    g.shell->showGrid(std::move(r));
    auto* grid = dynamic_cast<ui::GridPopup*>(g.shell->topPopup());
    REQUIRE(grid != nullptr);
    g.send(Kind::Turn, io::Control::Selector, -1);
    CHECK(grid->focusedItem() == 6); // wraps to the last
    g.send(Kind::Turn, io::Control::Selector, 1);
    CHECK(grid->focusedItem() == 0); // and back to the first
    g.send(Kind::Press, io::Control::Shift, 0);
    g.send(Kind::Turn, io::Control::Selector, 1);
    CHECK(grid->focusedItem() == 5); // one row
    g.send(Kind::Turn, io::Control::Selector, 1);
    CHECK(grid->focusedItem() == 6); // clamped at the end
    g.send(Kind::Release, io::Control::Shift, 0);
    CHECK(g.shell->textKey(ui::TextKey::Right));
    CHECK(grid->focusedItem() == 0); // ← → wrap too
    g.frame();
    CHECK(cells == std::vector<int>{0, 1, 2, 3, 4, 5, 6});
    CHECK(palettes.front() == ui::theme::palette(true).bg);
    g.shell->state().dark = false;
    palettes.clear();
    g.frame();
    CHECK(palettes.front() == ui::theme::palette(false).bg); // the light palette reaches the hook
}

TEST_CASE("shell.gaps: ListPopup rows carry track chips in the track colour (UID-842)", "[ui][popup]")
{
    Gaps g;
    ui::ListRequest r;
    r.title = "SOURCE";
    for (int t = 1; t <= 3; ++t)
    {
        r.items.push_back({t == 1 ? "TRACK 1" : t == 2 ? "TRACK 2" : "TRACK 3", {}, {}, t});
        r.chips.push_back({t, t == 2 ? 9 : 0, t == 3, false});
    }
    g.shell->showList(std::move(r));
    g.frame();
    auto chip = [&](std::uint32_t argb)
    {
        for (const auto& c : g.dl.commands())
            if (c.op == ui::DrawCmd::Op::Fill && c.rect.w == 32 && c.rect.h == 24 && c.argb == argb)
                return true;
        return false;
    };
    CHECK(chip(ui::theme::trackColour(1)));
    CHECK(chip(ui::theme::trackColour(9)));       // the pad colour, not the track number
    CHECK(chip(ui::theme::trackColour(3, true))); // an empty track: the dim variant
    CHECK(g.drewText("2"));
}

TEST_CASE("shell.gaps: BYPASS is a mode toggle of the shell that ends with its screen (UI-221)", "[ui][shell]")
{
    Gaps g;
    g.openProbe();
    const auto b = L::bypassBox;
    g.tap(b.x + b.w / 2, b.y + b.h / 2);
    CHECK(g.shell->state().bypassMode);
    g.frame();
    CHECK(g.shell->contextBar().model().bypass); // the LED bar lights
    g.tap(b.x + b.w / 2, b.y + b.h / 2);
    CHECK_FALSE(g.shell->state().bypassMode);
    g.tap(b.x + b.w / 2, b.y + b.h / 2);
    REQUIRE(g.shell->state().bypassMode);
    g.send(Kind::Press, io::Control::Home, 100);
    g.frame();
    CHECK_FALSE(g.shell->state().bypassMode);
}

TEST_CASE("shell.gaps: the lamp model lights what the screen shows (UI-150, UID-175)", "[ui][shell]")
{
    Gaps g;
    auto& st = g.shell->state();
    st.tracks[4].hasContent = true;
    g.frame();
    auto lamps = [&](io::Control c) { return g.shell->lamps()[c]; };
    CHECK(lamps(io::pad(5)).mode == io::LampMode::Dim); // not the selected track (io::padLamp's ladder)
    CHECK(lamps(io::pad(5)).rgb == ui::theme::kLedColours[4]); // the LED colour (a flash blends in it)
    CHECK(lamps(io::pad(6)).mode == io::LampMode::Off); // an empty track is dark
    g.send(Kind::Press, io::pad(5), 100);
    g.frame(16);
    CHECK(lamps(io::pad(5)).mode == io::LampMode::Flash); // a hit flashes white and fades back (UI-151)
    CHECK(io::resolve(lamps(io::pad(5)), g.now - 16).rgb == io::lampcolour::white);
    CHECK(io::resolve(lamps(io::pad(5)), g.now + 2000).rgb == ui::theme::kLedColours[4]);
    CHECK(io::resolve(lamps(io::pad(5)), g.now + 2000).level == 255); // the hit selected it: full
    st.tracks[4].soloed = true; // a solo: the soloed pad breathes, the other tracks fall to the minimum
    st.tracks[0].hasContent = true;
    g.frame(2000);
    CHECK(io::resolve(lamps(io::pad(5)), g.now).level > 0);
    CHECK(lamps(io::pad(1)).mode == io::LampMode::Faint);
    st.tracks[4].soloed = false;
    st.playing = true;
    g.send(Kind::Press, io::step(3), 100);
    g.frame(16);
    CHECK(io::resolve(lamps(io::Control::Play), g.now).level == 255);
    CHECK(lamps(io::step(3)).mode == io::LampMode::On);
    g.send(Kind::Release, io::step(3), 0);
    g.frame(16);
    CHECK(lamps(io::step(3)).mode == io::LampMode::Off);
    // A body that is also a LampSource refines the frame for its screen.
    CHECK(lamps(io::Control::Back).mode == io::LampMode::Off);
    g.openProbe();
    CHECK(lamps(io::Control::Back).rgb == 0x123456);
    // The composition root reads it through Root::lamps().
    auto root = ui::createRoot({core::builtinParams(), uitest::fixedMeasure(), nullptr, {}});
    ui::DrawList dl;
    root->frame(dl, 1000);
    REQUIRE(root->lamps() != nullptr);
    CHECK((*root->lamps())[io::pad(1)].mode != io::LampMode::Off); // the test-tone track has content
}

TEST_CASE("shell.gaps: a double-tap on a column opens no popup; the second touch drags the column like the first "
          "(no typed value entry)",
          "[ui][shell][popup]")
{
    uitest::Harness h;
    const auto col = L::column(0);
    const float x = col.x + 40, y = col.y + 40;
    h.touch(x, y, ui::touch::kDown);
    h.touch(x, y, ui::touch::kUp);
    h.now += 120;
    h.touch(x, y, ui::touch::kDown);
    CHECK(h.shell->topPopup() == nullptr);
    // The second down took the column: a vertical drag turns its value (8 lpx per detent).
    const float v0 = h.value("track.volume");
    h.touch(x, y - 16 * ui::touch::kToCanvas, ui::touch::kDrag);
    h.touch(x, y - 16 * ui::touch::kToCanvas, ui::touch::kUp);
    CHECK(h.value("track.volume") == v0 + 2);
    CHECK(h.shell->topPopup() == nullptr);
}

TEST_CASE("shell.gaps: popups fade out on close (motion.base), input ends the fade (UID-065)", "[ui][popup]")
{
    uitest::Harness h;
    ui::ListRequest r;
    r.title = "LIST";
    r.items = {{"A", {}, {}, 0}, {"B", {}, {}, 1}};
    h.shell->showList(r);
    h.frame();
    h.press(io::Control::Back);
    CHECK(h.shell->topPopup() == nullptr);
    CHECK(h.shell->closingPopups() == 1);
    h.frame(16);
    CHECK(h.drewText("LIST")); // still drawn, fading
    CHECK(h.shell->dirty(h.now));
    h.frame(200);
    CHECK(h.shell->closingPopups() == 0);
    CHECK_FALSE(h.drewText("LIST"));
    h.shell->showList(r);
    h.frame();
    h.press(io::Control::Back);
    h.turn(io::knob(1), 1); // the next input jumps the fade to its end
    CHECK(h.shell->closingPopups() == 0);
}

TEST_CASE("shell.gaps: a long enum in a setting popup opens a nested list (UID-914)", "[ui][popup]")
{
    uitest::Harness h;
    static constexpr std::string_view targets[] = {"MASTER", "T1", "T2", "T3", "T4",  "T5",
                                                   "T6",     "T7", "T8", "T9", "T10", "T11"};
    int committedRow = -1, committedOption = -1;
    ui::SettingRequest req;
    req.title = "SETTING";
    req.rows.push_back({"INSERT TRACK", {std::begin(targets), std::end(targets)}, 0});
    req.rows.push_back({"ORDER", {"PRE", "POST"}, 0});
    req.onCommit = [&](int row, int option, float)
    {
        committedRow = row;
        committedOption = option;
    };
    h.shell->showSetting(std::move(req));
    h.press(io::Control::SelectorPush);
    REQUIRE(h.shell->popupDepth() == 2);
    auto* list = dynamic_cast<ui::ListPopup*>(h.shell->topPopup());
    REQUIRE(list != nullptr);
    h.frame();
    CHECK(list->dx == L::nestedOffset); // 24 lpx right and down of its parent
    h.turn(io::Control::Selector, 3);
    h.press(io::Control::SelectorPush);
    CHECK(h.shell->popupDepth() == 1);
    CHECK(committedRow == 0);
    CHECK(committedOption == 3);
    // A short enum still edits in place.
    h.turn(io::Control::Selector, 1);
    h.press(io::Control::SelectorPush);
    CHECK(h.shell->popupDepth() == 1);
}

TEST_CASE("shell.gaps: the validator accepts rail holes and rejects a hole with a target", "[ui][descriptor]")
{
    static constexpr ui::RailItemSpec bad[] = {{ui::RailItemSpec::Kind::Button, "", "home"}};
    ui::ScreenRegistry r;
    r.add({.id = "home",
           .title = "HOME",
           .parent = {},
           .body = ui::BodyTemplate::Full,
           .contextBar = ui::ContextBarKind::Menu});
    r.add(kProbe);
    r.add({.id = "bad",
           .title = "BAD",
           .parent = "home",
           .body = ui::BodyTemplate::RailContent,
           .contextBar = ui::ContextBarKind::Menu,
           .rail = bad});
    const auto problems = r.validate({}, core::builtinParams());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("bad") != std::string::npos);
    CHECK(problems[0].find("rail hole") != std::string::npos);
}

TEST_CASE("shell.gaps: TRACK MENU in BYPASS mode toggles a module's bypass instead of opening it (UI-221)",
          "[ui][shell]")
{
    ui::ShellConfig cfg{core::builtinParams(), uitest::fixedMeasure()};
    ui::Shell shell(std::move(cfg));
    ui::DrawList dl;
    double now = 1000;
    auto frame = [&]
    {
        now += 500;
        dl.clear();
        shell.frame(dl, now);
    };
    auto tap = [&](float x, float y)
    {
        io::ControlEvent e{Kind::Touch, io::Control::Screen, ui::touch::kDown, io::kSourceVirtual, now};
        e.x = static_cast<std::int16_t>(x / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(y / ui::touch::kToCanvas + 0.5f);
        shell.dispatch(e);
        e.value = ui::touch::kUp;
        shell.dispatch(e);
    };
    shell.state().tracks[0].hasContent = true;
    REQUIRE(shell.open("track.menu"));
    frame();
    const auto* bypass = core::builtinParams().find("playback.bypass");
    REQUIRE(bypass != nullptr);
    const auto card = ui::ModuleCard::rect(0); // PLAYBACK
    const auto b = L::bypassBox;
    tap(b.x + b.w / 2, b.y + b.h / 2);
    REQUIRE(shell.state().bypassMode);
    frame();
    tap(card.x + 40, card.y + 40);
    tap(card.x + 40, card.y + 40); // a second tap would open PLAYBACK outside the mode
    frame();
    CHECK(shell.router().current().id == "track.menu");
    CHECK(shell.values().get(*bypass, shell.state().trackScope()) == 0.0f); // toggled twice
    tap(card.x + 40, card.y + 40);
    CHECK(shell.values().get(*bypass, shell.state().trackScope()) == 1.0f);
    // The LFO card (the sixth) is bypassable too.
    const auto* lfo = core::builtinParams().find("lfo.bypass");
    REQUIRE(lfo != nullptr);
    const auto lfoCard = ui::ModuleCard::rect(5);
    tap(lfoCard.x + 40, lfoCard.y + 40);
    CHECK(shell.values().get(*lfo, shell.state().trackScope()) == 1.0f);
    CHECK(shell.router().current().id == "track.menu");
    // Mode off: a bypassed module's setting screen does not open (the manual's BYPASS button, p.36); once it is not
    // bypassed, the first tap focuses and the second opens.
    tap(b.x + b.w / 2, b.y + b.h / 2);
    CHECK_FALSE(shell.state().bypassMode);
    tap(card.x + 40, card.y + 40);
    tap(card.x + 40, card.y + 40);
    frame();
    CHECK(shell.router().current().id == "track.menu");
    CHECK(shell.values().get(*bypass, shell.state().trackScope()) == 1.0f);
    {
        io::ControlEvent push{Kind::Press, io::Control::SelectorPush, 1, io::kSourceVirtual, now};
        shell.dispatch(push); // the push on the focused bypassed card does not open it either
        frame();
        CHECK(shell.router().current().id == "track.menu");
    }
    shell.values().set(*bypass, shell.state().trackScope(), 0.0f);
    tap(card.x + 40, card.y + 40);
    frame();
    CHECK(shell.router().current().id == "track.playback");
}

TEST_CASE("shell.gaps: a MIDI track's BYPASS covers CC1, CC2 and LFO, not NOTE (the manual, p.36)", "[ui][shell]")
{
    ui::ShellConfig cfg{core::builtinParams(), uitest::fixedMeasure()};
    ui::Shell shell(std::move(cfg));
    ui::DrawList dl;
    double now = 1000;
    auto frame = [&]
    {
        now += 500;
        dl.clear();
        shell.frame(dl, now);
    };
    auto tap = [&](float x, float y)
    {
        io::ControlEvent e{Kind::Touch, io::Control::Screen, ui::touch::kDown, io::kSourceVirtual, now};
        e.x = static_cast<std::int16_t>(x / ui::touch::kToCanvas + 0.5f);
        e.y = static_cast<std::int16_t>(y / ui::touch::kToCanvas + 0.5f);
        shell.dispatch(e);
        e.value = ui::touch::kUp;
        shell.dispatch(e);
    };
    const auto& p = core::builtinParams();
    const auto scope = shell.state().trackScope();
    shell.values().set(*p.find("track.mode"), scope, 1.0f); // MIDI
    REQUIRE(shell.open("track.menu"));
    frame();
    const auto b = L::bypassBox;
    tap(b.x + b.w / 2, b.y + b.h / 2);
    REQUIRE(shell.state().bypassMode);
    frame();
    const auto* noteBypass = p.find("midi.noteBypass");
    const auto* cc1Bypass = p.find("midi.cc1Bypass");
    if (noteBypass == nullptr || cc1Bypass == nullptr)
        SKIP("midi.* not registered (no MIDI catalogue built in)");
    const auto note = ui::ModuleCard::rect(0), cc1 = ui::ModuleCard::rect(1);
    tap(cc1.x + 40, cc1.y + 40);
    CHECK(shell.values().get(*cc1Bypass, scope) == 1.0f); // CC1 toggles
    tap(note.x + 40, note.y + 40);
    tap(note.x + 40, note.y + 40);
    frame();
    CHECK(shell.values().get(*noteBypass, scope) == 0.0f); // NOTE is not bypassable: it focuses and opens
    CHECK(shell.router().current().id == "track.note");
}
