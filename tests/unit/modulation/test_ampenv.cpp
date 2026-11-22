// AHR amp envelope behaviour (spec 02-features/amp-envelope.md ENV-002…011, tracks.md TRK-010/011): timing, shapes
// against their closed forms, retrigger from the current level, GATE release, velocity, sync, bypass, kill, locks,
// Thru, sample-accurate frames, SoA bank = single envelopes, real-time safety.
#include "tests/unit/modulation/test_ampenv_util.h"

#include "core/util/GainLaw.h"
#include "core/util/Realtime.h"
#include "engine/modulation/AmpEnvelopeBank.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

using namespace ampenv_test;
using engine::modulation::AmpEnvelope;
using engine::modulation::AmpEnvParams;
using engine::modulation::AmpEnvStage;
using engine::trigger::TriggerKind;
using engine::trigger::TriggerSource;

namespace
{
AmpEnvelope make(const AmpEnvParams& p, double bpm = 120.0)
{
    AmpEnvelope e;
    e.prepare(kRate);
    e.setParams(p, bpm);
    return e;
}

// First frame at which the gain equals `v` (exactly), or -1.
std::int64_t firstAt(const std::vector<float>& g, float v)
{
    const auto it = std::find(g.begin(), g.end(), v);
    return it == g.end() ? -1 : static_cast<std::int64_t>(it - g.begin());
}

double maxStep(const std::vector<float>& g, std::size_t from, std::size_t to)
{
    double m = 0;
    for (std::size_t i = from + 1; i < to && i < g.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(g[i]) - static_cast<double>(g[i - 1])));
    return m;
}
} // namespace

TEST_CASE("ampenv.timing: attack 0 / 64 / 127 reach the peak at 0, 132.1 ms and 32 s (± 1 sample)", "[ampenv]")
{
    const float peak = core::GainLaw::gain(127);
    for (auto [index, seconds] : {std::pair{0, 0.0}, std::pair{64, 0.1321}, std::pair{127, 32.0}})
    {
        INFO("attack index " << index);
        auto env = make(params(index, 128, 0));
        env.trigger(hit(0), false);
        std::vector<float> g;
        render(env, g, static_cast<int>(framesOf(seconds) / kBlock) + 2);
        const auto at = firstAt(g, peak); // frame whose gain is the peak = attack length − 1 (frame 0 = 1st step)
        const auto expected =
            std::max<std::int64_t>(framesOf(core::ampenv::envSecondsExact(
                                       core::ampenv::EnvStage::Attack, static_cast<std::uint8_t>(index), false, 120)) -
                                       1,
                                   0);
        CHECK(std::llabs(at - expected) <= 1);
        CHECK(std::fabs(static_cast<double>(at + 1) / kRate - seconds) <= 0.00006 + 1.0 / kRate);
        if (index > 0)
            CHECK(g[static_cast<std::size_t>(at) - 1] < peak);
    }
}

TEST_CASE("ampenv.infinity: HOLD = INF and RELEASE = INF never finish", "[ampenv]")
{
    auto env = make(params(10, 128, 128)); // one shot, hold INF
    env.trigger(hit(0), false);
    std::vector<float> g;
    render(env, g, static_cast<int>(10.0 * kRate / kBlock));
    CHECK_FALSE(env.finished());
    CHECK(env.stage() == AmpEnvStage::Hold);
    CHECK(g.back() == core::GainLaw::gain(127));

    auto gate = make(params(0, 128, 128));
    gate.trigger(hit(0), true);
    gate.release(5);
    g.clear();
    render(gate, g, static_cast<int>(5.0 * kRate / kBlock));
    CHECK_FALSE(gate.finished());
    CHECK(gate.stage() == AmpEnvStage::Release);
    CHECK(g.back() == core::GainLaw::gain(127)); // an infinite release stays at the level reached

    // hold finite, release INF: same, after the hold
    auto r = make(params(0, 40, 128));
    r.trigger(hit(0), false);
    g.clear();
    render(r, g, 400);
    CHECK_FALSE(r.finished());
    CHECK(r.stage() == AmpEnvStage::Release);
}

