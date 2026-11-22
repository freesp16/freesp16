// Sampling benchmarks (budget: audio thread ≤ 2.5 k A9 cycles per block — limiter 300,
// ring write 120, meter 60, threshold 60, sampler bookkeeping; Recorder thread ≤ 3 % of a core; take copy for 8 bars
// ≤ 10 ms on a worker). Budgets in A9 cycles at 1 GHz; the host is far faster, so these numbers rank and
// guard regressions; device numbers come from scripts/emulator-bench.sh bench_sampling and the hardware.
//
//   bench_sampling_common     limiter OFF, a quiet input, idle sampler: input meter + ring write + status (every
//                             block while the hub is installed).
//   bench_sampling            worst case per block: limiter ON on a loud input (copy + shaper), a running take (REC
//                             LENGTH OFF), live sampling armed, the input meter and status — input() + schedule() +
//                             capture().
//   bench_sampling_threshold  standby with REC THRESHOLD −20 dB on a quiet block (the full first-crossing scan).
//   bench_sampling_recorder   the Recorder's per-chunk work: 4096 stereo frames → S24 → file (64 KB chunks); budget =
//                             3 % of a core for 4096 frames at 44.1 kHz (2.79 ms).
//   bench_sampling_take       the take builder: 8 bars at 120 BPM + two 1-beat margins (750,000 frames, stereo) out of
//                             the ring into a new SampleBuffer, with its content hash; budget 10 ms.
#include "core/util/sampling/CaptureHub.h"
#include "engine/sampling/SamplingStage.h"
#include "engine/sampling/Threshold.h"
#include "services/sampling/TakeBuilder.h"
#include "services/sampling/WavWriter.h"
#include "tests/bench/harness/Bench.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

namespace
{
constexpr int kN = engine::kBlockFrames;
using engine::sampling::SamplingParam;

struct Rig
{
    std::unique_ptr<core::sampling::CaptureHub> hub = std::make_unique<core::sampling::CaptureHub>();
    std::unique_ptr<engine::sampling::SamplingStage> stage = std::make_unique<engine::sampling::SamplingStage>();
    std::unique_ptr<engine::stages::BlockBuffers> b = std::make_unique<engine::stages::BlockBuffers>();
    std::unique_ptr<engine::scene::ScenePatternSwitcher> switcher = std::make_unique<engine::scene::ScenePatternSwitcher>();
    engine::BlockContext ctx {};
    alignas(16) float inL[kN] {}, inR[kN] {}, tap[kN] {};

    explicit Rig(float amp)
    {
        hub->ring.allocate(30 * 44100);
        stage->prepare(44100.0);
        stage->install(hub.get());
        ctx.sampleRate = 44100.0;
        ctx.frames = kN;
        ctx.bpm = 120.0;
        std::uint32_t s = 7;
        for (int i = 0; i < kN; ++i)
        {
            s = s * 1664525u + 1013904223u;
            inL[i] = amp * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
            inR[i] = -inL[i];
            tap[i] = 0.5f * inL[i];
        }
    }
    void block()
    {
        b->inputL = inL;
        b->inputR = inR;
        stage->input(*b);
        stage->schedule(ctx, b->timing, *switcher, nullptr);
        stage->capture(ctx, *b, tap, tap, 0);
    }
};
} // namespace

BENCH(bench_sampling_common, 400)
{
    Rig rig(0.05f);
    state.run([&] {
        rig.block();
        bench::doNotOptimize(rig.hub->ring.head());
    });
}

BENCH(bench_sampling, 2500)
{
    Rig rig(1.2f); // above −3 dBFS everywhere the limiter has work (and clipped)
    auto& st = *rig.stage;
    st.setLimiter(true);
    st.set(SamplingParam::Rec, 3, 1.0f); // threshold OFF: records at once, REC LENGTH OFF (open take)
    st.set(SamplingParam::LiveMode, 0, 1.0f);
    st.set(SamplingParam::LiveRec, 4, 1.0f); // armed (stopped: no boundary)
    state.run([&] {
        rig.block();
        bench::doNotOptimize(rig.b->inputL);
    });
}

BENCH(bench_sampling_threshold, 300)
{
    Rig rig(0.05f); // −26 dBFS peaks: below −20 dB, the scan runs every block
    rig.stage->set(SamplingParam::Threshold, 0, 41.0f);
    rig.stage->set(SamplingParam::Rec, 0, 1.0f);
    state.run([&] {
        const int f = engine::sampling::firstAbove(rig.inL, rig.inR, kN, engine::sampling::thresholdGain(41));
        bench::doNotOptimize(f);
    });
}

BENCH(bench_sampling_recorder, 2790000)
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("bench_sampling.wav");
    services::sampling::WavWriter w;
    (void) w.open(file, 44100.0, 2);
    std::vector<float> l(4096), r(4096);
    for (int i = 0; i < 4096; ++i)
        l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = 0.3f * static_cast<float>(i % 97) / 97.0f;
    std::int64_t frames = 0;
    state.run([&] {
        if (frames > 44100 * 60) // keep the file small: restart every minute of audio
        {
            w.abandon();
            (void) w.open(file, 44100.0, 2);
            frames = 0;
        }
        (void) w.write(l.data(), r.data(), 4096);
        frames += 4096;
        bench::doNotOptimize(frames);
    });
    w.abandon();
}

BENCH(bench_sampling_take, 10000000)
{
    core::sampling::CaptureHub hub;
    hub.ring.allocate(30 * 44100);
    std::vector<float> x(kN);
    for (int i = 0; i < kN; ++i)
        x[static_cast<std::size_t>(i)] = 0.001f * static_cast<float>(i);
    for (int b = 0; b < 30 * 44100 / kN - 64; ++b)
        hub.ring.write(x.data(), x.data(), kN);
    const services::sampling::TakeWindow w {hub.ring.head() - 705600 - 22050, 705600, 22050, 2, 44100.0};
    state.run([&] {
        auto t = services::sampling::buildTake(hub.ring, w);
        bench::doNotOptimize(t.ok());
    });
}
