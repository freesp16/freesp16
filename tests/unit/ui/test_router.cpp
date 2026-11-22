// Router (UID-206, UI-400…408, UID-300, UI-009): breadcrumbs for every registered screen, BACK one level up the
// breadcrumb, HOME from any depth, popups closed first, feature-flag variants, focus memory.
#include "ui/shell/Router.h"
#include "ui/shell/Shell.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
ui::ScreenRegistry graph()
{
    ui::ScreenRegistry r;
    ui::installFallbackScreens(r);
    r.add({.id = "track.playback",
           .title = "PLAYBACK",
           .parent = "track.menu",
           .body = ui::BodyTemplate::RailContent,
           .contextBar = ui::ContextBarKind::TrackContext});
    r.add({.id = "track.playback.setSlice",
           .title = "SET SLICE",
           .parent = "track.playback",
           .body = ui::BodyTemplate::RailContent,
           .contextBar = ui::ContextBarKind::TrackContext});
    r.add({.id = "utility.midiMonitor",
           .title = "MIDI MONITOR",
           .parent = "utility",
           .body = ui::BodyTemplate::Full,
           .contextBar = ui::ContextBarKind::Menu,
           .rail = {},
           .bodyComponent = {},
           .strip = {},
           .meter = {},
           .featureFlag = "ui.midiMonitor"});
    return r;
}

struct FakePresenter final : ui::PopupPresenter
{
    int open = 0, presented = 0;
    bool present(const ui::PopupSpec&) override { return ++open, ++presented, true; }
    bool popupOpen() const override { return open > 0; }
    void dismissTop() override { --open; }
    void dismissAll() override { open = 0; }
};

std::string joined(std::span<const std::string_view> crumbs)
{
    std::string s;
    for (auto c : crumbs)
        s += (s.empty() ? "" : " > ") + std::string(c);
    return s;
}
} // namespace

TEST_CASE("router: breadcrumbs for every registered screen start at HOME (UID-300)", "[ui][router]")
{
    auto& builtin = ui::ScreenRegistry::builtin();
    ui::installFallbackScreens(builtin);
    ui::FeatureFlags all;
    ui::Router router(builtin, &all);
    for (const auto& s : builtin.all())
    {
        if (! s.featureFlag.empty())
            continue;
        std::array<std::string_view, ui::Router::kMaxDepth> out{};
        std::size_t depth = 0;
        INFO(std::string(s.id));
        if (s.id.rfind("contract.", 0) == 0)
            continue; // the contract test fixtures are a separate graph without HOME
        REQUIRE(router.breadcrumbOf(s.id, out, depth));
        CHECK(out[0] == "HOME");
        CHECK(out[depth - 1] == s.title);
    }
}

TEST_CASE("router: open, BACK one level up the breadcrumb, HOME from any depth (UI-400)", "[ui][router]")
{
    auto r = graph();
    ui::Router router(r);
    REQUIRE(router.hasCurrent());
    CHECK(router.current().id == "home");
    CHECK(joined(router.breadcrumb()) == "HOME");
    CHECK(router.open("track.menu"));
    CHECK(router.open("track.playback"));
    CHECK(router.open("track.playback.setSlice"));
    CHECK(router.lastDirection() == 1);
    CHECK(joined(router.breadcrumb()) == "HOME > TRACK MENU > PLAYBACK > SET SLICE");
    router.back();
    CHECK(router.current().id == "track.playback");
    CHECK(router.lastDirection() == -1);
    router.home();
    CHECK(router.current().id == "home");
    router.back(); // HOME is the base
    CHECK(router.current().id == "home");
    // A menu target opened from a deep screen: the breadcrumb is its own parent chain.
    CHECK(router.open("track.playback"));
    CHECK(router.open("mixer"));
    CHECK(joined(router.breadcrumb()) == "HOME > MIXER");
    CHECK_FALSE(router.open("no.such.screen"));
    CHECK(router.current().id == "mixer");
}

TEST_CASE("router: BACK and HOME close popups first (UI-005, UID-171)", "[ui][router]")
{
    auto r = graph();
    ui::Router router(r);
    FakePresenter presenter;
    router.setPresenter(&presenter);
    router.open("track.menu");
    REQUIRE(router.showPopup(ui::PopupSpec{ui::PopupSpec::Kind::List, "track.type", "TRACK TYPE"}));
    router.back();
    CHECK(presenter.open == 0);
    CHECK(router.current().id == "track.menu");
    router.showPopup(ui::PopupSpec{ui::PopupSpec::Kind::List, "x", "X"});
    router.showPopup(ui::PopupSpec{ui::PopupSpec::Kind::Message, "y", "Y"});
    router.home();
    CHECK(presenter.open == 0);
    CHECK(router.current().id == "home");
    // A flagged popup stays closed while its flag is off.
    CHECK_FALSE(router.showPopup(
        ui::PopupSpec{.kind = ui::PopupSpec::Kind::Toast, .id = "t", .title = "T", .featureFlag = "ext.x"}));
}

TEST_CASE("router: flagged screens exist only while their flag is on (UID-024)", "[ui][router]")
{
    auto r = graph();
    ui::FeatureFlags flags;
    ui::Router router(r, &flags);
    CHECK_FALSE(router.open("utility.midiMonitor"));
    flags.enable("ui.midiMonitor");
    CHECK(router.open("utility.midiMonitor"));
    CHECK(joined(router.breadcrumb()) == "HOME > UTILITY > MIDI MONITOR");
}

TEST_CASE("router: the focus position is remembered per screen (UI-009)", "[ui][router]")
{
    auto r = graph();
    ui::Router router(r);
    router.open("utility");
    router.rememberFocus(3);
    router.home();
    CHECK(router.focusOf("utility") == 3);
    CHECK(router.focusOf("mixer") == -1);
}