TEST_CASE("ampenv.shape: LINEAR and CURVE match their closed forms within 1e-5", "[ampenv]")
{
    const double P = static_cast<double>(core::GainLaw::gain(127));
    const int ai = 80, hi = 110, ri = 70;
    using core::ampenv::EnvStage;
    const auto Na = framesOf(core::ampenv::envSecondsExact(EnvStage::Attack, ai, false, 120));
    const auto Nh = framesOf(core::ampenv::envSecondsExact(EnvStage::Hold, hi, false, 120));
    const auto Nr = framesOf(core::ampenv::envSecondsExact(EnvStage::Release, ri, false, 120));
    REQUIRE(Na < Nh);
    for (int curve = 0; curve < 4; ++curve)
    {
        const bool ca = curve & 1, cr = curve & 2;
        INFO("attack curve " << ca << ", release curve " << cr);
        auto env = make(params(ai, hi, ri, ca, cr));
        env.trigger(hit(0), false);
        std::vector<float> g;
        render(env, g, static_cast<int>((Nh + Nr) / kBlock) + 3);
        const double ra = std::pow(1e-3, 1.0 / static_cast<double>(Na));
        const double rr = std::pow(1e-4, 1.0 / static_cast<double>(Nr));
        double worst = 0;
        for (std::int64_t k = 0; k < static_cast<std::int64_t>(g.size()); ++k)
        {
            double ref;
            if (k < Na - 1)
                ref = ca ? P - P * std::pow(ra, static_cast<double>(k + 1)) : P * static_cast<double>(k + 1) / Na;
            else if (k < Nh)
                ref = P;
            else if (k < Nh + Nr - 1)
            {
                const auto j = static_cast<double>(k - Nh + 1);
                ref = cr ? P * std::pow(rr, j) : P - P * j / static_cast<double>(Nr);
            }
            else
                ref = 0.0;
            worst = std::max(worst, std::fabs(static_cast<double>(g[static_cast<std::size_t>(k)]) - ref));
        }
        CHECK(worst <= 1e-5);
        CHECK(env.finished());
        CHECK(g.back() == 0.0f);
    }
}

TEST_CASE("ampenv.shape: a 32 s CURVE attack keeps its closed form (double state, no drift)", "[ampenv]")
{
    const double P = static_cast<double>(core::GainLaw::gain(100)); // 1.0
    auto p = params(127, 128, 0, true);
    p.velocity = 100;
    auto env = make(p);
    env.trigger(hit(0), false);
    std::vector<float> g;
    render(env, g, static_cast<int>(32.0 * kRate / kBlock) + 2);
    const auto Na = framesOf(32.0);
    const double ra = std::pow(1e-3, 1.0 / static_cast<double>(Na));
    double worst = 0;
    for (std::int64_t k = 0; k < Na - 1; k += 97)
        worst = std::max(worst, std::fabs(static_cast<double>(g[static_cast<std::size_t>(k)]) -
                                          (P - P * std::pow(ra, double(k + 1)))));
    CHECK(worst <= 1e-5);
    CHECK(g[static_cast<std::size_t>(Na - 1)] == 1.0f);
}

TEST_CASE("ampenv.retrigger: a FULL restart at 50 % starts from 50 %, no step > 1/1000 per frame", "[ampenv]")
{
    for (bool curve : {false, true})
    {
        INFO("curve " << curve);
        auto p = params(90, 128, 0, curve);
        p.velocity = 100; // peak 1.0
        auto env = make(p);
        env.trigger(hit(0), false);
        std::vector<float> g;
        while (env.currentGain() < 0.5f)
            render(env, g, 1);
        const float before = env.currentGain();
        env.trigger(hit(17), false);
        render(env, g, 400);
        const std::size_t at = g.size() - 400 * kBlock + 17;
        CHECK(std::fabs(g[at] - before) < 0.002f);
        CHECK(maxStep(g, 0, g.size()) <= 1.0 / 1000);
        CHECK(env.stage() == AmpEnvStage::Attack);
    }
    // attack 0 restarting from a level: a 32-frame de-click ramp instead of a step (our choice)
    auto p = params(0, 128, 128);
    p.velocity = 100;
    auto env = make(p);
    env.trigger(hit(0, 64), false);
    std::vector<float> g;
    render(env, g, 2);
    env.trigger(hit(0, 127), false);
    render(env, g, 2);
    CHECK(maxStep(g, 1, g.size()) <= 1.0 / 32 + 1e-6);
    CHECK(g.back() == 1.0f);
}

