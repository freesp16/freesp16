// The trigger router and the track sources inside engine::Engine (steps 1, 6, 8a): pad hits from the TriggerSink
// (UI-118) and posted TriggerNow reach the router, samples installed through ObjectSwap play, the parameters of
// playback.cpp / ampenv.cpp / track.cpp are bound, the preview reaches MAIN through aux, and the TEMPORARY test tone
// keeps playing on track 1 while track 1 has no sample.
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "core/params/ParamRegistry.h"
#include "core/util/GainLaw.h"
#include "engine/voice/VoiceSwaps.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }
double dbfs(float peak) { return 20.0 * std::log10(static_cast<double>(peak)); }

// Posts a buffer into a slot the way the library's pool does (one reference for the slot).
bool install(engine::Engine& e, const voicetest::TestBuffer& b, engine::SwapSlot slot)
{
    core::audio::retain(b.get());
    return e.bridge().post(engine::ObjectSwap {slot, b.get()});
}
} // namespace

TEST_CASE("trigger.engine: the test tone stays at -12 dBFS while track 1 has no sample", "[trigger][engine]")
{
    auto e = enginetest::make();
    const auto out = enginetest::render(*e, 44100, 256);
    CHECK(dbfs(enginetest::peak(out.l, 4096)) == Catch::Approx(-12.04).margin(0.05));
    // A pad hit on an empty track pings (test-tone behaviour, audible proof that the hit reached the engine).
    REQUIRE(e->triggers().push({3, 127, 60, 3}));
    const auto hit = enginetest::render(*e, 4096, 256);
    CHECK(enginetest::peak(hit.l) > 0.3f);
    CHECK(e->triggersReceived() == 1);
    CHECK(e->voiceStarts() == 0);
}

TEST_CASE("trigger.engine: a sample on track 1 replaces the test tone; pads play it from the TriggerSink",
          "[trigger][engine]")
{
    voicetest::TestBuffer buf(voicetest::sine(30000, 220.0, voicetest::kRate, 0.5f));
    {
        auto e = enginetest::make();
        enginetest::render(*e, 1024, 256);
        REQUIRE(install(*e, buf, engine::voice::trackSampleSlot(0, 0)));
        const auto silent = enginetest::render(*e, 8192, 256);
        CHECK(enginetest::peak(silent.l, 1024) == 0.0f); // the tone is gone, nothing triggered yet
        CHECK(e->trackSource(0).hasSample());

        REQUIRE(e->triggers().push({0, 127, 60, 0})); // panel / keyboard reader thread (UI-118)
        const auto played = enginetest::render(*e, 8192, 256);
        CHECK(enginetest::peak(played.l) > 0.2f);
        CHECK(e->voiceStarts() == 1);
        CHECK(e->bridge().playhead().trackPosition[0] > 0.0f);

        // TRACK pad mode: the same hit selects (message thread) and does not sound.
        REQUIRE(e->bridge().post(engine::ParamChange {id("pads.mode"), 0xFFFF, 1.0f, 0}));
        REQUIRE(e->triggers().push({0, 127, 60, 0}));
        enginetest::render(*e, 1024, 256);
        CHECK(e->voiceStarts() == 1);
        // SCALE mode plays the selected track (pads.selected) from any pad.
        REQUIRE(e->bridge().post(engine::ParamChange {id("pads.mode"), 0xFFFF, 4.0f, 0}));
        REQUIRE(e->bridge().post(engine::ParamChange {id("pads.selected"), 0xFFFF, 0.0f, 0}));
        REQUIRE(e->bridge().post(engine::TriggerNow {9, 100, 60, 9})); // a posted hit (virtual panel)
        enginetest::render(*e, 1024, 256);
        CHECK(e->voiceStarts() == 2);
        REQUIRE(e->bridge().post(engine::ParamChange {id("pads.mode"), 0xFFFF, 0.0f, 0}));

        // Removing the sample brings the tone back.
        REQUIRE(e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 0), nullptr}));
        const auto back = enginetest::render(*e, 16384, 256);
        CHECK(dbfs(enginetest::peak(back.l, 8192)) == Catch::Approx(-12.04).margin(0.1));
    }
    CHECK(buf.refs() == 0); // every reference the engine took is returned
}

