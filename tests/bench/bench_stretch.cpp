// Time-stretch bench (budget: ≤ 16k A9 cycles per 64-frame block
// per stereo MASTER TEMPO voice, time ratio 0.5–2, pitch ±12 st). One iteration = one 64-frame stereo block through a
// TimeStretcher the way the voice drives it (framesNeeded → input copy → process). Spectral/grain backends work in
// hops, so the per-block cost is bursty: the BENCH median is the mean over a batch of blocks; the worst block, the
// 16-voice worst block (grains staggered by the bank) and the priming burst are recorded separately.
//
//   bench_stretch_<backend>_t<tier>      ratio 0.5, +7 st (the dearest case: most core frames per output frame) ≤ 16000
//   bench_stretch_kernel_dot512[_scalar] one WSOLA correlation (512 MACs), the dispatched kernel vs the scalar
//                                        reference (NEON on the device; equal on x86-64)                      ≤ 600
//   bench_stretch_kernel_fft1024         one 1024-point complex FFT (the phase vocoder does two per grain)      ≤ 60000
//   bench_stretch_table (host, Catch)    the decision table: mean / worst cycles per block per voice for ratios
//                                        0.5 / 0.8 / 1.25 / 2 × pitch 0 / +7 st × both tiers × every backend, the
//                                        16-voice block (p99 ≤ 16 × 16k; worst printed) and the priming burst
// Desktop numbers are relative only; the device numbers come from scripts/emulator-bench.sh bench_stretch and a
// hardware run.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/voice/stretch/GrainStretcher.h"
#include "engine/voice/stretch/StretchFft.h"
#include "engine/voice/stretch/StretchKernels.h"
#include "engine/voice/stretch/StretcherBank.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
using engine::voice::TimeStretcher;
using engine::voice::StretcherRegistry;
using engine::voice::stretch::GrainStretcher;
constexpr int N = engine::kBlockFrames;
constexpr double kRate = 44100.0;
constexpr double kBudget = 16000.0;

// A looping stereo test signal (two detuned partials per channel plus a noise burst every beat, so transient and
// tonal paths both run) and the voice-side driver of one stretcher.
struct Feed
{
    static constexpr int kLen = 1 << 17;
    std::vector<float> l, r, inL, inR, outL, outR;
    std::size_t pos = 0;
    Feed() : l(kLen), r(kLen), inL(1 << 15), inR(1 << 15), outL(N), outR(N)
    {
        unsigned seed = 12345u;
        for (int i = 0; i < kLen; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const float noise = (i % 22050) < 400 ? (static_cast<float>(seed >> 9) / 4194304.0f - 1.0f) * 0.5f : 0.0f;
            const double t = i / kRate;
            l[static_cast<std::size_t>(i)] = static_cast<float>(0.3 * std::sin(2 * M_PI * 220.0 * t) + 0.2 * std::sin(2 * M_PI * 331.0 * t)) + noise;
            r[static_cast<std::size_t>(i)] = static_cast<float>(0.3 * std::sin(2 * M_PI * 221.5 * t) + 0.2 * std::sin(2 * M_PI * 440.0 * t)) + noise;
        }
    }
    void block(TimeStretcher& s)
    {
        const int need = std::min<int>(s.framesNeeded(N), static_cast<int>(inL.size()));
        for (int i = 0; i < need; ++i, pos = (pos + 1) & (kLen - 1))
        {
            inL[static_cast<std::size_t>(i)] = l[pos];
            inR[static_cast<std::size_t>(i)] = r[pos];
        }
        const float* in[2] = {inL.data(), inR.data()};
        float* out[2] = {outL.data(), outR.data()};
        s.process(in, need, out, N);
        bench::doNotOptimize(outL[5]);
    }
};

std::unique_ptr<TimeStretcher> make(const char* backend, int tier, double ratio, double pitch)
{
    auto s = StretcherRegistry::builtin().find(backend)();
    s->prepare(kRate, N, 2);
    if (auto* g = dynamic_cast<GrainStretcher*>(s.get()))
        g->setQuality(tier);
    s->setRatios(ratio, pitch);
    s->reset();
    return s;
}

void benchVoice(bench::State& state, const char* backend, int tier)
{
    auto s = make(backend, tier, 0.5, std::exp2(7.0 / 12.0));
    Feed f;
    for (int i = 0; i < 64; ++i) // past the priming burst
        f.block(*s);
    state.run([&] { f.block(*s); });
}
} // namespace

BENCH(bench_stretch_pvoc_t0, 16000) { benchVoice(state, "pvoc", 0); }
BENCH(bench_stretch_pvoc_t1, 16000) { benchVoice(state, "pvoc", 1); }
BENCH(bench_stretch_wsola_t0, 16000) { benchVoice(state, "wsola", 0); }
BENCH(bench_stretch_wsola_t1, 16000) { benchVoice(state, "wsola", 1); }

BENCH(bench_stretch_kernel_dot512, 600)
{
    std::vector<float> a(512), b(512);
    for (int i = 0; i < 512; ++i)
    {
        a[static_cast<std::size_t>(i)] = std::sin(0.01f * static_cast<float>(i));
        b[static_cast<std::size_t>(i)] = std::cos(0.013f * static_cast<float>(i));
    }
    state.run([&] { bench::doNotOptimize(engine::voice::stretch::kernels::dot(a.data(), b.data(), 512)); });
}

