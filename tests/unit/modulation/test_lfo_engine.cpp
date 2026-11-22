// The LFO inside engine::Engine: lfo.* ParamChanges through the bridge, the matrix applied once per block
// between the step locks and the voices, track and FX destinations audible, an FX destination of another type
// bit-exact silent, all under the real-time contract (OfflineDriver marks the thread real-time: any allocation
// aborts under APP_RT_CHECKS).
#include "tests/unit/engine/EngineTestUtil.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/modulation/Lfo.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace
{
core::ParamId pid(std::string_view id)
{
    const auto p = core::builtinParams().idOf(id);
    REQUIRE(p.has_value());
    return *p;
}

void post(engine::Engine& e, std::string_view id, float v, std::uint16_t scope = 0)
{
    REQUIRE(e.bridge().post(engine::ParamChange {pid(id), scope, v, 0}));
}

float insertType(std::string_view effect)
{
    const auto list = engine::fx::engineTypes(engine::fx::EffectRegistry::builtin(), engine::fx::SlotKind::Insert);
    const int i = engine::fx::typeIndexOf(list, effect);
    REQUIRE(i >= 0);
    return static_cast<float>(i + 1);
}

// Peaks of consecutive 64-frame blocks.
std::vector<float> blockPeaks(const std::vector<float>& v, std::size_t from)
{
    std::vector<float> p;
    for (std::size_t i = from; i + 64 <= v.size(); i += 64)
        p.push_back(enginetest::peak(v, i, i + 64));
    return p;
}

std::unique_ptr<engine::Engine> makeEngine()
{
    engine::EngineOptions o; // the TEMPORARY test tone on track 1 is the signal (−12 dBFS)
    auto e = std::make_unique<engine::Engine>(enginetest::fullSetup(), o);
    e->prepare(44100.0, 512);
    return e;
}

void lfoTo(engine::Engine& e, std::string_view dest, engine::modulation::LfoShape shape, float speed, float depth)
{
    post(e, "lfo.shape", static_cast<float>(shape));
    post(e, "lfo.speed", speed);
    post(e, "lfo.depth", depth);
    post(e, "lfo.destination", static_cast<float>(pid(dest).value));
}
} // namespace

TEST_CASE("lfo.engine: an LFO on track volume modulates the track; bypass returns the base (LFO-005/009)",
          "[lfo][modulation]")
{
    auto e = makeEngine();
    const auto steady = blockPeaks(enginetest::render(*e, 8192).l, 4096);
    const auto [lo0, hi0] = std::minmax_element(steady.begin(), steady.end());
    REQUIRE(*lo0 > 0.2f);
    CHECK(*hi0 - *lo0 < 0.01f);

    // A 50 % pulse at depth 127 around volume 100: clamp(100/127 ± 1) = index 127 (+6 dB) and index 0 (silence).
    lfoTo(*e, "track.volume", engine::modulation::LfoShape::Pulse50, 64.0f, 127.0f); // ≈ 0.63 Hz
    const auto mod = blockPeaks(enginetest::render(*e, 3 * 44100).l, 4096);
    const auto [lo, hi] = std::minmax_element(mod.begin(), mod.end());
    CHECK(*hi > 1.8f * *hi0); // +6 dB
    CHECK(*lo < 1e-4f);       // fader index 0
    CHECK(e->modMatrix().modulated(0, pid("track.volume")));
    CHECK(e->lfo().appliedDestination(0) == pid("track.volume"));

    post(*e, "lfo.bypass", 1.0f);
    const auto back = blockPeaks(enginetest::render(*e, 16384).l, 8192);
    const auto [lo2, hi2] = std::minmax_element(back.begin(), back.end());
    CHECK(std::fabs(*lo2 - *lo0) < 0.01f);
    CHECK(std::fabs(*hi2 - *hi0) < 0.01f);
    CHECK_FALSE(e->modMatrix().modulated(0, pid("track.volume")));
}

TEST_CASE("lfo.engine: an FX destination modulates its effect; another type's destination is bit-exact silent",
          "[lfo][modulation]")
{
    // Two engines with gainTest in track 1's FX1; one LFOs fx1.none.mix (not in the slot): identical outputs.
    auto a = makeEngine(), b = makeEngine();
    for (auto* e : {a.get(), b.get()})
        post(*e, "fx1.type", insertType("gainTest"));
    lfoTo(*b, "fx1.none.mix", engine::modulation::LfoShape::Sine, 100.0f, 127.0f);
    const auto ra = enginetest::render(*a, 44100), rb = enginetest::render(*b, 44100);
    CHECK(ra.l == rb.l);
    CHECK(ra.r == rb.r);
    CHECK(b->modMatrix().stale(0, pid("fx1.none.mix")));

    // Pointed at the effect that is there: the gain moves (a square around the stored 100/127 → index 127 / 0).
    lfoTo(*b, "fx1.gainTest.gain", engine::modulation::LfoShape::Pulse50, 64.0f, 127.0f);
    const auto mod = blockPeaks(enginetest::render(*b, 3 * 44100).l, 4096);
    const auto [lo, hi] = std::minmax_element(mod.begin(), mod.end());
    CHECK(*hi > 0.4f);
    CHECK(*lo < 1e-4f);
    CHECK_FALSE(b->modMatrix().stale(0, pid("fx1.gainTest.gain")));
}

TEST_CASE("lfo.engine: SYNC to the transport, scene values, every destination kind under the real-time contract",
          "[lfo][modulation]")
{
    auto e = makeEngine();
    post(*e, "fx1.type", insertType("gainTest"));
    post(*e, "lfo.sync", 1.0f);
    post(*e, "lfo.triggerMode", 0.0f); // FREE: from the transport start
    lfoTo(*e, "playback.pitch", engine::modulation::LfoShape::Triangle, 60.0f, 90.0f);
    for (int t = 1; t < 16; ++t) // every other track's LFO on a different kind of destination
    {
        const std::uint16_t sc = static_cast<std::uint16_t>(t);
        const char* dests[] = {"track.volume", "track.pan", "fx1.gainTest.gain", "playback.pitch", "fx2.none.mix"};
        post(*e, "lfo.destination", static_cast<float>(pid(dests[t % 5]).value), sc);
        post(*e, "lfo.shape", static_cast<float>(t % 7), sc);
        post(*e, "lfo.speed", static_cast<float>(t * 8), sc);
    }
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    const auto out = enginetest::render(*e, 2 * 44100); // aborts on any allocation (APP_RT_CHECKS)
    CHECK(enginetest::peak(out.l) > 0.0f);
    for (int t = 0; t < 16; ++t)
        CHECK(e->lfo().appliedDestination(t).valid());
    // SYNC FREE: the phase is the transport's PPQ / quarters per cycle (+ offset 0).
    const auto& l = e->lfo().lfo(0);
    REQUIRE(l.syncDivision() >= 0);
    CHECK(l.elapsed() > 0.0);

    // Scene 2 holds its own LFO settings (none): after the switch track 1's LFO releases its destination.
    post(*e, "scene.queue", 16.0f, 0xFFFF); // scene 2, pattern 1
    enginetest::render(*e, 4 * 44100);
    REQUIRE(e->bridge().playhead().scene == 1);
    CHECK_FALSE(e->lfo().appliedDestination(0).valid());
    CHECK_FALSE(e->modMatrix().modulated(0, pid("playback.pitch")));
}
