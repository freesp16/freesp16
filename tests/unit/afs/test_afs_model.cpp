// The digital model (AFS-004, AFS-010): the ladder's −24 dB/oct slope (−12 with the
// 2-pole tap), the resonance peak, self-oscillation within 2 % of the cutoff, aliasing ≤ −60 dB for a 5 kHz sine at
// full drive, no NaN / denormal build-up at resonance 100 %, the high-pass, the envelope and LFO modulation, and the
// golden comparison against an independent double-precision implementation (AfsReference.h).
#include "tests/unit/afs/AfsReference.h"
#include "tests/unit/afs/AfsTestUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace afstest;
using engine::afs::ModelProcessor;

namespace
{
int positionFor(double hz) { return static_cast<int>(std::lround(1023.0 * std::log(hz / 20.0) / std::log(1000.0))); }

// Gain (dB) of the slot's model at `hz` for a −20 dBFS sine, from the DFT bin of the settled output.
double gainDb(int lp, int res, double hz, bool twoPole = false, int hp = 0, double fs = 44100.0)
{
    SlotRig rig(false, fs);
    rig.set(AfsField::Active, 1);
    rig.set(AfsField::LpCutoff, static_cast<float>(lp));
    rig.set(AfsField::Resonance, static_cast<float>(res));
    rig.set(AfsField::HpCutoff, static_cast<float>(hp));
    rig.set(AfsField::Slope, twoPole ? 1.0f : 0.0f);
    rig.settle();
    const int frames = 32768;
    auto l = sine(frames, hz, 0.1, fs), r = l;
    runMaster(rig, l, r);
    double re = 0, im = 0, ire = 0, iim = 0;
    const auto in = sine(frames, hz, 0.1, fs);
    for (int i = 8192; i < frames; ++i)
    {
        const double w = 2.0 * kPi * hz * i / fs;
        re += static_cast<double>(l[static_cast<std::size_t>(i)]) * std::cos(w);
        im += static_cast<double>(l[static_cast<std::size_t>(i)]) * std::sin(w);
        ire += static_cast<double>(in[static_cast<std::size_t>(i)]) * std::cos(w);
        iim += static_cast<double>(in[static_cast<std::size_t>(i)]) * std::sin(w);
    }
    return 10.0 * std::log10((re * re + im * im) / (ire * ire + iim * iim));
}
} // namespace

TEST_CASE("afs.model: laws — cutoff 20 Hz … 20 kHz exponential, HP 20 Hz … 2 kHz, feedback, latency", "[afs][model]")
{
    CHECK(std::fabs(ModelProcessor::cutoffHz(0) - 20.0) < 1e-9);
    CHECK(std::fabs(ModelProcessor::cutoffHz(1023) - 20000.0) < 1e-6);
    CHECK(ModelProcessor::hpCutoffHz(0) == 0.0);
    CHECK(std::fabs(ModelProcessor::hpCutoffHz(1) - 20.0) < 1e-9);
    CHECK(std::fabs(ModelProcessor::hpCutoffHz(1023) - 2000.0) < 1e-6);
    CHECK(ModelProcessor::feedback(0.0f) == 0.0f);
    CHECK(std::fabs(ModelProcessor::feedback(0.95f) - 4.0f) < 1e-6f);
    CHECK(std::fabs(ModelProcessor::feedback(1.0f) - 4.6f) < 1e-5f);
    ModelProcessor m;
    m.prepare(44100.0, N);
    CHECK(m.latencySamples() == 31);
}

