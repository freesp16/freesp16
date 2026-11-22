// The Toggle trigger mode (TRK-012; engine/trigger/ext/toggle/ToggleLatch.h) through the
// whole engine: the first pad hit latches the note (its release does nothing), the next hit stops it; TOGGLE off or a
// latched track falling silent clears the latch; with no ext.toggle.track message the engine is unchanged (bit-exact).
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "core/params/ParamRegistry.h"
#include "engine/voice/VoiceSwaps.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

bool install(engine::Engine& e, const voicetest::TestBuffer& b, int track)
{
    core::audio::retain(b.get());
    return e.bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, track), b.get()});
}

bool toggle(engine::Engine& e, int track, bool on)
{
    return e.bridge().post(engine::ParamChange {id("ext.toggle.track"), static_cast<std::uint16_t>(track), on ? 1.0f : 0.0f, 0});
}

bool sounding(engine::Engine& e, int track) { return e.trackSource(track).voice().active(); }
} // namespace

TEST_CASE("ext toggle: the ParamSpec is a flagged, non-lockable track value after every base id", "[ext][toggle]")
{
    const auto& r = core::builtinParams();
    const auto* s = r.find("ext.toggle.track");
    REQUIRE(s != nullptr);
    CHECK(s->featureFlag == "ext.trk.toggle");
    CHECK(s->scope == core::ParamScope::Track);
    CHECK_FALSE(s->lockable);
    CHECK_FALSE(s->lfoTarget);
    CHECK_FALSE(s->stripTarget);
    CHECK(s->midiCc == -1);
    // Order 9950: after every other catalogue, so every existing ParamId is what it was.
    const auto mine = r.idOf("ext.toggle.track")->value;
    for (std::size_t i = 0; i < r.size(); ++i)
        if (r.all()[i].id.substr(0, 4) != "ext.")
            CHECK(i < mine);
}

TEST_CASE("ext toggle: first hit latches (pad up does nothing), second hit stops", "[ext][toggle]")
{
    voicetest::TestBuffer buf(std::vector<float>(60000, 0.25f));
    auto e = enginetest::make(false);
    REQUIRE(install(*e, buf, 0));
    REQUIRE(e->bridge().post(engine::ParamChange {id("playback.loop"), 0, 1.0f, 0})); // LOOP on: the note sustains
    REQUIRE(e->bridge().post(engine::ParamChange {id("env.release"), 0, 0.0f, 0}));
    REQUIRE(toggle(*e, 0, true));
    enginetest::render(*e, 256, 64);
    CHECK(e->toggleLatch().enabled(0, 0));
    CHECK(e->triggerRouter().settings(0, 0).triggerMode == core::model::TriggerMode::Gate);

    REQUIRE(e->triggers().push({0, 127, 60, 0})); // press
    enginetest::render(*e, 1024, 64);
    CHECK(sounding(*e, 0));
    CHECK(e->toggleLatch().latched(0));
    REQUIRE(e->triggers().push({0, 0, 60, 0})); // pad up: swallowed
    const auto held = enginetest::render(*e, 88200, 256); // 2 s, longer than the sample: LOOP keeps it going
    CHECK(sounding(*e, 0));
    CHECK(enginetest::peak(held.l, 80000) > 0.1f);
    CHECK(e->toggleLatch().latched(0));
    CHECK(e->voiceStarts() == 1);

    REQUIRE(e->triggers().push({0, 127, 60, 0})); // second press: stop
    const auto stop = enginetest::render(*e, 8192, 64);
    CHECK(enginetest::peak(stop.l, 4096) == 0.0f);
    CHECK_FALSE(sounding(*e, 0));
    CHECK_FALSE(e->toggleLatch().latched(0));
    CHECK(e->voiceStarts() == 1); // the stop is a release, not a new note
    REQUIRE(e->triggers().push({0, 0, 60, 0})); // its pad up: swallowed, nothing happens
    enginetest::render(*e, 1024, 64);
    CHECK(e->voiceStarts() == 1);

    // Again: a third press starts it again.
    REQUIRE(e->triggers().push({0, 127, 60, 0}));
    enginetest::render(*e, 1024, 64);
    CHECK(sounding(*e, 0));
    CHECK(e->voiceStarts() == 2);
}

