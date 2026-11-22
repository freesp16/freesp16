// engine::mixer::Mixer (spec audio-engine.md "Track strip", "Buses"; mixer-and-routing.md MIX-001…MIX-060).
// The research-model parity of the strip and bus chain is the golden `mixer` (tests/golden/mixer); these tests pin the
// acceptance criteria directly: laws, ramp lengths, bit transparency, the solo rule, the send tap and return, the
// limiter, the trim, hard mute, locks, meters, silent tracks and the real-time contract.
#include "tests/unit/mixer/MixerTestUtil.h"

#include "core/util/GainLaw.h"
#include "core/util/Realtime.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using engine::mixer::GlobalParam;
using mixertest::kN;
using mixertest::strip;

namespace
{
struct Scale final : engine::mixer::BusInsert
{
    float g = 1.0f;
    explicit Scale(float gain) : g(gain) {}
    void process(float* l, float* r) noexcept override
    {
        for (int i = 0; i < kN; ++i)
        {
            l[i] *= g;
            r[i] *= g;
        }
    }
};
} // namespace

TEST_CASE("mixer.laws: linear balance pan, fader and send gain law", "[mixer]")
{
    struct Case
    {
        int pan;
        float l, r;
    };
    for (const Case c : {Case {-64, 1.0f, 0.0f}, Case {-32, 1.0f, 0.5f}, Case {0, 1.0f, 1.0f}, Case {32, 0.5f, 1.0f},
                         Case {64, 0.0f, 1.0f}, Case {-1, 1.0f, 63.0f / 64.0f}})
    {
        INFO("pan " << c.pan);
        mixertest::Rig rig;
        rig.m().setStrip(0, 0, strip(100, c.pan), 0);
        rig.m().prepare(44100.0, kN); // settled: no start-up ramp
        rig.dc(1u);
        CHECK(rig.b->mainBus.l[0] == c.l);
        CHECK(rig.b->mainBus.r[kN - 1] == c.r);
    }
    for (int v : {0, 1, 9, 10, 50, 100, 127})
    {
        INFO("volume / send " << v);
        mixertest::Rig rig;
        rig.m().setStrip(0, 0, strip(v, 0, v), 0);
        rig.m().prepare(44100.0, kN);
        rig.dc(1u);
        CHECK(rig.b->mainBus.l[5] == core::GainLaw::gain(v));
        CHECK(rig.b->sendBus.l[5] == core::GainLaw::gain(v) * 0.5f); // send: the fader law × 0.5
    }
    CHECK(core::GainLaw::gain(127) * 0.5f == Catch::Approx(1.0f).margin(1e-6)); // send 127 = unity
}

TEST_CASE("mixer.transparency: volume 100, pan 0, no FX, default master is bit-transparent (NFR-030)", "[mixer]")
{
    mixertest::Rig rig;
    mixertest::Noise noise;
    for (int blk = 0; blk < 8; ++blk)
    {
        std::vector<float> in(kN);
        for (auto& x : in)
            x = noise.next(0.99f);
        rig.block(1u, [&](int, int i) { return in[static_cast<std::size_t>(i)]; });
        CHECK(std::memcmp(rig.b->master.l, in.data(), sizeof(float) * kN) == 0);
        CHECK(std::memcmp(rig.b->master.r, in.data(), sizeof(float) * kN) == 0);
    }
    CHECK_FALSE(rig.m().sendReturnActive());
}

