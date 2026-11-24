// MIDI byte parser and the fixed CC table (midi.md MID-030/031/035, FX-086, SCN-050).
#include "tests/unit/midi/MidiTestUtil.h"

#include "io/midi/MidiParser.h"
#include "io/midi/VirtualPort.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
std::vector<core::midi::MidiEvent> parse(std::initializer_list<int> bytes)
{
    io::midi::MidiParser p;
    std::vector<core::midi::MidiEvent> out;
    core::midi::MidiEvent e;
    for (int b : bytes)
        if (p.feed(static_cast<std::uint8_t>(b), e))
            out.push_back(e);
    return out;
}
} // namespace

TEST_CASE("midi.parser: running status, real time inside a message, SysEx skipped", "[midi]")
{
    const auto v = parse({0x90, 60, 100, 62, 101, 0xF8, 0x80, 60, 0xF8, 0, 0xF0, 1, 2, 0xF8, 3, 0xF7, 0xB0, 7, 127,
                          0xC5, 9, 10, 0xF2, 0x10, 0x01, 0x33});
    REQUIRE(v.size() == 10);
    CHECK((v[0].bytes[0] == 0x90 && v[0].bytes[1] == 60 && v[0].bytes[2] == 100 && v[0].size == 3));
    CHECK((v[1].bytes[0] == 0x90 && v[1].bytes[1] == 62 && v[1].bytes[2] == 101)); // running status
    CHECK((v[2].bytes[0] == 0xF8 && v[2].size == 1));                               // real time between messages
    CHECK((v[3].bytes[0] == 0xF8));                                                 // real time inside Note Off
    CHECK((v[4].bytes[0] == 0x80 && v[4].bytes[1] == 60 && v[4].bytes[2] == 0));
    CHECK((v[5].bytes[0] == 0xF8)); // real time inside SysEx; the SysEx data is dropped
    CHECK((v[6].bytes[0] == 0xB0 && v[6].bytes[1] == 7 && v[6].bytes[2] == 127));
    CHECK((v[7].bytes[0] == 0xC5 && v[7].bytes[1] == 9 && v[7].size == 2));
    CHECK((v[8].bytes[0] == 0xC5 && v[8].bytes[1] == 10)); // running status, one data byte
    CHECK((v[9].bytes[0] == 0xF2 && v[9].bytes[1] == 0x10 && v[9].bytes[2] == 0x01)); // SPP; 0x33 has no status now
}

TEST_CASE("midi.parser: a stray EOX (F7) outside SysEx clears running status", "[midi]")
{
    // F7 is system common: the data bytes after it have no status (running status would make 62 101 a Note On).
    const auto v = parse({0x90, 60, 100, 0xF7, 62, 101, 0x90, 64, 90, 0xF7, 0xF8, 65});
    REQUIRE(v.size() == 3);
    CHECK((v[0].bytes[0] == 0x90 && v[0].bytes[1] == 60));
    CHECK((v[1].bytes[0] == 0x90 && v[1].bytes[1] == 64 && v[1].bytes[2] == 90));
    CHECK((v[2].bytes[0] == 0xF8)); // real time still passes; 65 has no status
    // A message cut short by F7 is dropped, not completed by later data.
    const auto w = parse({0xB0, 7, 0xF7, 100, 0xB0, 10, 64});
    REQUIRE(w.size() == 1);
    CHECK((w[0].bytes[0] == 0xB0 && w[0].bytes[1] == 10 && w[0].bytes[2] == 64));
}

