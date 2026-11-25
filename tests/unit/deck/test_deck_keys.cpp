// ui::input::deck::DeckKeys: the DJS-1000's DJ section (TEMPO fader, RANGE, RESET, SYNC, MASTER, FX, LIVE SAMPLING).
#include "ui/input/deck/DeckKeys.h"

#include "core/params/ParamRegistry.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/shell/ParamAccess.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using ui::input::deck::DeckKeys;
using Kind = io::ControlEvent::Kind;

namespace
{
io::ControlEvent ev(Kind k, io::Control c, int v = 0)
{
    return {k, c, v, io::kSourcePanel, 0};
}
io::ControlEvent press(io::Control c)
{
    return ev(Kind::Press, c, 100);
}
io::ControlEvent fader(int raw)
{
    return ev(Kind::Value, io::Control::TempoFader, raw);
}

struct Rig
{
    ui::LocalParamValues values{core::builtinParams()};
    ui::fx::LocalFxSlots slots;
    std::vector<double> tempos;
    std::vector<std::string> opened;
    DeckKeys keys{values, slots,
                  DeckKeys::Hooks{[this](double b) { tempos.push_back(b); },
                                  [this](std::string_view id) { opened.emplace_back(id); }, [] { return 1; }}};
    std::uint64_t block = 0;
    // The engine plays `bpm`, some blocks later.
    void feed(double bpm, std::uint64_t blocks = 100)
    {
        block += blocks;
        engine::PlayheadFeed f;
        f.blockIndex = block;
        f.bpm = bpm;
        keys.frame(f);
    }
    float get(const char* id) { return values.get(*core::builtinParams().find(id), 0); }
};
} // namespace

TEST_CASE("deck.tempo: the fader's position, with a dead zone in the centre", "[deck]")
{
    CHECK(DeckKeys::faderOffset(512) == 0.0);
    CHECK(DeckKeys::faderOffset(512 + DeckKeys::kFaderDeadZone) == 0.0);
    CHECK(DeckKeys::faderOffset(0) == -1.0);
    CHECK(DeckKeys::faderOffset(1023) == 1.0);
    CHECK(DeckKeys::faderOffset(400) < 0.0);
}

TEST_CASE("deck.tempo: the fader plays the base tempo within the range, the project's tempo is kept", "[deck]")
{
    Rig r;
    r.feed(120.0);
    REQUIRE(r.keys.handle(fader(0))); // the minus end: −6 %
    REQUIRE(r.tempos.size() == 1);
    CHECK(r.tempos.back() == Catch::Approx(112.8));
    CHECK(r.keys.liveTempo() == Catch::Approx(112.8));
    CHECK(r.get("transport.tempo") == 120.0f);     // no model edit
    r.keys.handle(press(io::Control::TempoRange)); // ±10 %
    CHECK(r.tempos.back() == Catch::Approx(108.0));
    r.keys.handle(fader(512)); // back to the centre: the base again
    CHECK(r.tempos.back() == Catch::Approx(120.0));
    CHECK(r.keys.liveTempo() == 0.0);
}

TEST_CASE("deck.tempo: a fader read before the engine runs applies once the engine reports its tempo", "[deck]")
{
    Rig r;
    r.keys.handle(fader(1023)); // the panel's first frames: the engine is not running yet
    CHECK(r.tempos.empty());
    r.feed(120.0);
    REQUIRE(r.tempos.size() == 1);
    CHECK(r.tempos.back() == Catch::Approx(127.2));
    r.feed(127.2);
    CHECK(r.tempos.size() == 1);
}

TEST_CASE("deck.tempo: RANGE cycles 6, 10, 16, 50 % and RESET ignores the fader", "[deck]")
{
    Rig r;
    r.feed(100.0);
    for (int want : {10, 16, 50, 6})
    {
        r.keys.handle(press(io::Control::TempoRange));
        CHECK(r.keys.rangePercent() == want);
    }
    r.keys.handle(fader(1023));
    CHECK(r.tempos.back() == Catch::Approx(106.0));
    r.keys.handle(press(io::Control::TempoReset));
    CHECK(r.keys.tempoReset());
    CHECK(r.tempos.back() == Catch::Approx(100.0));
    io::LampFrame f;
    r.keys.lamps(f);
    CHECK(f[io::Control::TempoReset].mode == io::LampMode::On);
    r.keys.handle(press(io::Control::TempoReset));
    CHECK(r.tempos.back() == Catch::Approx(106.0));
}