TEST_CASE("ext toggle: TOGGLE off releases a latched note and restores the model's trigger mode", "[ext][toggle]")
{
    voicetest::TestBuffer buf(std::vector<float>(60000, 0.25f));
    auto e = enginetest::make(false);
    REQUIRE(install(*e, buf, 2));
    REQUIRE(e->bridge().post(engine::ParamChange {id("playback.loop"), 2, 1.0f, 0}));
    REQUIRE(e->bridge().post(engine::ParamChange {id("env.release"), 2, 0.0f, 0}));
    REQUIRE(toggle(*e, 2, true));
    // The model's TRIGGER MODE changes while TOGGLE is on: remembered, the router stays GATE.
    REQUIRE(e->bridge().post(engine::ParamChange {id("track.triggerMode"), 2, 0.0f, 0}));
    REQUIRE(e->triggers().push({2, 127, 60, 2}));
    REQUIRE(e->triggers().push({2, 0, 60, 2}));
    enginetest::render(*e, 4096, 64);
    CHECK(e->toggleLatch().latched(2));
    CHECK(e->triggerRouter().settings(0, 2).triggerMode == core::model::TriggerMode::Gate);
    CHECK(sounding(*e, 2));

    REQUIRE(toggle(*e, 2, false));
    const auto out = enginetest::render(*e, 8192, 64);
    CHECK_FALSE(e->toggleLatch().latched(2));
    CHECK_FALSE(sounding(*e, 2));
    CHECK(enginetest::peak(out.l, 4096) == 0.0f);
    CHECK(e->triggerRouter().settings(0, 2).triggerMode == core::model::TriggerMode::OneShot);
}

TEST_CASE("ext toggle: a one-shot sample that ends clears the latch; posted hits latch by track", "[ext][toggle]")
{
    voicetest::TestBuffer buf(std::vector<float>(4000, 0.25f)); // 90 ms, LOOP off
    auto e = enginetest::make(false);
    REQUIRE(install(*e, buf, 1));
    REQUIRE(toggle(*e, 1, true));
    REQUIRE(e->triggers().push({1, 127, 60, 1}));
    enginetest::render(*e, 1024, 64);
    CHECK(e->toggleLatch().latched(1));
    enginetest::render(*e, 8192, 64); // the sample ended
    CHECK_FALSE(sounding(*e, 1));
    CHECK_FALSE(e->toggleLatch().latched(1));
    REQUIRE(e->triggers().push({1, 0, 60, 1})); // the first press's pad up: swallowed
    // The next press starts it again (not a stop: the latch cleared).
    REQUIRE(e->triggers().push({1, 127, 60, 1}));
    enginetest::render(*e, 1024, 64);
    CHECK(e->voiceStarts() == 2);

    // A posted hit without a pad (virtual panel / MIDI-style, pad −1) latches by track.
    voicetest::TestBuffer loop(std::vector<float>(60000, 0.25f));
    REQUIRE(install(*e, loop, 3));
    REQUIRE(e->bridge().post(engine::ParamChange {id("playback.loop"), 3, 1.0f, 0}));
    REQUIRE(toggle(*e, 3, true));
    REQUIRE(e->bridge().post(engine::TriggerNow {3, 100, 60, -1}));
    REQUIRE(e->bridge().post(engine::TriggerNow {3, 0, 60, -1})); // note off: swallowed
    enginetest::render(*e, 8192, 64);
    CHECK(sounding(*e, 3));
    REQUIRE(e->bridge().post(engine::TriggerNow {3, 100, 60, -1})); // second note on: stop
    enginetest::render(*e, 8192, 64);
    CHECK_FALSE(sounding(*e, 3));
}

TEST_CASE("ext toggle: with no ext.toggle.track message the engine output is bit-identical", "[ext][toggle][parity]")
{
    voicetest::TestBuffer buf(voicetest::sine(30000, 220.0, voicetest::kRate, 0.5f));
    auto run = [&](bool touchToggle) {
        auto e = enginetest::make(false);
        REQUIRE(install(*e, buf, 0));
        if (touchToggle)
        {
            // On and off again on another track: the latch is back to its idle state.
            REQUIRE(toggle(*e, 5, true));
            REQUIRE(toggle(*e, 5, false));
        }
        std::vector<float> all;
        for (int k = 0; k < 6; ++k)
        {
            REQUIRE(e->triggers().push({0, static_cast<std::uint8_t>(127 - k * 10), 60, 0}));
            const auto a = enginetest::render(*e, 3000, 64);
            REQUIRE(e->triggers().push({0, 0, 60, 0}));
            const auto b = enginetest::render(*e, 2000, 64);
            all.insert(all.end(), a.l.begin(), a.l.end());
            all.insert(all.end(), b.l.begin(), b.l.end());
        }
        return all;
    };
    const auto plain = run(false);
    const auto touched = run(true);
    CHECK(plain == touched);
    CHECK(enginetest::peak(plain) > 0.1f);
}