TEST_CASE("afs.model: the passband is flat; the 4-pole slope is −24 dB/oct, the 2-pole tap −12 dB/oct", "[afs][model]")
{
    const int lp = positionFor(500.0);
    // Fully open = four coincident poles at 20 kHz (−3 dB near 8.7 kHz), the ladder's own droop (our choice).
    CHECK(std::fabs(gainDb(1023, 0, 1000.0)) < 0.1);
    CHECK(gainDb(1023, 0, 8000.0) > -3.0);
    const double g2 = gainDb(lp, 0, 2000.0), g4 = gainDb(lp, 0, 4000.0);
    INFO("4-pole 2k " << g2 << " dB, 4k " << g4 << " dB");
    CHECK(g4 - g2 < -22.0);
    CHECK(g4 - g2 > -25.0);
    CHECK(std::fabs(gainDb(lp, 0, 50.0)) < 0.2); // DC region at unity (no resonance)
    const double t2 = gainDb(lp, 0, 2000.0, true), t4 = gainDb(lp, 0, 4000.0, true);
    INFO("2-pole 2k " << t2 << " dB, 4k " << t4 << " dB");
    CHECK(t4 - t2 < -10.5);
    CHECK(t4 - t2 > -13.0);
}

TEST_CASE("afs.model: resonance raises a peak at the cutoff; the bass drops by 1/(1 + k)", "[afs][model]")
{
    const int lp = positionFor(1000.0);
    const int res = 716; // 70 % → k ≈ 2.95
    double best = -1e9, bestHz = 0;
    for (double hz = 700; hz <= 1300; hz += 25)
    {
        const double g = gainDb(lp, res, hz);
        if (g > best)
        {
            best = g;
            bestHz = hz;
        }
    }
    const double dc = gainDb(lp, res, 40.0);
    const double k = ModelProcessor::feedback(res / 1023.0f);
    INFO("peak " << best << " dB at " << bestHz << " Hz, low " << dc << " dB");
    CHECK(std::fabs(dc + 20.0 * std::log10(1.0 + k)) < 0.3);
    CHECK(best - dc > 10.0);
    CHECK(bestHz > 850);
    CHECK(bestHz < 1100);
}

TEST_CASE("afs.model: resonance 100 % self-oscillates within 2 % of the cutoff", "[afs][model]")
{
    for (double hz : {200.0, 1000.0, 5000.0, 12000.0})
    {
        const int lp = positionFor(hz);
        const double fc = ModelProcessor::cutoffHz(lp);
        SlotRig rig;
        rig.set(AfsField::Active, 1);
        rig.set(AfsField::LpCutoff, static_cast<float>(lp));
        rig.set(AfsField::Resonance, 1023);
        rig.settle();
        Vec l(3 * 44100 / N * N, 0.0f), r = l; // silence in: the seed starts it
        runMaster(rig, l, r);
        const double f = zeroCrossingHz(l, l.size() / 2);
        const double amp = rms(l, l.size() / 2, l.size()) * std::sqrt(2.0);
        INFO("cutoff " << fc << " Hz: oscillates at " << f << " Hz, amplitude " << amp);
        CHECK(std::fabs(f / fc - 1.0) < 0.02);
        CHECK(amp > 0.1);
        CHECK(amp < static_cast<double>(engine::fx::dsp::afs::kSatMax));
    }
}

TEST_CASE("afs.model: a 5 kHz sine at full drive aliases ≤ −60 dB (2× oversampling)", "[afs][model][alias]")
{
    for (double amp : {1.0, 0.5})
    {
        SlotRig rig;
        rig.set(AfsField::Active, 1);
        rig.set(AfsField::Drive, 1023);
        rig.set(AfsField::LpCutoff, 1023);
        rig.settle();
        constexpr int M = 8192;
        const double f0 = std::round(5000.0 / 44100.0 * M) * 44100.0 / M; // on a bin
        auto l = sine(4 * M, f0, amp), r = l;
        runMaster(rig, l, r);
        const Vec seg(l.begin() + 2 * M, l.begin() + 3 * M);
        const auto spec = spectrum(seg);
        double fund = 0, worst = 0, worstHz = 0;
        for (std::size_t k = 0; k < spec.size(); ++k)
        {
            const double hz = static_cast<double>(k) * 44100.0 / M;
            const double h = hz / f0;
            const bool harmonic = std::fabs(h - std::round(h)) * f0 < 6.0 * 44100.0 / M; // ±6 bins of a harmonic
            if (std::fabs(hz - f0) < 1.0)
                fund = spec[k];
            else if (! harmonic && hz > 40.0 && spec[k] > worst)
            {
                worst = spec[k];
                worstHz = hz;
            }
        }
        const double db = 20.0 * std::log10(worst / fund);
        INFO("input " << 20 * std::log10(amp) << " dBFS ×4: worst non-harmonic " << db << " dB at " << worstHz << " Hz");
        CHECK(db <= -60.0);
    }
}

