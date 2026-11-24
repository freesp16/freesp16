// services::settings::migrateMidiPortLayout: the unit's port preferences saved when USB was port 1 and DIN port 2
// (ALSA card order) follow their sockets once DIN is port 1 and USB port 2; an OUT MODE keeps its meaning (its MERGE /
// THRU source names an input, and the inputs swapped too); the USB port keeps only OUT / OUT + MIDI IN (manual 1.60
// p.12); once only; a new install gets the marker.
#include "services/settings/MidiPortLayout.h"

#include <catch2/catch_test_macros.hpp>

namespace st = services::settings;

namespace
{
// Modes (core/util/midi/MidiWire.h): 0 OUT, 1 OUT + MERGE A, 2 OUT + MERGE B, 3 THRU A, 4 THRU B.
// Old layout: port 1 (key "midi.portMode") = USB, port 2 ("#1") = DIN, input A = USB IN, B = MIDI (DIN) IN.
// New layout: port 1 = DIN, port 2 = USB, A = MIDI (DIN) IN, B = USB IN.
constexpr float kOut = 0, kMergeA = 1, kMergeB = 2, kThruA = 3, kThruB = 4;
} // namespace

TEST_CASE("settings: the MIDI port preferences follow DIN to port 1 and USB to port 2, once", "[settings]")
{
    SECTION("old layout: SYNC PORT 2 (DIN then) becomes 1, the per-port values swap and keep their meaning")
    {
        st::Preferences p;
        p.set("midi.syncSource", 1);
        p.set("midi.syncPort", 2);
        p.set("midi.portMode#1", kThruB);  // DIN OUT on THRU (MIDI IN): B was the DIN input
        p.set("midi.portMode", kMergeB);   // USB OUT on OUT + MIDI IN: merges B, the DIN input
        p.set("midi.portMode#2", kMergeA); // port 3 merges A = the USB input
        p.set("midi.portMode#3", kThruB);  // port 4 passes the DIN input on
        p.set("midi.portOffset", -10);     // USB (then port 1)
        p.set("midi.portIn#1", 0);         // DIN IN off
        CHECK(st::migrateMidiPortLayout(p));
        CHECK(p.get("midi.syncPort") == 1.0f);
        CHECK(p.get("midi.syncSource") == 1.0f);
        CHECK(p.get("midi.portMode") == kThruA);    // DIN OUT: still THRU (MIDI IN)
        CHECK(p.get("midi.portMode#1") == kMergeA); // USB OUT: still OUT + MIDI IN
        CHECK(p.get("midi.portMode#2") == kMergeB); // still merges the USB input
        CHECK(p.get("midi.portMode#3") == kThruA);  // still passes the DIN input on
        CHECK(p.get("midi.portOffset#1") == -10.0f);
        CHECK_FALSE(p.has("midi.portOffset"));
        CHECK(p.get("midi.portIn#0") == 0.0f);
        CHECK_FALSE(p.has("midi.portIn#1"));
        CHECK(p.get(st::kMidiPortLayoutKey) == 1.0f);
        // Once only.
        CHECK_FALSE(st::migrateMidiPortLayout(p));
        CHECK(p.get("midi.syncPort") == 1.0f);
        CHECK(p.get("midi.portMode") == kThruA);
        CHECK(p.get("midi.portMode#1") == kMergeA);
    }
    SECTION("SYNC PORT 1 (USB then) becomes 2; a mode without a source stays; DIN OUT + USB IN keeps its meaning")
    {
        st::Preferences p;
        p.set("midi.syncPort", 1);
        p.set("midi.portMode#1", kMergeA); // DIN OUT merges A = the USB input
        p.set("midi.portMode#4", kOut);
        CHECK(st::migrateMidiPortLayout(p));
        CHECK(p.get("midi.syncPort") == 2.0f);
        CHECK(p.get("midi.portMode") == kMergeB); // DIN: OUT + USB IN
        CHECK_FALSE(p.has("midi.portMode#1"));
        CHECK(p.get("midi.portMode#4") == kOut);
    }
    SECTION("what the USB port cannot do becomes the nearest of OUT / OUT + MIDI IN")
    {
        const struct
        {
            float before, after;
        } cases[] = {
            {kMergeA, kOut},    // merged its own (USB) input → OUT
            {kThruA, kOut},     // passed its own (USB) input on → OUT
            {kThruB, kMergeA},  // passed the DIN input on → OUT + MIDI IN
            {kMergeB, kMergeA}, // OUT + MIDI IN
            {kOut, kOut},
        };
        for (const auto& c : cases)
        {
            st::Preferences p;
            p.set("midi.portMode", c.before); // old port 1 = USB
            st::migrateMidiPortLayout(p);
            INFO(c.before);
            CHECK(p.get("midi.portMode#1") == c.after);
        }
    }
    SECTION("a new install: nothing to swap, only the marker")
    {
        st::Preferences p;
        CHECK_FALSE(st::migrateMidiPortLayout(p));
        CHECK(p.has(st::kMidiPortLayoutKey));
        CHECK(p.get("midi.syncPort") == 1.0f);
    }
}