TEST_CASE("ampenv.retrigger: HALF and retrigger repeats restart only with Retrigger on (ENV-007)", "[ampenv]")
{
    for (bool retrigger : {false, true})
    {
        INFO("retrigger " << retrigger);
        auto p = params(0, 30, 90);
        p.retrigger = retrigger;
        auto env = make(p);
        env.trigger(hit(0), false);
        std::vector<float> g;
        render(env, g, 100); // well into the release
        const float level = env.currentGain();
        REQUIRE(env.stage() == AmpEnvStage::Release);
        env.trigger(hit(0, 127, TriggerKind::Half), false);
        render(env, g, 1);
        CHECK((env.stage() == AmpEnvStage::Hold) == retrigger);
        CHECK((env.currentGain() > level) == retrigger);
        render(env, g, 50);
        env.trigger(hit(0, 127, TriggerKind::Full, TriggerSource::Retrigger), false);
        render(env, g, 1);
        CHECK((env.stage() == AmpEnvStage::Hold) == retrigger);
        // FULL from a pad always restarts
        env.trigger(hit(3), false);
        render(env, g, 1);
        CHECK(env.stage() == AmpEnvStage::Hold);
    }
    // HALF on an idle track starts nothing (SEQ-011, our choice); LockOnly never triggers
    auto p = params(0, 128, 0);
    p.retrigger = true;
    auto idle = make(p);
    idle.trigger(hit(0, 127, TriggerKind::Half), false);
    idle.trigger(hit(5, 127, TriggerKind::LockOnly), false);
    std::vector<float> g;
    render(idle, g, 1);
    CHECK(idle.finished());
    CHECK(*std::max_element(g.begin(), g.end()) == 0.0f);
}

TEST_CASE("ampenv.gate: a GATE release at frame 20 enters the release at frame 20 (ENV-008)", "[ampenv]")
{
    auto env = make(params(0, 128, 60));
    env.trigger(hit(0), true);
    std::vector<float> g;
    render(env, g, 3);
    env.release(20);
    render(env, g, 1);
    const float peak = core::GainLaw::gain(127);
    const std::size_t base = 3 * kBlock;
    CHECK(g[base + 19] == peak);
    CHECK(g[base + 20] < peak);
    CHECK(g[base + 20] > peak * 0.99f);
    CHECK(env.stage() == AmpEnvStage::Release);
    // GATE with finite hold: A → H → R while held (enumerations.md), release then does nothing more
    auto held = make(params(0, 20, 10));
    held.trigger(hit(0), true);
    g.clear();
    render(held, g, 200);
    CHECK(held.finished());
    // One Shot ignores the pad release
    auto one = make(params(0, 128, 60));
    one.trigger(hit(0), false);
    one.release(10);
    g.clear();
    render(one, g, 4);
    CHECK(one.stage() == AmpEnvStage::Hold);
    // RELEASE 0 still fades over 32 frames (no click, our choice)
    auto zero = make(params(0, 128, 0));
    zero.trigger(hit(0), true);
    zero.release(0);
    g.clear();
    render(zero, g, 2);
    CHECK(g[30] > 0.0f);
    CHECK(g[31] == 0.0f);
    CHECK(maxStep(g, 0, 64) <= static_cast<double>(core::GainLaw::gain(127)) / 32 + 1e-6);
    CHECK(zero.finished());
}

TEST_CASE("ampenv.hold: HOLD shorter than ATTACK releases from the level reached at the hold time (ENV-002)",
          "[ampenv]")
{
    auto p = params(100, 60, 50);
    p.velocity = 100;
    auto env = make(p);
    env.trigger(hit(0), false);
    std::vector<float> g;
    render(env, g, 1000);
    using core::ampenv::EnvStage;
    const auto Na = framesOf(core::ampenv::envSecondsExact(EnvStage::Attack, 100, false, 120));
    const auto Nh = framesOf(core::ampenv::envSecondsExact(EnvStage::Hold, 60, false, 120));
    REQUIRE(Nh < Na);
    const auto top = std::max_element(g.begin(), g.end());
    CHECK(top - g.begin() == Nh - 1);
    CHECK(*top == Catch::Approx(static_cast<double>(Nh) / Na).margin(1e-5));
    CHECK(env.finished());
}

TEST_CASE("ampenv.velocity: VELOCITY is the ceiling, scaled by (vel/127)² when sensitive (ENV-005, PLY-041)",
          "[ampenv]")
{
    auto p = params(0, 128, 0);
    p.velocitySensitive = true;
    auto env = make(p);
    env.trigger(hit(0, 64), false);
    std::vector<float> g;
    render(env, g, 1);
    CHECK(g[10] ==
          Catch::Approx(static_cast<double>(core::GainLaw::gain(127)) * (64.0 / 127) * (64.0 / 127)).epsilon(1e-6));
    p.velocitySensitive = false;
    p.velocity = 90;
    auto fixed = make(p);
    fixed.trigger(hit(0, 64), false);
    g.clear();
    render(fixed, g, 1);
    CHECK(g[10] == core::GainLaw::gain(90));
}

