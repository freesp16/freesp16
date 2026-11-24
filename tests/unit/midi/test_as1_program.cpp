// TORAIZ AS-1 preset selection (the manual's 7.1, addendum p.40-41): the bank names U1–U5 / F1–F5 and program numbers
// 1–99 against the messages sent (core::midi::as1, UNVERIFIED mapping), and the engine sending them on a scene switch.
#include "tests/unit/midi/MidiTestUtil.h"

#include "core/util/midi/As1Program.h"
#include "ui/screens/miditrack/As1Popups.h"

#include <catch2/catch_test_macros.hpp>

namespace as1 = core::midi::as1;

TEST_CASE("midi.as1: banks U1–U5 / F1–F5 are Bank Select MSB 0, LSB 0–9; programs 1–99 are PC 0–98", "[midi]")
{
    for (int b = 0; b < as1::kBanks; ++b)
    {
        CHECK(as1::bankOf(as1::kBankMsb, as1::bankLsb(b)) == b);
        CHECK(ui::as1BankText(as1::kBankMsb, as1::bankLsb(b)) == as1::kBankNames[b]);
    }
    CHECK(ui::as1BankText(0, 0) == "U1");
    CHECK(ui::as1BankText(0, 4) == "U5");
    CHECK(ui::as1BankText(0, 5) == "F1");
    CHECK(ui::as1BankText(0, 9) == "F5");
    CHECK(ui::as1BankText(-1, -1) == "---"); // unset: nothing is sent
    CHECK(ui::as1BankText(0, 10) == "---");  // not an AS-1 bank
    CHECK(ui::as1BankText(3, 0) == "---");
    for (int shown = 1; shown <= as1::kPrograms; ++shown)
        CHECK(as1::programShown(as1::programChange(shown)) == shown);
    CHECK(ui::as1ProgramText(as1::programShown(0)) == "P01");
    CHECK(ui::as1ProgramText(as1::programShown(98)) == "P99");
    CHECK(ui::as1ProgramText(as1::programShown(99)) == "---");
    CHECK(ui::as1ProgramText(as1::programShown(-1)) == "---");
}

TEST_CASE("midi.as1: the scene's bank and program go out when the scene becomes current (MID-050)", "[midi]")
{
    auto e = miditest::make();
    miditest::Driver d {*e, 64};
    e->midi().hub().setOutputConnected(0, true);
    const std::uint16_t scene2 = 1 << 4 | 4; // scene 2, track 5
    d.param("track.mode", scene2, 3.0f);     // TORAIZ AS-1
    d.param("midi.bankMsb", scene2, static_cast<float>(as1::kBankMsb));
    d.param("midi.bankLsb", scene2, static_cast<float>(as1::bankLsb(7))); // F3
    d.param("midi.program", scene2, static_cast<float>(as1::programChange(42)));
    miditest::drainOut(*e);
    e->bridge().post(engine::ParamChange {miditest::id("scene.queue"), 0, 16.0f, 0}); // scene 2 (stopped: at once)
    d.blocks(3);
    const auto out = miditest::drainOut(*e);
    REQUIRE(out.size() == 3);
    CHECK((out[0].bytes[0] == 0xB4 && out[0].bytes[1] == 0 && out[0].bytes[2] == 0));
    CHECK((out[1].bytes[0] == 0xB4 && out[1].bytes[1] == 32 && out[1].bytes[2] == 7));
    CHECK((out[2].bytes[0] == 0xC4 && out[2].bytes[1] == 41));
}