TEST_CASE("mixer.ramps: fader, pan and send ramp exactly 100 samples from their frame; mute exactly 128", "[mixer]")
{
    SECTION("fader 100 -> 0 at frame 10 crosses the block boundary and lands at frame 110")
    {
        mixertest::Rig rig;
        std::vector<float> l;
        rig.m().setStrip(0, 0, strip(0), 10);
        for (int blk = 0; blk < 3; ++blk)
        {
            rig.dc(1u);
            l.insert(l.end(), rig.b->mainBus.l, rig.b->mainBus.l + kN);
        }
        for (int i = 0; i <= 10; ++i)
            CHECK(l[static_cast<std::size_t>(i)] == 1.0f); // held until the change's frame (the value before its step)
        CHECK(l[11] < 1.0f);
        CHECK(l[109] > 0.0f);
        CHECK(l[109] < 0.011f);
        CHECK(l[110] == 0.0f); // exactly 100 samples
    }
    SECTION("pan 0 -> 64 and send 0 -> 127 at frame 0")
    {
        mixertest::Rig rig;
        std::vector<float> l, s;
        rig.m().setStrip(0, 0, strip(100, 64, 127), 0);
        for (int blk = 0; blk < 2; ++blk)
        {
            rig.dc(1u);
            l.insert(l.end(), rig.b->mainBus.l, rig.b->mainBus.l + kN);
            s.insert(s.end(), rig.b->sendBus.r, rig.b->sendBus.r + kN);
        }
        CHECK(l[99] > 0.0f);
        CHECK(l[100] == 0.0f);
        CHECK(s[99] < core::GainLaw::gain(127) * 0.5f);
        CHECK(s[100] == core::GainLaw::gain(127) * 0.5f);
    }
    SECTION("mute at frame 30: 128-sample fade from frame 30; unmute turns round mid-fade")
    {
        mixertest::Rig rig;
        std::vector<float> l;
        rig.m().setStrip(0, 0, strip(100, 0, 0, true), 30);
        for (int blk = 0; blk < 4; ++blk)
        {
            rig.dc(1u);
            l.insert(l.end(), rig.b->mainBus.l, rig.b->mainBus.l + kN);
        }
        CHECK(l[29] == 1.0f);
        CHECK(l[30] == 1.0f);
        CHECK(l[31] < 1.0f);
        CHECK(l[30 + 127] > 0.0f);
        CHECK(l[30 + 128] == 0.0f); // exactly 128 samples
        CHECK(l[255] == 0.0f);

        rig.m().setStrip(0, 0, strip(100, 0, 0, true), 0);
        rig.m().setStrip(0, 0, strip(100, 0, 0, false), 0);
        rig.dc(1u);
        CHECK(rig.b->mainBus.l[0] == 0.0f);
        CHECK(rig.b->mainBus.l[63] == Catch::Approx(63.0 / 128.0).margin(1e-5));
        rig.m().setStrip(0, 0, strip(100, 0, 0, true), 0); // turn round: back down from 0.5 over the 64 samples done
        rig.dc(1u);
        CHECK(rig.b->mainBus.l[0] == Catch::Approx(64.0 / 128.0).margin(1e-5));
        CHECK(rig.b->mainBus.l[63] > 0.0f);
        rig.dc(1u);
        CHECK(rig.b->mainBus.l[0] == 0.0f);
    }
}

TEST_CASE("mixer.solo: truth table of the MIX-003 / SCN-012 rule (OQ-11: either level solos, mute wins)", "[mixer]")
{
    // Track 0's own track/pattern mute and solo, and whether another track (1) is soloed at track or pattern level.
    for (int bits = 0; bits < 64; ++bits)
    {
        const bool tMute = bits & 1, pMute = bits & 2, tSolo = bits & 4, pSolo = bits & 8, oTSolo = bits & 16,
                   oPSolo = bits & 32;
        mixertest::Rig rig;
        rig.m().setStrip(0, 0, strip(100, 0, 0, tMute, tSolo), 0);
        rig.m().setStrip(0, 1, strip(100, 0, 0, false, oTSolo), 0);
        rig.m().setPatternMuteSolo(static_cast<std::uint16_t>(pMute ? 1u : 0u),
                                   static_cast<std::uint16_t>((pSolo ? 1u : 0u) | (oPSolo ? 2u : 0u)));
        const bool anySolo = tSolo || pSolo || oTSolo || oPSolo;
        const bool expected = ! tMute && ! pMute && (! anySolo || tSolo || pSolo);
        INFO("mute t/p " << tMute << pMute << " solo t/p " << tSolo << pSolo << " other solo t/p " << oTSolo << oPSolo);
        CHECK(((rig.m().audibleMask() & 1u) != 0) == expected);
        // the audio follows through the mute ramp
        rig.m().prepare(44100.0, kN);
        rig.block(1u, [](int, int) { return 1.0f; });
        CHECK((rig.b->mainBus.l[10] == 1.0f) == expected);
        CHECK((rig.b->mainBus.l[10] == 0.0f) == ! expected);
    }
}

