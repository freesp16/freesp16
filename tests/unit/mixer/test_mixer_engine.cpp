// The mixer inside engine::Engine (steps 8b–13) and its parameter catalogue (core/params/catalogue/mixer.cpp).
#include "tests/unit/engine/EngineTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "core/util/GainLaw.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

double dbfs(float peak) { return 20.0 * std::log10(static_cast<double>(peak)); }
} // namespace

TEST_CASE("mixer.engine: the test tone stays at -12 dBFS on MAIN at the default track and master levels", "[mixer][engine]")
{
    auto e = enginetest::make();
    const auto out = enginetest::render(*e, 44100, 256);
    CHECK(dbfs(enginetest::peak(out.l)) == Catch::Approx(-12.04).margin(0.05));
    CHECK(enginetest::peak(out.r) == enginetest::peak(out.l));
    const auto& m = e->bridge().meters();
    CHECK(m.trackPeak[0][0] == Catch::Approx(0.25).epsilon(0.01)); // post-fader track meter
    CHECK(m.masterPeak[0] == Catch::Approx(0.25).epsilon(0.01));
    CHECK(m.sendPeak[0] == 0.0f); // Send FX Off: the return is silence
}

TEST_CASE("mixer.engine: track and mixer parameters reach the strip, the master and the trim", "[mixer][engine]")
{
    auto e = enginetest::make();
    auto& b = e->bridge();
    enginetest::render(*e, 640, 64);
    auto level = [&] {
        const auto out = enginetest::render(*e, 64 * 40, 64);
        return std::pair {enginetest::peak(out.l, out.l.size() - 1024), enginetest::peak(out.r, out.l.size() - 1024)};
    };
    REQUIRE(b.post(engine::ParamChange {id("track.pan"), 0, 32.0f, 0}));
    auto lr = level();
    CHECK(lr.first == Catch::Approx(0.125).epsilon(0.01));
    CHECK(lr.second == Catch::Approx(0.25).epsilon(0.01));
    REQUIRE(b.post(engine::ParamChange {id("track.pan"), 0, 0.0f, 0}));
    REQUIRE(b.post(engine::ParamChange {id("mixer.masterVolume"), 0xFFFF, 50.0f, 0}));
    CHECK(level().first == Catch::Approx(0.25 * static_cast<double>(core::GainLaw::gain(50))).epsilon(0.01));
    REQUIRE(b.post(engine::ParamChange {id("mixer.masterVolume"), 0xFFFF, 100.0f, 0}));
    REQUIRE(b.post(engine::ParamChange {id("mixer.outputTrim"), 0xFFFF, 1.0f, 0}));
    CHECK(dbfs(level().first) == Catch::Approx(-72.04).margin(0.1));
    REQUIRE(b.post(engine::ParamChange {id("mixer.outputTrim"), 0xFFFF, 1023.0f, 0}));
    REQUIRE(b.post(engine::ParamChange {id("track.mute"), 0, 1.0f, 0}));
    CHECK(level().first == 0.0f);
    REQUIRE(b.post(engine::ParamChange {id("track.mute"), 0, 0.0f, 0}));
    REQUIRE(b.post(engine::ParamChange {id("track.solo"), 1, 1.0f, 0})); // track 2 soloed: track 1 drops out
    CHECK(level().first == 0.0f);
    REQUIRE(b.post(engine::ParamChange {id("track.solo"), 1, 0.0f, 0}));
    REQUIRE(b.post(engine::ParamChange {id("track.volume"), 1 << 4, 0.0f, 0})); // scene 2: not the current scene
    CHECK(level().first == Catch::Approx(0.25).epsilon(0.01));
    for (const char* p : {"track.sendAmount", "track.sendChannel", "mixer.sendReturnVolume", "mixer.headphoneVolume",
                          "mixer.softLimiter", "mixer.masterLimiter"})
    {
        INFO(p);
        CHECK(b.post(engine::ParamChange {id(p), 0, 1.0f, 0})); // bound: accepted
    }
    enginetest::render(*e, 640, 64);
}

TEST_CASE("mixer.params: the catalogue's ids, ranges, defaults, CCs and lock flags", "[mixer][params]")
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
    for (const Row row : {Row {"track.volume", 0, 127, 100, 7, true, S::Track},
                          Row {"track.pan", -64, 64, 0, 10, true, S::Track},
                          Row {"track.sendAmount", 0, 127, 0, 17, true, S::Track},
                          Row {"track.mute", 0, 1, 0, 18, true, S::Track},
                          Row {"track.solo", 0, 1, 0, -1, false, S::Track},
                          Row {"track.sendChannel", 0, 1, 0, -1, false, S::Track},
                          Row {"mixer.masterVolume", 0, 127, 100, -1, false, S::Project},
                          Row {"mixer.sendReturnVolume", 0, 127, 100, -1, false, S::Project},
                          Row {"mixer.headphoneVolume", 0, 127, 100, -1, false, S::Project},
                          Row {"mixer.softLimiter", 0, 1, 0, -1, false, S::Project},
                          Row {"mixer.masterLimiter", 0, 1, 0, -1, false, S::Global},
                          Row {"mixer.outputTrim", 0, 1023, 1023, -1, false, S::Global}})
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
    auto fmt = [&](const char* p, float v) {
        const int n = r.find(p)->formatValue(v, buf, sizeof buf);
        return std::string(buf, static_cast<std::size_t>(n));
    };
    // Levels read as their raw values, never in dB (the laws are unchanged).
    CHECK(fmt("track.sendAmount", 127) == "127");
    CHECK(fmt("track.sendAmount", 0) == "0");
    CHECK(fmt("mixer.outputTrim", 1) == "1");
    CHECK(fmt("mixer.outputTrim", 1023) == "1023");
    CHECK(fmt("mixer.masterVolume", 127) == "127");
    CHECK(fmt("mixer.headphoneVolume", 100) == "100");
    CHECK(fmt("track.mute", 1) == "ON");
}
