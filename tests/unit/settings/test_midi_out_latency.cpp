// services::settings::migrateMidiOutLatency: MIDI OUTPUT LATENCY is an extra trim now (the sender compensates the
// audio buffer itself); a stored 12 ms (the old default) becomes the new default 0 once, a user's own value stays.
#include "services/settings/MidiOutLatency.h"

#include <catch2/catch_test_macros.hpp>

namespace st = services::settings;

TEST_CASE("settings: a stored MIDI OUTPUT LATENCY of 12 ms (the old default) becomes 0, once", "[settings]")
{
    REQUIRE(core::builtinParams().find("midi.outLatency") != nullptr);
    CHECK(core::builtinParams().find("midi.outLatency")->defaultValue == 0.0f);
    SECTION("the old default stored: back to the default 0")
    {
        st::Preferences p;
        p.set("midi.outLatency", 12);
        CHECK(st::migrateMidiOutLatency(p));
        CHECK_FALSE(p.has("midi.outLatency"));
        CHECK(p.get("midi.outLatency") == 0.0f);
        CHECK(p.get(st::kMidiOutLatencyKey) == 1.0f);
        // Once only: a 12 set afterwards is the user's choice.
        p.set("midi.outLatency", 12);
        CHECK_FALSE(st::migrateMidiOutLatency(p));
        CHECK(p.get("midi.outLatency") == 12.0f);
    }
    SECTION("another value is the user's trim: kept")
    {
        st::Preferences p;
        p.set("midi.outLatency", 20);
        CHECK_FALSE(st::migrateMidiOutLatency(p));
        CHECK(p.get("midi.outLatency") == 20.0f);
        CHECK(p.has(st::kMidiOutLatencyKey));
    }
    SECTION("a new install: nothing stored, only the marker")
    {
        st::Preferences p;
        CHECK_FALSE(st::migrateMidiOutLatency(p));
        CHECK(p.has(st::kMidiOutLatencyKey));
        CHECK(p.get("midi.outLatency") == 0.0f);
    }
}