TEST_CASE("trigger.engine: playback, envelope, track, scale and pad parameters are bound", "[trigger][engine]")
{
    auto e = enginetest::make();
    for (const char* p : {"playback.pitch", "playback.start", "playback.loopStart", "playback.length", "playback.loop",
                          "playback.timeStretch", "playback.reverse", "playback.bypass", "env.velocity", "env.attack",
                          "env.hold", "env.release", "env.sync", "env.attackCurve", "env.releaseCurve",
                          "env.retrigger", "env.bypass", "track.padVelocity", "track.mode", "track.triggerMode",
                          "track.chokeGroup", "track.inputSource", "scale.type", "scale.keyShift", "scale.area",
                          "scaleMidi.type", "scaleMidi.root", "scaleMidi.area"})
    {
        INFO(p);
        CHECK(e->bridge().post(engine::ParamChange {id(p), 0x21, 1.0f, 0}));
    }
    for (const char* p : {"pads.mode", "pads.selected", "pads.shift"})
        CHECK(e->bridge().post(engine::ParamChange {id(p), 0xFFFF, 0.0f, 0}));
    // Message-thread-only ids are refused (the engine does not implement them).
    for (const char* p : {"track.padColor", "pads.velocityCurve", "pads.fixedVelocity"})
        CHECK_FALSE(e->bridge().post(engine::ParamChange {id(p), 0, 1.0f, 0}));
    enginetest::render(*e, 1024, 256);
    const auto& r = e->triggerRouter();
    CHECK(r.settings(2, 1).chokeGroup == 1);
    CHECK(r.settings(2, 1).triggerMode == core::model::TriggerMode::Gate);
    CHECK(r.settings(2, 1).padSemitones[1] == 30); // MAJOR, key shift +1, window 1: pad 2 = degree 17 = 29 st
    CHECK(r.settings(0, 1).chokeGroup == 0);       // other scenes untouched
}

TEST_CASE("trigger.engine: choke and gate through the engine, voice × envelope on MAIN", "[trigger][engine]")
{
    voicetest::TestBuffer buf(std::vector<float>(60000, 0.25f));
    auto e = enginetest::make(false);
    for (int t = 0; t < 2; ++t)
    {
        REQUIRE(install(*e, buf, engine::voice::trackSampleSlot(0, t)));
        REQUIRE(e->bridge().post(engine::ParamChange {id("track.chokeGroup"), static_cast<std::uint16_t>(t), 3.0f, 0}));
    }
    REQUIRE(e->bridge().post(engine::ParamChange {id("track.triggerMode"), 1, 1.0f, 0})); // track 2 GATE
    REQUIRE(e->bridge().post(engine::ParamChange {id("env.release"), 1, 0.0f, 0}));
    enginetest::render(*e, 256, 64);
    REQUIRE(e->triggers().push({0, 127, 60, 0}));
    enginetest::render(*e, 1024, 64);
    CHECK(e->trackSource(0).voice().mainActive());
    REQUIRE(e->triggers().push({1, 127, 60, 1}));
    enginetest::render(*e, 1024, 64);
    CHECK_FALSE(e->trackSource(0).voice().active()); // choked
    CHECK(e->trackSource(1).voice().mainActive());
    REQUIRE(e->bridge().post(engine::TriggerNow {1, 0, 60, 1})); // pad up: GATE release
    const auto tail = enginetest::render(*e, 4096, 64);
    CHECK(enginetest::peak(tail.l, 2048) == 0.0f);
    CHECK_FALSE(e->trackSource(1).voice().active());
}

TEST_CASE("trigger.engine: the BROWSE preview plays once into MAIN through aux", "[trigger][engine]")
{
    voicetest::TestBuffer buf(std::vector<float>(2000, 0.5f));
    auto e = enginetest::make(false);
    REQUIRE(e->bridge().post(engine::ParamChange {id("mixer.masterVolume"), 0xFFFF, 100.0f, 0}));
    REQUIRE(install(*e, buf, engine::voice::previewSlot()));
    const auto out = enginetest::render(*e, 4096, 256);
    CHECK(enginetest::peak(out.l, 256, 1500) == Catch::Approx(0.5f * core::GainLaw::gain(50)).epsilon(0.02));
    CHECK(enginetest::peak(out.l, 2300) == 0.0f); // once
}