TEST_CASE("ampenv.sync: SYNC lengths follow the tempo given to setParams (ENV-003)", "[ampenv]")
{
    auto p = params(72, 128, 0); // 1/4
    p.sync = true;
    p.velocity = 100;
    for (double bpm : {120.0, 90.0})
    {
        INFO("bpm " << bpm);
        auto env = make(p, bpm);
        env.trigger(hit(0), false);
        std::vector<float> g;
        render(env, g, static_cast<int>(kRate / kBlock) + 2);
        CHECK(std::llabs(firstAt(g, 1.0f) + 1 - framesOf(60.0 / bpm)) <= 1);
    }
}

TEST_CASE("ampenv.bypass: gain 1 while the voice plays, 2 ms ramps at start and stop (ENV-009)", "[ampenv]")
{
    auto p = params(127, 128, 127);
    p.bypass = true;
    auto env = make(p);
    env.trigger(hit(0), true);
    std::vector<float> g;
    render(env, g, 3);
    const auto ramp = framesOf(0.002);
    CHECK(firstAt(g, 1.0f) == ramp - 1);
    CHECK(env.stage() == AmpEnvStage::Bypass);
    // unity is bit-transparent
    alignas(16) float l[kBlock], r[kBlock];
    for (int i = 0; i < kBlock; ++i)
        l[i] = r[i] = 0.1f * static_cast<float>(i) - 3.0f;
    env.process(l, r);
    for (int i = 0; i < kBlock; ++i)
        CHECK(l[i] == 0.1f * static_cast<float>(i) - 3.0f);
    env.release(0);
    g.clear();
    render(env, g, 3);
    CHECK(firstAt(g, 0.0f) == ramp - 1);
    CHECK(env.finished());
}

TEST_CASE("ampenv.bypass: a BYPASS flip applies to the sounding note with the 2 ms ramp (ENV-009, our choice)",
          "[ampenv]")
{
    using core::ampenv::EnvStage;
    const auto ramp = framesOf(0.002);
    auto p = params(0, 128, 0); // one shot, HOLD INF
    p.velocity = 80;
    const float peak = core::GainLaw::gain(80);
    REQUIRE(peak < 0.9f);
    auto env = make(p);
    env.trigger(hit(0), false);
    std::vector<float> g;
    render(env, g, 2);
    CHECK(g.back() == peak);
    p.bypass = true; // on: ramp to unity now, not at the next note
    env.setParams(p, 120.0);
    g.clear();
    render(env, g, 3);
    CHECK(g[0] > peak);
    CHECK(g[0] < 1.0f);
    CHECK(firstAt(g, 1.0f) == ramp - 1);
    CHECK(maxStep(g, 0, g.size()) <= (1.0 - static_cast<double>(peak)) / static_cast<double>(ramp) + 1e-6);
    CHECK(env.stage() == AmpEnvStage::Bypass);
    p.bypass = false; // off: back to the note's level (HOLD INF: the peak, held)
    env.setParams(p, 120.0);
    g.clear();
    render(env, g, 3);
    CHECK(firstAt(g, peak) == ramp - 1);
    CHECK(maxStep(g, 0, g.size()) <= (1.0 - static_cast<double>(peak)) / static_cast<double>(ramp) + 1e-6);
    CHECK(env.stage() == AmpEnvStage::Hold);
    CHECK(g.back() == peak);

    // A note started bypassed, BYPASS off within its HOLD: the peak for the rest of the hold, then the release
    auto q = params(0, 60, 40);
    q.velocity = 80;
    q.bypass = true;
    auto held = make(q);
    held.trigger(hit(0), false);
    g.clear();
    render(held, g, 4);
    CHECK(held.stage() == AmpEnvStage::Bypass);
    q.bypass = false;
    held.setParams(q, 120.0);
    const auto Nh = framesOf(core::ampenv::envSecondsExact(EnvStage::Hold, 60, false, 120));
    REQUIRE(Nh > 4 * kBlock + ramp);
    g.clear();
    render(held, g, static_cast<int>(Nh / kBlock) + 2);
    CHECK(firstAt(g, peak) == ramp - 1);
    CHECK(g[static_cast<std::size_t>(Nh - 4 * kBlock - 2)] == peak); // held until the note's own HOLD end …
    CHECK(g.back() < peak);                                          // … then released
    CHECK(held.stage() == AmpEnvStage::Release);

    // … and past its HOLD: the release at once, from the current level (no step)
    q.bypass = true;
    held.setParams(q, 120.0); // the releasing one shot is still playing: bypass ramps it up again
    g.clear();
    render(held, g, 2);
    CHECK(held.stage() == AmpEnvStage::Bypass);
    q.bypass = false;
    held.setParams(q, 120.0);
    g.clear();
    render(held, g, 1);
    CHECK(held.stage() == AmpEnvStage::Release);
    CHECK(g[0] < 1.0f);
    CHECK(g[0] > 0.99f);

    // A released GATE note keeps fading out
    auto gate = make(params(0, 128, 64));
    gate.trigger(hit(0), true);
    gate.release(0);
    g.clear();
    render(gate, g, 1);
    REQUIRE(gate.stage() == AmpEnvStage::Release);
    auto r = params(0, 128, 64);
    r.bypass = true;
    gate.setParams(r, 120.0);
    render(gate, g, 1);
    CHECK(gate.stage() == AmpEnvStage::Release);
    CHECK(g[kBlock] < g[kBlock - 1]);
}