TEST_CASE("midi.cctable: the fixed map follows the registry (MID-030/031)", "[midi]")
{
    const auto& t = miditest::table();
    using K = core::midi::CcKind;
    CHECK(t.cc[0].kind == K::SceneSelect);
    CHECK(t.cc[32].kind == K::PatternSelect);
    CHECK(t.cc[123].kind == K::AllNotesOff);
    for (int n = 0; n < 6; ++n)
    {
        CHECK((t.cc[46 + n].kind == K::Fx1 && t.cc[46 + n].bands == n));
        CHECK((t.cc[63 + n].kind == K::Fx2 && t.cc[63 + n].bands == n));
    }
    // Every other CC is exactly the registry's byMidiCc.
    for (int cc = 1; cc < 128; ++cc)
    {
        if (cc == 32 || cc == 123 || (cc >= 46 && cc <= 51) || (cc >= 63 && cc <= 68))
            continue;
        const auto byCc = core::builtinParams().byMidiCc(cc);
        CHECK(byCc.has_value() == (t.cc[cc].kind != K::None));
        if (byCc)
            CHECK(t.cc[cc].param == *byCc);
    }
    const auto& vol = t.cc[7];
    REQUIRE(vol.kind == K::Linear);
    CHECK(vol.param == miditest::id("track.volume"));
    CHECK(core::midi::ccLinear(vol, 0) == vol.lo);
    CHECK(core::midi::ccLinear(vol, 127) == vol.hi);
    // Enumerations in equal bands (CC 16 trigger mode: 2 values, CC 14 time stretch: 3, MID-031).
    REQUIRE(t.cc[16].kind == K::Bands);
    CHECK(core::midi::ccBand(t.cc[16].bands, 63) == 0);
    CHECK(core::midi::ccBand(t.cc[16].bands, 64) == 1);
    REQUIRE(t.cc[14].kind == K::Bands);
    REQUIRE(t.cc[14].bands == 3);
    CHECK(core::midi::ccBand(3, 42) == 0);
    CHECK(core::midi::ccBand(3, 43) == 1);
    CHECK(core::midi::ccBand(3, 85) == 1);
    CHECK(core::midi::ccBand(3, 86) == 2);
    CHECK(core::midi::ccBand(3, 127) == 2);
    // SCN-050: 1–16, >= 16 → 16, 0 ignored.
    CHECK(core::midi::ccSelect(0) == -1);
    CHECK(core::midi::ccSelect(1) == 0);
    CHECK(core::midi::ccSelect(16) == 15);
    CHECK(core::midi::ccSelect(127) == 15);
}

TEST_CASE("midi.cctable: FX grids and the CC arithmetic equal core::fx (FX-086)", "[midi]")
{
    const auto& t = miditest::table();
    int knobs = 0;
    for (const auto& spec : core::builtinParams().all())
    {
        if (spec.id.substr(0, 4) != "fx1." && spec.id.substr(0, 4) != "fx2.")
            continue;
        const auto g = t.fxGrid[miditest::id(spec.id).value];
        if (g.hi <= g.lo)
            continue;
        ++knobs;
        for (int cc = 0; cc < 128; ++cc)
            REQUIRE(core::midi::ccFxValue(cc, g) == core::fx::ccToValue(cc, core::fx::Grid {g.lo, g.hi}));
    }
    CHECK(knobs > 40); // every insert effect's knobs, both slots, plus the SYNC (.div) forms
    const auto rate = t.fxGrid[miditest::id("fx1.chorus.rate").value];
    const auto rateDiv = t.fxGrid[miditest::id("fx1.chorus.rate.div").value];
    CHECK(rate.hi > rate.lo);
    CHECK(rateDiv.hi > rateDiv.lo);
    CHECK_FALSE(rate == rateDiv);
}

TEST_CASE("midi.virtual: script parsing", "[midi]")
{
    const auto s = io::midi::parseVirtualScript("# test\n100 90 05 64\n50 start\n200 clock 120 2 1.0\n300 spp 16\n"
                                                "400 continue\n500 stop\n600 bogus\n");
    REQUIRE(s.errors.size() == 1);
    REQUIRE(s.bytes.size() == 5);
    CHECK(s.bytes[0].bytes[0] == 0xFA); // sorted by time
    CHECK((s.bytes[1].bytes[0] == 0x90 && s.bytes[1].bytes[1] == 5 && s.bytes[1].bytes[2] == 0x64));
    CHECK((s.bytes[2].bytes[0] == 0xF2 && s.bytes[2].bytes[1] == 16 && s.bytes[2].bytes[2] == 0));
    REQUIRE(s.clocks.size() == 1);
    CHECK(s.clocks[0].bpm == 120.0);
    CHECK(s.clocks[0].jitterMs == 1.0);
}
