// Logical control names (03-ui/control-surface.md §2.1, UI-101/102): the enum (Pad1…) and the spec ids ("pad.1")
// map both ways; every control of the spec's tables parses.
#include "io/controllers/ControlSurface.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

TEST_CASE("every control has a spec name that round-trips", "[contracts][controls]")
{
    std::set<std::string_view> names;
    for (int i = 0; i < static_cast<int>(io::Control::Count); ++i)
    {
        const auto c = static_cast<io::Control>(i);
        const auto name = io::nameOf(c);
        if (name.empty())
        {
            // gaps in the numbering (Step16+1 … Bar1-1 do not exist): only between groups
            CHECK(((i > static_cast<int>(io::Control::Pattern) && i < static_cast<int>(io::Control::Knob1))));
            continue;
        }
        INFO(name);
        CHECK(names.insert(name).second);
        const auto back = io::controlFromName(name);
        REQUIRE(back.has_value());
        CHECK(*back == c);
    }
}

TEST_CASE("spec ids and enum names both parse", "[contracts][controls]")
{
    for (const char* id : {"pad.1", "pad.16", "step.1", "step.16", "bar.4", "padmode.track", "padmode.scale", "shift", "rec",
                           "stop", "play", "scene", "pattern", "enc.1", "enc.6", "sel", "sel.push", "back", "home", "strip",
                           "strip.mode", "strip.hold", "strip.led.1", "strip.led.8", "stripmode.pitch", "stripmode.user2",
                           "afs.active", "afs.drive", "afs.cutoff", "afs.resonance", "afs.hpCutoff", "afs.over",
                           "main.volume", "ext.copy", "ext.paste", "ext.clear", "ext.undo", "ext.redo", "ext.cue",
                           "ext.erase", "ext.tap", "ext.nudgeDown", "ext.nudgeUp", "ext.accent", "ext.mixer", "ext.browse",
                           "ext.utility", "ext.project"})
    {
        INFO(id);
        CHECK(io::controlFromName(id).has_value());
    }
    CHECK(io::controlFromName("Pad1") == io::Control::Pad1);
    CHECK(io::controlFromName("Pad16") == io::Control::Pad16);
    CHECK(io::controlFromName("Knob6") == io::Control::Knob6);
    CHECK(io::controlFromName("enc.3") == io::Control::Knob3);
    CHECK(io::controlFromName("SelectorPush") == io::Control::SelectorPush);
    CHECK(io::nameOf(io::Control::Knob3) == "enc.3");
    CHECK(io::nameOf(io::pad(7)) == "pad.7");
    CHECK_FALSE(io::controlFromName("pad.17").has_value());
    CHECK_FALSE(io::controlFromName("pad.0").has_value());
    CHECK_FALSE(io::controlFromName("pad.x").has_value());
    CHECK_FALSE(io::controlFromName("").has_value());
    STATIC_REQUIRE(static_cast<int>(io::Control::Pad1) == 0);
    STATIC_REQUIRE(static_cast<int>(io::Control::Step1) == 16);
    STATIC_REQUIRE(static_cast<int>(io::Control::Knob1) == 48);
    STATIC_REQUIRE(io::indexOf(io::Control::Pad16) == 16);
    STATIC_REQUIRE(io::indexOf(io::Control::Play) == 0);
}
