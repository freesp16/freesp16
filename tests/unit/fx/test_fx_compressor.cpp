// Compressor (spec 02-features/effects/compressor.md): both quirk profiles (FX-1180 through the shell,
// FX-1183), the declared latency (look-ahead, FX-1125), reset (FX-1161), the neutral default, the catalogue and the
// option routes. The behavioural CI gate (static curve, slopes, look-ahead lead, mix law) is
// tests/golden/harness/selftest/test_compressor_behaviour.cpp; the app-maths golden is tests/golden/compressor.
#include "tests/unit/fx/FxTestUtil.h"

#include "core/util/Realtime.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/FxSlotRoutes.h"
#include "engine/fx/effects/Compressor.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using engine::fx::effects::Compressor;

namespace
{
constexpr int N = engine::kBlockFrames;

std::vector<float> noise(int n, unsigned seed, float amp)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-amp, amp);
    std::vector<float> v(static_cast<std::size_t>(n));
    for (auto& x : v)
        x = d(rng);
    return v;
}

// Step fractions of the page's grids.
float thr(int db)
{
    return static_cast<float>(db + 52) / 52.0f;
}
float ratio(double r)
{
    return static_cast<float>((std::lround(r * 10) - 10) / 290.0);
}

void compressing(engine::fx::EffectBase& e)
{
    e.setParam(0, thr(-30));
    e.setParam(1, ratio(10));
    e.setParam(2, 0.0f);
    e.setParam(3, 0.1f);
    e.setParam(4, 0.0f);
    e.setParam(5, 1.0f);
}

// Runs `blocks` blocks of (inL, inR) through process() in place; returns L then R per block, concatenated per channel.
std::vector<float> runLeft(Compressor& c, const std::vector<float>& in, int blocks)
{
    std::vector<float> out;
    const engine::fx::ProcessContext ctx = fxtest::context();
    alignas(16) float l[N], r[N];
    for (int b = 0; b < blocks; ++b)
    {
        std::memcpy(l, in.data() + b * N, sizeof l);
        std::memcpy(r, in.data() + b * N, sizeof r);
        c.process(l, r, ctx);
        out.insert(out.end(), l, l + N);
    }
    return out;
}
} // namespace

TEST_CASE("fx.compressor: the neutral defaults pass the signal within 1e-6 through the shell (FX-092)",
          "[fx][compressor]")
{
    const auto* d = engine::fx::EffectRegistry::builtin().find("compressor");
    REQUIRE(d != nullptr);
    auto e = d->create();
    e->prepare(44100.0, N);
    alignas(16) float sl[N], sr[N];
    engine::fx::FxShell shell;
    shell.prepare(*e, d->shell, 44100.0, engine::fx::SlotKind::Send, sl, sr);
    (void) core::builtinParams(); // the catalogues record their defs when the registry is built
    const auto* set = core::fx::effectParams("compressor");
    REQUIRE(set != nullptr);
    for (const auto& p : set->params)
        shell.setParam(p.k, core::fx::defaultFor(p, core::fx::kSlotSend));
    shell.activate(false);
    const auto in = noise(40 * N, 3, 0.9f);
    const auto ctx = fxtest::context();
    alignas(16) float l[N], r[N];
    for (int b = 0; b < 40; ++b)
    {
        std::memcpy(l, in.data() + b * N, sizeof l);
        std::memcpy(r, in.data() + b * N, sizeof r);
        shell.process(l, r, ctx);
        // After the activation fade (N_shell = 147). Not bit for bit: below the threshold the reference computes
        // y = −160 + (L + 160)·1, whose rounding gives a target gain 1 − O(1e-7) (compressor.py does the same).
        if (b >= 3)
            for (int i = 0; i < N; ++i)
                CHECK(std::abs(l[i] - in[static_cast<std::size_t>(b * N + i)]) <=
                      1e-6f * std::abs(in[static_cast<std::size_t>(b * N + i)]));
    }
}