TEST_CASE("afs.model: resonance 100 % and full drive: no NaN, no denormal, bounded (44.1 and 48 kHz)", "[afs][model]")
{
    for (double fs : {44100.0, 48000.0})
        for (int lp : {0, 300, 1023})
        {
            SlotRig rig(false, fs);
            rig.set(AfsField::Active, 1);
            rig.set(AfsField::Drive, 1023);
            rig.set(AfsField::LpCutoff, static_cast<float>(lp));
            rig.set(AfsField::Resonance, 1023);
            rig.set(AfsField::HpCutoff, 500);
            rig.settle();
            auto l = noise(4 * 44100 / N * N, 11, 1.0f), r = noise(4 * 44100 / N * N, 12, 1.0f);
            for (std::size_t i = l.size() / 2; i < l.size(); ++i)
                l[i] = r[i] = 0.0f; // then silence: the tail must not decay into denormals
            runMaster(rig, l, r);
            bool finite = true, denormal = false;
            float peak = 0;
            for (std::size_t i = 0; i < l.size(); ++i)
                for (float v : {l[i], r[i]})
                {
                    finite = finite && std::isfinite(v);
                    denormal = denormal || std::fpclassify(v) == FP_SUBNORMAL;
                    peak = std::max(peak, std::fabs(v));
                }
            INFO("fs " << fs << " lp " << lp << " peak " << peak);
            CHECK(finite);
            CHECK_FALSE(denormal);
            CHECK(peak < 2.0f);
        }
}

TEST_CASE("afs.model: the high-pass after the ladder — 0 = off, −12 dB/oct below its cutoff", "[afs][model]")
{
    const int hp = 1 + static_cast<int>(std::lround(1022.0 * std::log(500.0 / 20.0) / std::log(100.0))); // ≈ 500 Hz
    const double hz = ModelProcessor::hpCutoffHz(hp);
    CHECK(std::fabs(hz / 500.0 - 1.0) < 0.01);
    const auto rel = [&](double f) { return gainDb(1023, 0, f, false, hp) - gainDb(1023, 0, f, false, 0); };
    CHECK(std::fabs(rel(4000.0)) < 0.05);
    CHECK(std::fabs(rel(hz) + 3.01) < 0.1); // Butterworth −3 dB at the cutoff
    const double a = rel(100.0), b = rel(50.0);
    CHECK(b - a < -11.0);
    CHECK(b - a > -13.0);
    CHECK(std::fabs(gainDb(1023, 0, 50.0, false, 0)) < 0.1); // off: the ladder's low end is flat
}