TEST_CASE("mixer.send: pre-fader, post-mute tap; Send FX Off returns silence; the return is added after the master FX",
          "[mixer]")
{
    mixertest::Rig rig;
    rig.m().setStrip(0, 0, strip(0, -32, 127), 0);
    rig.m().prepare(44100.0, kN);
    rig.dc(1u);
    CHECK(rig.b->mainBus.l[3] == 0.0f);                                  // fader 0
    CHECK(rig.b->sendBus.l[3] == core::GainLaw::gain(127) * 0.5f);       // pre-fader
    CHECK(rig.b->sendBus.r[3] == 0.5f * (core::GainLaw::gain(127) * 0.5f)); // post-pan
    CHECK(rig.b->master.l[3] == 0.0f);                                   // Send FX Off: no return
    CHECK_FALSE(rig.m().sendReturnActive());

    Scale sendFx {0.5f}, masterFx {0.0f};
    rig.hooks.sendFx = &sendFx;
    rig.hooks.masterFx = &masterFx; // kills the main bus: what is left on the master is the return alone
    rig.m().setStrip(0, 0, strip(100, 0, 127), 0);
    rig.m().prepare(44100.0, kN);
    rig.dc(1u);
    const float ret = core::GainLaw::gain(127) * 0.5f * 0.5f;
    CHECK(rig.b->master.l[7] == ret);
    CHECK(rig.m().meters().peak[engine::mixer::kMeterSend][0] == ret);

    rig.m().setStrip(0, 0, strip(100, 0, 127, true), 0); // muted: feeds neither bus
    rig.m().prepare(44100.0, kN);
    rig.dc(1u);
    CHECK(rig.b->sendBus.l[7] == 0.0f);
    CHECK(rig.b->master.l[7] == 0.0f);
}

TEST_CASE("mixer.master: master fader, return level, aux after the master fader, output trim", "[mixer]")
{
    mixertest::Rig rig;
    rig.m().setGlobal(GlobalParam::MasterVolume, 50.0f);
    rig.m().setGlobal(GlobalParam::OutputTrim, 512.0f);
    rig.m().prepare(44100.0, kN);
    rig.b->auxActive = true;
    for (int i = 0; i < kN; ++i)
        rig.b->aux.l[i] = rig.b->aux.r[i] = 0.25f;
    rig.dc(1u);
    const float master = core::GainLaw::gain(50) + 0.25f; // aux is not scaled by the master fader
    CHECK(rig.m().meters().peak[engine::mixer::kMeterMaster][0] == master); // metered before the trim
    CHECK(rig.b->master.l[9] == master * engine::mixer::outputTrimGain(512));
    CHECK(rig.m().global(GlobalParam::MasterVolume) == 50);
    rig.m().setGlobal(GlobalParam::SendReturnVolume, 300.0f);
    CHECK(rig.m().global(GlobalParam::SendReturnVolume) == 127); // clamped
}

TEST_CASE("mixer.limiter: transparent at -3.1 dBFS, never above -0.3 dBFS, monotonic, NEON == scalar", "[mixer]")
{
    engine::mixer::SoftLimiter lim;
    const double ceiling = std::pow(10.0, -0.3 / 20.0);
    CHECK(static_cast<double>(lim.ceiling()) <= ceiling);
    CHECK(lim.threshold() == Catch::Approx(std::pow(10.0, -3.0 / 20.0)).epsilon(1e-6));

    // a -3.1 dBFS sine is untouched bit for bit
    alignas(16) float l[kN], r[kN], ref[kN];
    const float a31 = static_cast<float>(std::pow(10.0, -3.1 / 20.0));
    for (int i = 0; i < kN; ++i)
        l[i] = r[i] = ref[i] = a31 * static_cast<float>(std::sin(0.3 * i));
    lim.process(l, r, kN);
    CHECK(std::memcmp(l, ref, sizeof l) == 0);

    // a ramp from 0 to +20 dBFS (±): never above the ceiling, monotonic, the NEON path equals shape()
    float prev = 0.0f;
    double maxOut = 0.0;
    for (int blk = 0; blk < 200; ++blk)
    {
        for (int i = 0; i < kN; ++i)
        {
            const float x = 10.0f * static_cast<float>(blk * kN + i) / (200.0f * kN);
            l[i] = x;
            r[i] = -x;
        }
        std::memcpy(ref, l, sizeof ref);
        lim.process(l, r, kN);
        for (int i = 0; i < kN; ++i)
        {
            CHECK(l[i] == lim.shape(ref[i]));
            CHECK(r[i] == -l[i]);
            CHECK(l[i] >= prev);
            prev = l[i];
            maxOut = std::max(maxOut, static_cast<double>(std::fabs(l[i])));
        }
    }
    CHECK(maxOut <= ceiling);
    CHECK(maxOut > 0.96); // it approaches the ceiling
    CHECK(lim.shape(lim.threshold()) == lim.threshold());
    CHECK(lim.shape(1e9f) <= lim.ceiling());

    // in the mixer: off by default, on with mixer.masterLimiter
    mixertest::Rig rig;
    rig.dc(1u, 2.0f);
    CHECK(rig.b->master.l[0] == 2.0f);
    CHECK((rig.m().meters().clip >> engine::mixer::kMeterMaster & 1u) == 1u);
    rig.m().setGlobal(GlobalParam::MasterLimiter, 1.0f);
    rig.dc(1u, 2.0f);
    CHECK(static_cast<double>(rig.b->master.l[0]) <= ceiling);
    CHECK((rig.m().meters().clip >> engine::mixer::kMeterMaster & 1u) == 0u); // track 1 itself still clips (pre-limiter)
}