TEST_CASE("deck.tempo: a tempo set by someone else becomes the new base", "[deck]")
{
    Rig r;
    r.feed(120.0);
    r.keys.handle(fader(1023)); // +6 %: 127.2
    r.feed(127.2);              // the engine plays it: nothing new
    CHECK(r.tempos.size() == 1);
    r.feed(130.0); // a scene's BPM
    CHECK(r.keys.baseTempo() == Catch::Approx(130.0));
    CHECK(r.tempos.back() == Catch::Approx(137.8));
}

TEST_CASE("deck.sync: back from the clock to the fader's own tempo, the fader is not applied twice", "[deck]")
{
    Rig r;
    r.feed(120.0);
    r.keys.handle(fader(1023)); // +6 %: 127.2
    r.feed(127.2);
    REQUIRE(r.tempos.size() == 1);
    r.values.set(*core::builtinParams().find("midi.syncSource"), 0, 1.0f); // SYNC SOURCE in UTILITY
    r.feed(133.0);
    r.feed(133.0);
    r.values.set(*core::builtinParams().find("midi.syncSource"), 0, 0.0f); // and back to INTERNAL
    r.feed(127.2, 10);
    r.feed(127.2);               // the engine's internal tempo again: the fader's own
    CHECK(r.tempos.size() == 1); // no 134.8
    CHECK(r.keys.baseTempo() == Catch::Approx(120.0));
    CHECK(r.keys.liveTempo() == Catch::Approx(127.2));
}

TEST_CASE("deck.sync: SYNC follows the MIDI clock and the fader then does nothing; MASTER sends the clock", "[deck]")
{
    Rig r;
    r.feed(120.0);
    r.keys.handle(press(io::Control::Sync));
    CHECK(r.keys.following());
    CHECK(r.get("midi.syncSource") == 1.0f);
    r.keys.handle(fader(0));
    CHECK(r.tempos.empty());
    r.feed(133.0);                    // the clock's tempo
    CHECK(r.keys.liveTempo() == 0.0); // the screen shows the engine's, not the fader's
    CHECK(r.tempos.empty());
    r.keys.handle(press(io::Control::Sync));
    CHECK_FALSE(r.keys.following());
    r.feed(133.0, 10); // the feed has not settled yet: nothing
    CHECK(r.tempos.empty());
    r.feed(133.0); // the engine kept the clock's tempo: the new base
    REQUIRE(! r.tempos.empty());
    CHECK(r.tempos.back() == Catch::Approx(125.0)); // the fader again, on the clock's last tempo (133 − 6 %)

    io::LampFrame f;
    r.keys.lamps(f);
    const bool sent = r.get("midi.out.clock") >= 0.5f;
    CHECK((f[io::Control::Master].mode == io::LampMode::On) == sent);
    r.keys.handle(press(io::Control::Master));
    CHECK((r.get("midi.out.clock") >= 0.5f) == ! sent);
}

TEST_CASE("deck.fx: FX ON turns the MASTER FX on and off, FX opens its page, FX LEVEL sets its mix", "[deck]")
{
    Rig r;
    r.keys.handle(press(io::Control::FxOn)); // no effect yet: choose one
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened.back() == "mixer.busFx");
    r.slots.selectEffect(ui::fx::Slot::Master, 1, 1, "reverb");
    io::LampFrame f;
    r.keys.lamps(f);
    CHECK(f[io::Control::FxOn].mode == io::LampMode::On);
    r.keys.handle(press(io::Control::FxOn));
    CHECK(r.slots.bypass(ui::fx::Slot::Master, 1, 1));
    r.keys.lamps(f);
    CHECK(f[io::Control::FxOn].mode == io::LampMode::Off);
    r.keys.handle(press(io::Control::FxSelect));
    CHECK(r.opened.back() == "mixer.busFx");
    const auto* mix = core::builtinParams().find("master.reverb.mix");
    REQUIRE(mix != nullptr);
    CHECK(r.keys.handle(ev(Kind::Value, io::Control::FxLevel, 1023)));
    CHECK(r.values.get(*mix, 0) == Catch::Approx(1.0f));
}

TEST_CASE("deck.livesampling: LIVE SAMPLING opens its screen; other controls are not the deck's", "[deck]")
{
    Rig r;
    CHECK(r.keys.handle(press(io::Control::LiveSampling)));
    REQUIRE(r.opened.size() == 2);
    CHECK(r.opened[0] == "track.menu");
    CHECK(r.opened[1] == "sampling.live");
    CHECK(r.keys.handle(press(io::Control::PadModeHotSlice)));
    CHECK(r.opened.size() == 2);
    CHECK_FALSE(r.keys.handle(press(io::Control::Play)));
    CHECK_FALSE(r.keys.handle(press(io::Control::PadModeSlice)));
}