TEST_CASE("trigger.engine: a Thru track passes the audio input to MAIN (no envelope)", "[trigger][engine]")
{
    auto e = enginetest::make(false);
    REQUIRE(e->bridge().post(engine::ParamChange {id("track.mode"), 4, 2.0f, 0})); // track 5 THRU
    std::vector<float> in(8192), outL(8192), outR(8192);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.3f * std::sin(0.05f * static_cast<float>(i));
    const float* inputs[2] = {in.data(), in.data()};
    float* outs[2] = {outL.data(), outR.data()};
    core::rt::ScopedRealtimeThread realtime;
    for (int done = 0; done < 8192; done += 256)
    {
        const float* ip[2] = {inputs[0] + done, inputs[1] + done};
        float* op[2] = {outs[0] + done, outs[1] + done};
        e->process(engine::AudioIO {op, 1, ip, 2, 256, 0});
    }
    // One block of input latency (Engine::inputLatencyFrames) + the strip at unity.
    float worst = 0.0f;
    for (std::size_t i = 2048; i < 8192; ++i)
        worst = std::max(worst, std::fabs(outL[i] - in[i - static_cast<std::size_t>(engine::Engine::inputLatencyFrames())]));
    CHECK(worst < 1e-6f);
}

TEST_CASE("trigger.params: track.cpp's ids, ranges, defaults, CC and lock flags", "[trigger][params]")
{
    const auto& r = core::builtinParams();
    CHECK(r.errors().empty());
    struct Row
    {
        const char* id;
        float lo, hi, def;
        int cc;
        bool lockable;
        core::ParamScope scope;
    };
    using S = core::ParamScope;
    const Row rows[] = {
        {"track.mode", 0, 3, 0, -1, false, S::Track},           {"track.triggerMode", 0, 1, 0, 16, true, S::Track},
        {"track.chokeGroup", 0, 8, 0, -1, false, S::Track},     {"track.padColor", 0, 15, 0, -1, false, S::Track},
        {"track.padVelocity", 0, 1, 1, -1, false, S::Track},    {"track.inputSource", 0, 3, 0, -1, false, S::Track},
        {"pads.velocityCurve", 0, 7, 3, -1, false, S::Global},  {"pads.fixedVelocity", 1, 127, 100, -1, false, S::Global},
        {"scale.type", 0, 13, 0, -1, false, S::Track},          {"scale.keyShift", -12, 12, 0, -1, false, S::Track},
        {"scale.area", -2, 2, 0, -1, false, S::Track},          {"scaleMidi.type", 0, 13, 0, -1, false, S::Track},
        {"scaleMidi.root", 0, 127, 60, -1, false, S::Track},    {"scaleMidi.area", -2, 2, 0, -1, false, S::Track},
        {"pads.mode", 0, 4, 0, -1, false, S::Global},           {"pads.selected", 0, 15, 0, -1, false, S::Global},
        {"pads.shift", 0, 1, 0, -1, false, S::Global},
    };
    for (const auto& row : rows)
    {
        INFO(row.id);
        const auto* s = r.find(row.id);
        REQUIRE(s != nullptr);
        CHECK(s->minValue == row.lo);
        CHECK(s->maxValue == row.hi);
        CHECK(s->defaultValue == row.def);
        CHECK(s->midiCc == row.cc);
        CHECK(s->lockable == row.lockable);
        CHECK(s->scope == row.scope);
    }
    char buf[32];
    auto fmt = [&](const char* name, float v) {
        const int n = r.find(name)->formatValue(v, buf, sizeof buf);
        return std::string(buf, static_cast<std::size_t>(n));
    };
    CHECK(fmt("track.chokeGroup", 0) == "--");
    CHECK(fmt("track.chokeGroup", 3) == "3");
    CHECK(fmt("scale.type", 0) == "CHROMATIC");
    CHECK(fmt("scale.keyShift", 5) == "+5");
    CHECK(fmt("scaleMidi.root", 60) == "C5");
    CHECK(fmt("pads.selected", 0) == "1");
    CHECK(fmt("pads.velocityCurve", 3) == "LINEAR");
}

TEST_CASE("trigger.engine: a pad hit on track 1 without a sample does not interrupt the test tone", "[trigger][engine]")
{
    auto e = enginetest::make();
    enginetest::render(*e, 4096, 64);
    REQUIRE(e->triggers().push({0, 127, 60, 0}));
    const auto out = enginetest::render(*e, 4096, 64);
    // The tone continues (no dropout block); the ping rides on it.
    float minPeak = 1.0f;
    for (std::size_t b = 0; b + 64 <= out.l.size(); b += 64)
        minPeak = std::min(minPeak, enginetest::peak(out.l, b, b + 64));
    CHECK(minPeak > 0.15f); // a 64-frame window of the 440 Hz tone peaks ≥ 0.18; a dropout block would be 0
    CHECK(enginetest::maxStep(out.l) < 0.2f);
}