TEST_CASE("mixer.trim: MIX-060 output trim 0 = mute, 1 = -60.0 dB, linear in dB to 1023 = 0 dB", "[mixer]")
{
    using engine::mixer::outputTrimGain;
    const auto db = [](int i) { return 20.0 * std::log10(static_cast<double>(outputTrimGain(i))); };
    CHECK(outputTrimGain(0) == 0.0f);
    CHECK(db(1) == Catch::Approx(-60.0).margin(1e-4));
    CHECK(outputTrimGain(1023) == 1.0f);
    CHECK(db(512) == Catch::Approx(-60.0 + 511.0 * 60.0 / 1022.0).margin(1e-4));
    for (int i = 2; i < 1024; ++i)
        CHECK(db(i) - db(i - 1) == Catch::Approx(60.0 / 1022.0).margin(1e-4));
    CHECK(outputTrimGain(5000) == 1.0f);
    CHECK(outputTrimGain(-3) == 0.0f);
}

TEST_CASE("mixer.hardMute: cuts at once (no ramp, tails included), fades back in from the next block", "[mixer]")
{
    mixertest::Rig rig;
    rig.dc(1u);
    rig.m().hardMute(0);
    rig.dc(1u);
    CHECK(rig.b->mainBus.l[0] == 0.0f);
    CHECK(rig.b->mainBus.l[63] == 0.0f);
    CHECK(rig.m().meters().peak[0][0] == 0.0f);
    rig.dc(1u);
    CHECK(rig.b->mainBus.l[0] == 0.0f);
    CHECK(rig.b->mainBus.l[63] == Catch::Approx(63.0 / 128.0).margin(1e-5));
    rig.dc(1u);
    rig.dc(1u);
    CHECK(rig.b->mainBus.l[0] == 1.0f);
    // a muted track stays closed after a hard mute
    rig.m().setStrip(0, 0, strip(100, 0, 0, true), 0);
    rig.m().hardMute(0);
    for (int i = 0; i < 4; ++i)
        rig.dc(1u);
    CHECK(rig.b->mainBus.l[30] == 0.0f);
}