TEST_CASE("fx.compressor: quirk_compressor FX-1180 — the reference Insert compressor starts 147 samples late",
          "[fx][compressor][quirk]")
{
    const auto* d = engine::fx::EffectRegistry::builtin().find("compressor");
    REQUIRE(d != nullptr);
    CHECK(d->shell.skipWhenSilent);
    CHECK(d->shell.clampOutput);
    const auto in = noise(8 * N, 9, 0.8f);
    for (bool reference : {true, false})
    {
        INFO((reference ? "reference" : "sixteen"));
        auto e = d->create();
        e->prepare(44100.0, N);
        alignas(16) float sl[N], sr[N];
        engine::fx::FxShell shell;
        shell.prepare(*e, d->shell, 44100.0, engine::fx::SlotKind::Insert, sl, sr);
        for (int k = 0; k < 6; ++k)
            shell.setParam(k, k == 0 ? thr(-30) : (k == 1 ? ratio(10) : (k == 3 ? 0.1f : (k == 5 ? 1.0f : 0.0f))));
        shell.setQuirkProfile(reference ? engine::fx::kQuirkProfileReference : engine::fx::kQuirkProfileSixteen);
        shell.activate();
        std::vector<float> out;
        const auto ctx = fxtest::context();
        alignas(16) float l[N], r[N];
        for (int b = 0; b < 8; ++b)
        {
            std::memcpy(l, in.data() + b * N, sizeof l);
            std::memcpy(r, in.data() + b * N, sizeof r);
            shell.process(l, r, ctx);
            out.insert(out.end(), l, l + N);
        }
        int firstDiff = -1;
        for (int i = 0; i < 8 * N && firstDiff < 0; ++i)
            if (out[static_cast<std::size_t>(i)] != in[static_cast<std::size_t>(i)])
                firstDiff = i;
        if (reference)
            CHECK(firstDiff >= 147); // FX-033: the output equals the input for N_shell samples
        else
            CHECK((firstDiff >= 0 && firstDiff <= 1)); // Sixteen: processing from the first samples (FX-1180 fix)
    }
}

TEST_CASE("fx.compressor: quirk_compressor FX-1183 — the reference hold length starts at 0, inaudibly",
          "[fx][compressor][quirk]")
{
    Compressor ref, six;
    ref.setQuirkProfile(Compressor::kQuirkHoldInit);
    ref.prepare(44100.0, N);
    six.prepare(44100.0, N);
    CHECK(ref.holdLimit() == 0);
    CHECK(six.holdLimit() == 441); // min(release/4, 10 ms) of the constructor's release (4000 ms)
    compressing(ref);
    compressing(six);
    const auto in = noise(60 * N, 5, 0.9f);
    CHECK(runLeft(ref, in, 60) == runLeft(six, in, 60)); // the hold only counts after an attack, which sets it
}

TEST_CASE("fx.compressor: declared latency = the look-ahead delay, measured (FX-1125, contract)", "[fx][compressor]")
{
    for (double fs : {44100.0, 48000.0, 96000.0})
    {
        INFO("fs " << fs);
        Compressor c;
        c.prepare(fs, N);
        CHECK(c.latencySamples() == 0);
        c.setOption(Compressor::kOptLookAhead, 1);
        const int want = static_cast<int>(std::lround(0.002 * fs)); // round(0.002·fs) (our choice): 88 / 96 / 192
        CHECK(c.latencySamples() == want);
        c.setParam(0, 1.0f); // 0 dB threshold: an impulse of 0.5 is not compressed
        c.setParam(1, ratio(4));
        c.setParam(5, 1.0f);
        c.reset(); // the crossfade rests on the delayed side
        std::vector<float> imp(static_cast<std::size_t>(8 * N), 0.0f);
        imp[0] = 0.5f;
        const auto y = runLeft(c, imp, 8);
        int first = -1;
        for (int i = 0; i < 8 * N && first < 0; ++i)
            if (y[static_cast<std::size_t>(i)] != 0.0f)
                first = i;
        CHECK(first == want);
        CHECK(y[static_cast<std::size_t>(want)] == 0.5f);
    }
}

TEST_CASE("fx.compressor: reset (activation) uses make-up 1 until the next block (FX-1161)", "[fx][compressor]")
{
    Compressor c;
    c.prepare(44100.0, N);
    compressing(c);
    c.setParam(4, 12.0f / 50.0f); // +12 dB
    c.setParam(5, 0.5f);
    c.reset();
    CHECK(c.telemetry(engine::fx::kTelemetryDry) == 0.5f);
    CHECK(c.telemetry(engine::fx::kTelemetryWet) == 0.5f);
    const auto in = noise(N, 2, 0.5f);
    runLeft(c, in, 1);
    const float makeup = static_cast<float>(std::pow(10.0, 12.0 / 20.0));
    CHECK(std::abs(c.makeup() - makeup) <= 1e-6f * makeup);
    CHECK(c.telemetry(engine::fx::kTelemetryWet) == 0.5f * c.makeup());
    CHECK(c.telemetry(Compressor::kTelGainReduction) > 0.0f);
}