TEST_CASE("ampenv.kill: 64-frame de-click to 0, then finished", "[ampenv]")
{
    auto env = make(params(0, 128, 128));
    env.trigger(hit(0), false);
    std::vector<float> g;
    render(env, g, 1);
    env.kill(8);
    render(env, g, 2);
    CHECK(g[64 + 7] == core::GainLaw::gain(127));
    CHECK(g[64 + 8 + 62] > 0.0f);
    CHECK(g[64 + 8 + 63] == 0.0f);
    CHECK(env.finished());
    // Stop events kill as well
    env.trigger(hit(0), false);
    env.trigger(hit(40, 0, TriggerKind::Stop), false);
    g.clear();
    render(env, g, 3);
    CHECK(env.finished());
}

TEST_CASE("ampenv.locks: velocity/attack/hold/release locks apply at the trigger and revert at the next (ENV-010)",
          "[ampenv]")
{
    float values[35] = {};
    values[11] = 100; // velocity → gain 1.0
    values[12] = 64;  // attack 132 ms
    values[13] = 128; // hold INF
    values[14] = 30;
    engine::trigger::LockSet locks;
    locks.mask = (1ull << 11) | (1ull << 12) | (1ull << 13) | (1ull << 14);
    locks.values = values;
    auto env = make(params(0, 10, 0)); // base: instant attack, short hold
    env.trigger(hit(0, 127, TriggerKind::Full, TriggerSource::Sequencer, 0, &locks), false);
    std::vector<float> g;
    render(env, g, 400);
    CHECK(std::llabs(firstAt(g, 1.0f) + 1 - framesOf(0.1321)) <= 2);
    CHECK(env.stage() == AmpEnvStage::Hold);
    env.trigger(hit(0), false); // no locks: back to the base values (attack 0 from a level: the 32-frame de-click)
    g.clear();
    render(env, g, 1);
    CHECK(g[31] == core::GainLaw::gain(127));
    CHECK(env.stage() == AmpEnvStage::Hold);
    render(env, g, 3);
    CHECK(env.finished()); // base hold 10 and release 0 are back as well
}

TEST_CASE("ampenv.frames: events are sample-accurate inside the block", "[ampenv]")
{
    auto env = make(params(0, 128, 0));
    env.trigger(hit(37), false);
    std::vector<float> g;
    render(env, g, 1);
    for (int i = 0; i < 37; ++i)
        CHECK(g[static_cast<std::size_t>(i)] == 0.0f);
    CHECK(g[37] == core::GainLaw::gain(127));
    CHECK_FALSE(env.finished());
    // a trigger is pending until its block is processed: not finished
    auto fresh = make(params(0, 0, 0));
    fresh.trigger(hit(63), false);
    CHECK_FALSE(fresh.finished());
}

