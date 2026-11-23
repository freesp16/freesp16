// services::settings::migrateMidiPortLayout: the unit's port preferences saved when USB was port 1 and DIN port 2
// (ALSA card order) follow their sockets once DIN is port 1 and USB port 2; once only; a new install gets the marker.
#include "services/settings/MidiPortLayout.h"

#include <catch2/catch_test_macros.hpp>

namespace st = services::settings;

TEST_CASE("settings: the MIDI port preferences follow DIN to port 1 and USB to port 2, once", "[settings]")
{
    SECTION("old layout: SYNC PORT 2 (DIN then) becomes 1, the per-port values swap")
    {
        st::Preferences p;
        p.set("midi.syncSource", 1);
        p.set("midi.syncPort", 2);
        p.set("midi.portMode#1", 3);   // DIN (then port 2) on THRU A
        p.set("midi.portOffset", -10); // USB (then port 1)
        p.set("midi.portIn#1", 0);     // DIN IN off
        CHECK(st::migrateMidiPortLayout(p));
        CHECK(p.get("midi.syncPort") == 1.0f);
        CHECK(p.get("midi.syncSource") == 1.0f);
        CHECK(p.get("midi.portMode") == 3.0f);
        CHECK_FALSE(p.has("midi.portMode#1"));
        CHECK(p.get("midi.portOffset#1") == -10.0f);
        CHECK_FALSE(p.has("midi.portOffset"));
        CHECK(p.get("midi.portIn#0") == 0.0f);
        CHECK_FALSE(p.has("midi.portIn#1"));
        CHECK(p.get(st::kMidiPortLayoutKey) == 1.0f);
        // Once only.
        CHECK_FALSE(st::migrateMidiPortLayout(p));
        CHECK(p.get("midi.syncPort") == 1.0f);
        CHECK(p.get("midi.portMode") == 3.0f);
    }
    SECTION("SYNC PORT 1 (USB then) becomes 2; ports 3 and up stay")
    {
        st::Preferences p;
        p.set("midi.syncPort", 1);
        p.set("midi.portMode#2", 4);
        CHECK(st::migrateMidiPortLayout(p));
        CHECK(p.get("midi.syncPort") == 2.0f);
        CHECK(p.get("midi.portMode#2") == 4.0f);
    }
    SECTION("a new install: nothing to swap, only the marker")
    {
        st::Preferences p;
        CHECK_FALSE(st::migrateMidiPortLayout(p));
        CHECK(p.has(st::kMidiPortLayoutKey));
        CHECK(p.get("midi.syncPort") == 1.0f);
    }
}