TEST_CASE("fx.compressor: catalogue — slots, page order, grids, defaults, texts, options per slot (FX-067/069/092)",
          "[fx][compressor]")
{
    const auto& reg = core::builtinParams();
    const auto* d = engine::fx::EffectRegistry::builtin().find("compressor");
    REQUIRE(d != nullptr);
    CHECK(d->slots == engine::fx::kAllSlotKinds);
    CHECK(d->params[0] == "fx.compressor.ratio");
    CHECK(d->params[3] == "fx.compressor.threshold");
    CHECK(d->visual == "meters");
    CHECK(d->options.size() == 3);
    CHECK(d->options[Compressor::kOptLookAhead] == "fx.compressor.lookAhead");
    CHECK(d->options[Compressor::kOptDetect] == "fx.compressor.type2");
    CHECK(d->options[Compressor::kOptKnee] == "fx.compressor.type1");
    CHECK(reg.find("fx1.compressor.lookAhead") == nullptr); // OQ-15: the Insert panel's cell 1 stays empty
    REQUIRE(reg.find("send.compressor.lookAhead") != nullptr);
    REQUIRE(reg.find("master.compressor.lookAhead") != nullptr);
    CHECK(reg.find("fx2.compressor.type2")->enumLabels.size() == 2);
    CHECK(reg.find("fx1.compressor.type1")->type == core::ParamType::Bool);
    char b[32];
    auto text = [&](std::string_view id, float v)
    {
        const auto* s = reg.find(id);
        REQUIRE(s != nullptr);
        s->format(v, b, sizeof b);
        return std::string(b);
    };
    CHECK(text("fx1.compressor.threshold", reg.find("fx1.compressor.threshold")->defaultValue) == "0 dB");
    CHECK(text("fx1.compressor.threshold", 0.0f) == "-52 dB");
    CHECK(text("send.compressor.ratio", 0.0f) == "1.0:1");
    CHECK(text("send.compressor.ratio", 1.0f) == "30.0:1");
    CHECK(text("fx1.compressor.attack", 0.0f) == "0.05 ms");
    CHECK(text("fx1.compressor.attack", 19.0f / 1018.0f) == "1.00 ms");
    CHECK(text("fx1.compressor.attack", 20.0f / 1018.0f) == "2 ms");
    CHECK(text("fx1.compressor.attack", 1.0f) == "1000 ms");
    CHECK(text("fx1.compressor.release", 1.0f) == "4000 ms");
    CHECK(text("fx1.compressor.gain", 1.0f) == "50 dB");
    CHECK(text("master.compressor.mix", 1.0f) == "100 %");
    CHECK(reg.find("fx1.compressor.ratio")->lockable);
    CHECK_FALSE(reg.find("send.compressor.ratio")->lockable);
    CHECK(reg.find("master.compressor.mix")->defaultValue == 1.0f);
    // the routes: options by their position in the descriptor
    engine::fx::FxSlotRoutes routes;
    const auto& er = engine::fx::EffectRegistry::builtin();
    const auto ins = engine::fx::engineTypes(er, engine::fx::SlotKind::Insert);
    const auto snd = engine::fx::engineTypes(er, engine::fx::SlotKind::Send);
    const auto mst = engine::fx::engineTypes(er, engine::fx::SlotKind::Master);
    const auto ids = fxtest::builtinIds();
    routes.build(ins, snd, mst, ids, core::fx::internalIndexTable(reg));
    CHECK(routes.find(fxtest::idOf("send.compressor.lookAhead")).index == Compressor::kOptLookAhead);
    CHECK(routes.find(fxtest::idOf("fx1.compressor.type2")).index == Compressor::kOptDetect);
    CHECK(routes.find(fxtest::idOf("master.compressor.type1")).index == Compressor::kOptKnee);
    CHECK(routes.find(fxtest::idOf("fx2.compressor.threshold")).kind == engine::fx::FxRoute::Kind::Knob);
    CHECK(routes.find(fxtest::idOf("fx2.compressor.threshold")).index == 0);
    CHECK(routes.find(fxtest::idOf("fx2.compressor.ratio")).index == 1);
}

TEST_CASE("fx.compressor: the detector sees the undelayed input; the gain leads the audio by the look-ahead (FX-1125)",
          "[fx][compressor]")
{
    Compressor c;
    c.prepare(44100.0, N);
    c.setParam(0, thr(-40));
    c.setParam(1, ratio(20));
    c.setParam(2, 0.0f);
    c.setParam(3, 0.2f);
    c.setParam(5, 1.0f);
    c.setOption(Compressor::kOptLookAhead, 1);
    c.reset();
    // silence, then a 0.8 DC step at sample 3 · 64 + 10
    std::vector<float> in(static_cast<std::size_t>(10 * N), 0.0f);
    for (std::size_t i = 3 * N + 10; i < in.size(); ++i)
        in[i] = 0.8f;
    const auto y = runLeft(c, in, 10);
    const std::size_t arrive = 3 * N + 10 + 88;
    CHECK(y[arrive - 1] == 0.0f);
    CHECK(y[arrive] > 0.0f);
    CHECK(y[arrive] < 0.8f * 0.5f); // already reduced when the step reaches the output
}