TEST_CASE("mixer.locks: volume/pan/send/mute locks apply from their frame, revert, or hold", "[mixer]")
{
    mixertest::Rig rig;
    float values[35] {};
    std::uint8_t change[35] {};
    mixertest::FakeLockSet locks;
    locks.values = values;
    locks.change = change;
    locks.mask = 1u << engine::mixer::kLockVolume | 1u << engine::mixer::kLockPan;
    values[engine::mixer::kLockVolume] = 0.0f;
    values[engine::mixer::kLockPan] = 64.0f;
    change[engine::mixer::kLockPan] = 1; // Hold
    rig.m().applyLocks(0, &locks, 20);
    for (int i = 0; i < 3; ++i)
        rig.dc(1u);
    CHECK(rig.b->mainBus.l[0] == 0.0f);
    CHECK(rig.m().strip(0, 0).volume == 100); // the strip's own value is unchanged
    // next trigger without locks: volume reverts, the held pan stays
    rig.m().applyLocks(0, static_cast<const mixertest::FakeLockSet*>(nullptr), 0);
    for (int i = 0; i < 3; ++i)
        rig.dc(1u);
    CHECK(rig.b->mainBus.r[0] == 1.0f);
    CHECK(rig.b->mainBus.l[0] == 0.0f); // pan 64: L silent
    // a mute lock goes through the mute ramp; send lock
    locks.mask = 1u << engine::mixer::kLockMute | 1u << engine::mixer::kLockSend | 1u << engine::mixer::kLockPan;
    values[engine::mixer::kLockMute] = 1.0f;
    values[engine::mixer::kLockSend] = 127.0f;
    values[engine::mixer::kLockPan] = 0.0f;
    change[engine::mixer::kLockPan] = 0;
    rig.m().applyLocks(0, &locks, 0);
    CHECK((rig.m().audibleMask() & 1u) == 0u);
    for (int i = 0; i < 3; ++i)
        rig.dc(1u);
    CHECK(rig.b->mainBus.l[0] == 0.0f);
    CHECK(rig.b->sendBus.l[0] == 0.0f); // muted: no send either
    rig.m().applyLocks(0, static_cast<const mixertest::FakeLockSet*>(nullptr), 0);
    CHECK((rig.m().audibleMask() & 1u) == 1u);
}

TEST_CASE("mixer.meters: post-fader track peaks and sums, send return, master and clip bits", "[mixer]")
{
    mixertest::Rig rig;
    rig.m().setStrip(0, 3, strip(50, 64), 0);
    rig.m().prepare(44100.0, kN);
    rig.block(1u << 3 | 1u, [](int t, int i) { return t == 0 ? (i == 7 ? -1.5f : 0.1f) : 0.5f; });
    const auto& m = rig.m().meters();
    CHECK(m.peak[0][0] == 1.5f);
    CHECK(m.sumSq[0][0] == Catch::Approx(1.5 * 1.5 + 63 * 0.01).epsilon(1e-5));
    CHECK(m.peak[3][0] == 0.0f); // pan 64: L silent
    CHECK(m.peak[3][1] == 0.5f * core::GainLaw::gain(50));
    CHECK(m.peak[5][0] == 0.0f); // silent track
    CHECK(m.peak[engine::mixer::kMeterMaster][0] == 1.5f);
    CHECK(m.clip == (1u | 1u << engine::mixer::kMeterMaster));
}

TEST_CASE("mixer.silent: a silent track skips the strip but keeps its ramps exact", "[mixer]")
{
    mixertest::Rig a, b;
    for (auto* r : {&a, &b})
        r->m().setStrip(0, 0, strip(30, -20, 90), 17);
    a.dc(1u);
    b.dc(0u); // track 0 silent for one block: the ramps still advance
    CHECK(b.m().meters().peak[0][0] == 0.0f);
    a.dc(1u);
    b.dc(1u);
    CHECK(std::memcmp(a.b->mainBus.l, b.b->mainBus.l, sizeof(float) * kN) == 0);
    CHECK(std::memcmp(a.b->sendBus.r, b.b->sendBus.r, sizeof(float) * kN) == 0);
}

TEST_CASE("mixer.realtime: 16 strips with ramps, hooks, limiter and meters never allocate", "[mixer][realtime]")
{
    mixertest::Rig rig;
    Scale fx {0.7f};
    rig.hooks.sendFx = &fx;
    rig.hooks.masterFx = &fx;
    rig.m().setGlobal(GlobalParam::MasterLimiter, 1.0f);
    mixertest::Noise noise;
    {
        core::rt::ScopedRealtimeThread rt;
        for (int blk = 0; blk < 64; ++blk)
        {
            for (int t = 0; t < engine::kTracks; ++t)
                rig.m().setStrip(0, t, strip((blk * 7 + t) % 128, (blk % 3) * 30 - 30, blk % 2 ? 127 : 0, blk % 4 == 0),
                                 blk % 64);
            rig.m().hardMute(blk % 16);
            rig.block(0xFFFFu, [&](int, int) { return noise.next(); });
        }
    }
#if APP_RT_CHECKS
    CHECK(core::rt::violationCount() == 0);
#endif
    for (int i = 0; i < kN; ++i)
        CHECK(std::isfinite(rig.b->master.l[i]));
}