BENCH(bench_stretch_kernel_dot512_scalar, 600)
{
    std::vector<float> a(512), b(512);
    for (int i = 0; i < 512; ++i)
    {
        a[static_cast<std::size_t>(i)] = std::sin(0.01f * static_cast<float>(i));
        b[static_cast<std::size_t>(i)] = std::cos(0.013f * static_cast<float>(i));
    }
    state.run([&] { bench::doNotOptimize(engine::voice::stretch::kernels::scalar::dot(a.data(), b.data(), 512)); });
}

BENCH(bench_stretch_kernel_fft1024, 60000)
{
    engine::voice::stretch::StretchFft fft;
    fft.prepare(1024);
    std::vector<float> re(1024), im(1024);
    for (int i = 0; i < 1024; ++i)
        re[static_cast<std::size_t>(i)] = std::sin(0.05f * static_cast<float>(i));
    state.run(
        [&]
        {
            fft.forward(re.data(), im.data());
            fft.inverse(re.data(), im.data());
            for (int i = 0; i < 4; ++i) // keep the values bounded (inverse is unscaled)
                re[static_cast<std::size_t>(i)] *= 1.0f / 1024.0f;
            bench::doNotOptimize(re[7]);
        });
}

#if ! defined(BENCH_STANDALONE)
namespace
{
struct Stats
{
    double mean = 0, worst = 0, p99 = 0;
};

Stats timeBlocks(TimeStretcher& s, Feed& f, int blocks)
{
    std::vector<double> t(static_cast<std::size_t>(blocks));
    for (int b = 0; b < blocks; ++b)
    {
        const std::uint64_t t0 = bench::nowNs();
        f.block(s);
        t[static_cast<std::size_t>(b)] = static_cast<double>(bench::nowNs() - t0);
    }
    Stats st;
    for (double v : t)
        st.mean += v;
    st.mean /= blocks;
    std::sort(t.begin(), t.end());
    st.worst = t.back();
    st.p99 = t[static_cast<std::size_t>(blocks * 99 / 100)];
    return st;
}
} // namespace

TEST_CASE("bench_stretch_table", "[bench]")
{
    std::printf("STRETCH TABLE (cycles per 64-frame block per stereo voice; desktop ns = relative)\n");
    std::printf("%-6s %-4s %-6s %-6s %10s %10s %10s\n", "backend", "tier", "ratio", "pitch", "mean", "p99", "worst");
    for (const char* backend : {"pvoc", "wsola", "none"})
        for (int tier = 0; tier < 2; ++tier)
            for (const double ratio : {0.5, 0.8, 1.25, 2.0})
                for (const int st : {0, 7})
                {
                    if (std::string(backend) == "none" && tier == 1)
                        continue;
                    auto s = make(backend, tier, ratio, std::exp2(st / 12.0));
                    Feed f;
                    for (int i = 0; i < 64; ++i)
                        f.block(*s);
                    const Stats r = timeBlocks(*s, f, 4000);
                    std::printf("%-7s %-4d %-6.2f %+-6d %10.0f %10.0f %10.0f\n", backend, tier, ratio, st, r.mean, r.p99,
                                r.worst);
                    char name[96];
                    std::snprintf(name, sizeof name, "stretch_%s_t%d_r%03d_p%d_mean", backend, tier,
                                  static_cast<int>(ratio * 100 + 0.5), st);
                    bench::record(name, kBudget, r.mean, r.p99);
                    CHECK(r.mean <= kBudget);
                }
    // 16 MASTER TEMPO voices started in the same block (the bank staggers their grains), ratio 0.8, +7 st.
    for (const char* backend : {"pvoc", "wsola"})
    {
        engine::voice::stretch::StretcherBank bank;
        bank.select(backend);
        bank.prepare(kRate, N);
        std::vector<Feed> feeds(16);
        for (int t = 0; t < 16; ++t)
        {
            bank.lane(t)->setRatios(0.8, std::exp2(7.0 / 12.0));
            bank.lane(t)->reset();
        }
        double firstBlock = 0;
        const int blocks = 4000;
        std::vector<double> t16;
        for (int b = 0; b < blocks; ++b)
        {
            const std::uint64_t t0 = bench::nowNs();
            for (int t = 0; t < 16; ++t)
                feeds[static_cast<std::size_t>(t)].block(*bank.lane(t));
            const double dt = static_cast<double>(bench::nowNs() - t0);
            if (b == 0)
                firstBlock = dt; // 16 primes at once (the engine's bank allows two per block)
            else if (b >= 64)
                t16.push_back(dt);
        }
        double sum = 0;
        for (double v : t16)
            sum += v;
        std::sort(t16.begin(), t16.end());
        const double p99 = t16[t16.size() * 99 / 100], worst = t16.back();
        std::printf("16 voices %-6s: mean %.0f p99 %.0f worst %.0f per block; first block (16 primes) %.0f; one prime %.0f\n",
                    backend, sum / static_cast<double>(t16.size()), p99, worst, firstBlock, firstBlock / 16);
        char name[96];
        // Gate: p99 (the worst single block on a shared desktop is scheduler noise; it is printed and recorded as p95).
        std::snprintf(name, sizeof name, "stretch_%s_16voices_p99", backend);
        bench::record(name, 16 * kBudget, p99, worst);
        std::snprintf(name, sizeof name, "stretch_%s_prime", backend);
        bench::record(name, 4 * kBudget, firstBlock / 16);
        CHECK(p99 <= 16 * kBudget);
    }
}
#endif