TEST_CASE("afs.model: the envelope (assigned track's triggers) and the track LFO move the cutoff in octaves",
          "[afs][model][mod]")
{
    SlotRig rig;
    rig.set(AfsField::Active, 1);
    rig.set(AfsField::InsertTrack, 0);
    rig.set(AfsField::LpCutoff, 200);
    rig.set(AfsField::EnvAmount, 100);
    rig.set(AfsField::EnvDecay, 64); // ≈ 205 ms to −60 dB
    rig.settle();
    std::array<float, N> l {}, r {};
    rig.master(l.data(), r.data());
    CHECK(std::fabs(rig.slot.model().smoothedPosition() - 200.0f) < 1e-3f);
    engine::trigger::TriggerEvent ev {3, 100, 0, engine::trigger::TriggerKind::Full, engine::trigger::TriggerSource::Pad,
                                      0, false, nullptr};
    rig.master(l.data(), r.data(), nullptr, nullptr, std::span(&ev, 1)); // Master route: any track's trigger
    rig.master(l.data(), r.data());
    const float peak = rig.slot.model().smoothedPosition();
    INFO("position after the attack: " << peak);
    CHECK(peak > 200.0f + 4.0f * 102.65f); // most of +6 octaves after 2 ms attack and 1 ms smoothing
    for (int b = 0; b < 100; ++b) // ≈ 145 ms: about −40 dB
        rig.master(l.data(), r.data());
    const float mid = rig.slot.model().smoothedPosition();
    CHECK(mid < 200.0f + 0.03f * 6.0f * 102.65f);
    CHECK(mid > 200.0f);
    for (int b = 0; b < 300; ++b)
        rig.master(l.data(), r.data());
    CHECK(std::fabs(rig.slot.model().smoothedPosition() - 200.0f) < 1e-3f); // decayed to 0 (fast path snapped)

    // The LFO on a track route: value 1 × 100 % = +4 octaves.
    SlotRig t;
    t.set(AfsField::Active, 1);
    t.set(AfsField::InsertTrack, 4);
    t.set(AfsField::LpCutoff, 300);
    t.set(AfsField::LfoAmount, 100);
    t.settle();
    for (int b = 0; b < 20; ++b)
    {
        std::array<float, N> bl {}, br {};
        t.slot.beginPeriod(t.io());
        t.slot.beginBlock({}, 1.0f);
        t.slot.processAt(Route::TrackPreFx, 3, bl.data(), br.data(), N);
        t.slot.endBlock();
    }
    CHECK(std::fabs(t.slot.model().smoothedPosition() - (300.0f + 4.0f * 102.65133f)) < 0.5f);
    CHECK(t.slot.control().lfo == 4.0f);
}

TEST_CASE("afs.model: golden — within 1e-5 of peak (or −100 dBFS) of the double-precision reference", "[afs][model][golden]")
{
    struct Case
    {
        int lp, res, hp;
        bool twoPole;
        float amp;
    };
    const Case cases[] = {{1023, 0, 0, false, 0.5f}, {500, 300, 0, false, 0.5f}, {300, 700, 200, false, 0.3f},
                          {700, 500, 0, true, 0.8f}, {200, 0, 600, false, 1.5f}, {900, 900, 0, false, 0.2f}};
    for (double fs : {44100.0, 48000.0})
        for (const auto& c : cases)
        {
            engine::afs::ModelProcessor m;
            m.prepare(fs, N);
            m.useScalarKernels(true);
            ModelReference ref(fs);
            engine::afs::AfsControl ctl;
            ctl.lpCutoff01 = c.lp / 1023.0f;
            ctl.resonance01 = c.res / 1023.0f;
            ctl.hpCutoff01 = c.hp / 1023.0f;
            ctl.twoPole = c.twoPole;
            auto inL = noise(64 * N, 31, c.amp), inR = sine(64 * N, 330.0, c.amp, fs);
            auto a = inL, b = inR, x = inL, y = inR;
            double peak = 0, err = 0;
            for (int blk = 0; blk < 64; ++blk)
            {
                const auto o = static_cast<std::size_t>(blk * N);
                m.process(a.data() + o, b.data() + o, N, ctl);
                ref.process(x.data() + o, y.data() + o, c.lp, c.res, c.hp, c.twoPole);
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                peak = std::max({peak, std::fabs(static_cast<double>(x[i])), std::fabs(static_cast<double>(y[i]))});
                err = std::max({err, std::fabs(static_cast<double>(a[i]) - static_cast<double>(x[i])), std::fabs(static_cast<double>(b[i]) - static_cast<double>(y[i]))});
            }
            INFO("fs " << fs << " lp " << c.lp << " res " << c.res << " hp " << c.hp << " 2p " << c.twoPole
                       << ": max error " << err << " of peak " << peak << " (" << 20 * std::log10(err / peak + 1e-30)
                       << " dB)");
            // The default golden tolerance: 1e-5 relative to peak, or a residual ≤ −100 dBFS (a quiet output).
            CHECK((err <= 1e-5 * peak || err <= 1e-5));
        }
}