TEST_CASE("ampenv.thru: a Thru track has no envelope", "[ampenv]")
{
    auto env = make(params(127, 128, 0));
    env.setThru(true);
    env.trigger(hit(0), false);
    alignas(16) float l[kBlock], r[kBlock];
    std::fill(l, l + kBlock, 0.25f);
    std::fill(r, r + kBlock, -0.5f);
    env.process(l, r);
    CHECK(l[63] == 0.25f);
    CHECK(r[0] == -0.5f);
    CHECK_FALSE(env.finished());
    env.setThru(false);
    CHECK(env.finished());
}

TEST_CASE("ampenv.bank: the 16-lane SoA bank equals 16 single envelopes; NEON = scalar within 1e-6", "[ampenv]")
{
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> idx(0, 128), frame(0, 63), vel(1, 127), coin(0, 1), what(0, 9);
    auto bank = std::make_unique<engine::modulation::AmpEnvelopeBank>();
    auto ref = std::make_unique<engine::modulation::AmpEnvelopeBank>();
    bank->prepare(kRate);
    ref->prepare(kRate);
    ref->useScalarKernels(true);
    std::vector<AmpEnvelope> single(16);
    for (auto& s : single)
    {
        s.prepare(kRate);
        s.useScalarKernels(true);
    }
    for (int t = 0; t < 16; ++t)
    {
        AmpEnvParams p;
        p.attack = static_cast<std::uint8_t>(std::min(idx(rng), 100));
        p.hold = static_cast<std::uint8_t>(idx(rng));
        p.release = static_cast<std::uint8_t>(std::min(idx(rng), 110));
        p.attackCurve = coin(rng) ? engine::modulation::EnvCurve::Curve : engine::modulation::EnvCurve::Linear;
        p.releaseCurve = coin(rng) ? engine::modulation::EnvCurve::Curve : engine::modulation::EnvCurve::Linear;
        p.retrigger = coin(rng);
        p.sync = t == 5;
        p.bypass = t == 9;
        bank->setParams(t, p, 133.0);
        ref->setParams(t, p, 133.0);
        single[static_cast<std::size_t>(t)].setParams(p, 133.0);
    }
    double worst = 0;
    alignas(16) float a[kBlock], b[kBlock], c[kBlock];
    for (int block = 0; block < 3000; ++block)
        for (int t = 0; t < 16; ++t)
        {
            const int w = what(rng);
            if (w < 2 || (w == 2 && block % 7 == 0))
            {
                const auto kind = w == 1 ? TriggerKind::Half : TriggerKind::Full;
                const auto e = hit(frame(rng), vel(rng), kind, TriggerSource::Pad, t);
                const bool gate = coin(rng);
                bank->trigger(e, gate);
                ref->trigger(e, gate);
                single[static_cast<std::size_t>(t)].trigger(e, gate);
            }
            else if (w == 3)
            {
                const int f = frame(rng);
                bank->release(t, f);
                ref->release(t, f);
                single[static_cast<std::size_t>(t)].release(f);
            }
            else if (w == 4 && block % 11 == 0)
            {
                bank->kill(t, 30);
                ref->kill(t, 30);
                single[static_cast<std::size_t>(t)].kill(30);
            }
            bank->renderGain(t, a);
            ref->renderGain(t, b);
            single[static_cast<std::size_t>(t)].renderGain(c);
            for (int i = 0; i < kBlock; ++i)
            {
                worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
                REQUIRE(b[i] == c[i]); // same code, same kernels: identical
            }
        }
    CHECK(worst <= 1e-6);
}

TEST_CASE("ampenv.rt: processing allocates nothing on the audio thread", "[ampenv]")
{
    auto bank = std::make_unique<engine::modulation::AmpEnvelopeBank>();
    bank->prepare(kRate);
    alignas(16) float l[kBlock], r[kBlock];
    const auto before = core::rt::violationCount();
    {
        core::rt::ScopedRealtimeThread rt;
        for (int block = 0; block < 200; ++block)
            for (int t = 0; t < 16; ++t)
            {
                AmpEnvParams p;
                p.attack = static_cast<std::uint8_t>(block % 100);
                p.sync = t & 1;
                bank->setParams(t, p, 100.0 + block % 3);
                if (block % 10 == t % 10)
                    bank->trigger(hit(t * 3, 100, TriggerKind::Full, TriggerSource::Pad, t), true);
                if (block % 10 == 5)
                    bank->release(t, 9);
                std::fill(l, l + kBlock, 0.5f);
                std::fill(r, r + kBlock, 0.5f);
                bank->process(t, l, r);
            }
    }
    CHECK(core::rt::violationCount() == before);
}
