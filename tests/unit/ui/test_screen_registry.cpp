// Descriptor validator (UID-205…207, UID-225): the built-in registry validates; broken descriptors and a flagged spec
// that moves a reference control are rejected.
#include "core/params/ParamRegistry.h"
#include "ui/shell/Shell.h"
#include "ui/widgets/Widget.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace
{
using K = ui::RailItemSpec::Kind;
constexpr ui::RailItemSpec kRail[] = {{K::Button, "SET SLICE", "home"}, {K::Toggle, "REVERSE"}, {K::Button, "SETTING"}};
constexpr ui::RailItemSpec kRailPlusOne[] = {
    {K::Button, "SET SLICE", "home"}, {K::Toggle, "REVERSE"}, {K::Button, "SETTING"}, {K::Button, "DETECT"}};
constexpr ui::RailItemSpec kRailMoved[] = {
    {K::Toggle, "REVERSE"}, {K::Button, "SET SLICE", "home"}, {K::Button, "SETTING"}};

ui::ScreenSpec screen(std::string_view id, std::span<const ui::RailItemSpec> rail, ui::StripSpec strip,
                      std::string_view flag = {})
{
    return {.id = id,
            .title = "PLAYBACK",
            .parent = "home",
            .body = ui::BodyTemplate::RailContent,
            .contextBar = ui::ContextBarKind::TrackContext,
            .rail = rail,
            .bodyComponent = {},
            .strip = strip,
            .meter = {},
            .featureFlag = flag};
}

bool mentions(const std::vector<std::string>& problems, std::string_view what)
{
    return std::any_of(problems.begin(), problems.end(),
                       [&](const auto& p) { return p.find(what) != std::string::npos; });
}

ui::ScreenRegistry withHome()
{
    ui::ScreenRegistry r;
    ui::installFallbackScreens(r);
    return r;
}
} // namespace

TEST_CASE("descriptor: every built-in screen validates (UID-207)", "[ui][descriptor]")
{
    auto& builtin = ui::ScreenRegistry::builtin();
    ui::installFallbackScreens(builtin);
    const auto problems = builtin.validate(ui::WidgetRegistry::builtin().dispatcher(), core::builtinParams());
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
}

TEST_CASE("descriptor: the validator rejects a flagged spec that moves a reference control (UID-225)",
          "[ui][descriptor]")
{
    const ui::StripSpec strip{"PLAYBACK", {"playback.pitch", "", "playback.loop"}};
    auto r = withHome();
    r.add(screen("track.playback", kRail, strip));

    SECTION("filling rail slot 4 and an empty strip column is an extension")
    {
        r.add(screen("track.playback", kRailPlusOne, {"PLAYBACK", {"playback.pitch", "track.pan", "playback.loop"}},
                     "ext.detect"));
        const auto problems = r.validate({}, core::builtinParams());
        INFO((problems.empty() ? std::string() : problems.front()));
        CHECK(problems.empty());
    }
    SECTION("reordering the rail moves a reference control")
    {
        r.add(screen("track.playback", kRailMoved, strip, "ext.bad"));
        CHECK(mentions(r.validate({}, core::builtinParams()), "moves reference rail item SET SLICE"));
    }
    SECTION("moving a strip column moves a reference control")
    {
        r.add(screen("track.playback", kRail, {"PLAYBACK", {"", "playback.pitch", "playback.loop"}}, "ext.bad"));
        CHECK(mentions(r.validate({}, core::builtinParams()), "moves reference strip column 1"));
    }
    SECTION("changing the template is rejected")
    {
        auto s = screen("track.playback", kRail, strip, "ext.bad");
        s.body = ui::BodyTemplate::Full;
        r.add(s);
        CHECK(mentions(r.validate({}, core::builtinParams()), "UID-225"));
    }
    SECTION("an unflagged second descriptor is a duplicate")
    {
        r.add(screen("track.playback", kRail, strip));
        CHECK(mentions(r.validate({}, core::builtinParams()), "duplicate id"));
    }
}

TEST_CASE("descriptor: broken navigation and bindings are reported", "[ui][descriptor]")
{
    auto r = withHome();
    constexpr static ui::RailItemSpec rail[] = {{K::Option, "SHAPE", {}, "no.such.param"},
                                                {K::Button, "X", "no.such.screen"}};
    r.add({.id = "a",
           .title = "A",
           .parent = "b",
           .body = ui::BodyTemplate::RailContent,
           .contextBar = ui::ContextBarKind::Menu,
           .rail = rail,
           .bodyComponent = {},
           .strip = {"A", {"no.param"}}});
    r.add({.id = "b",
           .title = "B",
           .parent = "a",
           .body = ui::BodyTemplate::Full,
           .contextBar = ui::ContextBarKind::Menu});
    r.add({.id = "c",
           .title = "",
           .parent = "home",
           .body = ui::BodyTemplate::Full,
           .contextBar = ui::ContextBarKind::Menu});
    const auto problems = r.validate({}, core::builtinParams());
    CHECK(mentions(problems, "cyclic"));
    CHECK(mentions(problems, "binds unknown parameter no.such.param"));
    CHECK(mentions(problems, "opens unknown screen or popup no.such.screen"));
    CHECK(mentions(problems, "strip column no.param"));
    CHECK(mentions(problems, "no title"));
}

TEST_CASE("descriptor: a compact rail of five is allowed on rail templates only", "[ui][descriptor]")
{
    constexpr static ui::RailItemSpec five[] = {
        {K::Button, "1"}, {K::Button, "2"}, {K::Button, "3"}, {K::Button, "4"}, {K::Button, "5"}};
    auto r = withHome();
    r.add({.id = "browse",
           .title = "BROWSE",
           .parent = "home",
           .body = ui::BodyTemplate::RailContent,
           .contextBar = ui::ContextBarKind::TrackContext,
           .rail = five});
    CHECK(r.validate({}, core::builtinParams()).empty());
    r.add({.id = "full",
           .title = "FULL",
           .parent = "home",
           .body = ui::BodyTemplate::Full,
           .contextBar = ui::ContextBarKind::Menu,
           .rail = five});
    CHECK(mentions(r.validate({}, core::builtinParams()), "more than 4 rail items"));
}
