// Sample voice benchmarks. One iteration = one
// 64-frame engine block. Budgets in A9 cycles (1 GHz):
//   16 voices + 16 release tails, pitched, stereo, every track retriggering every block   ≤ 120 µs = 120 000
//   the unpitched fast path (rate ±1), 16 voices                                          ≤  30 µs =  30 000
//   one pitched stereo voice + tail                                                       ≤ 7.5 µs =   7 500
//   preview voice                                                                         ≤   6 µs =   6 000
// Sweep: rate 1.0, 1.06 (+100 cents), 0.5 (−1200 cents), −1.0 (reverse); mono and stereo buffers; 1, 8, 16 voices.
// Host numbers rank and guard regressions (ci/bench-baseline.json); the A9 numbers come from
// `scripts/emulator-bench.sh bench_voice` (relative only, qemu) and from hardware (open until measured there).
#include "tests/bench/harness/Bench.h"

#include "engine/voice/InterpKernels.h"
#include "engine/voice/PreviewVoice.h"
#include "engine/voice/TrackVoice.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
using core::audio::SampleBuffer;
using engine::kBlockFrames;
using engine::voice::TrackVoice;

constexpr double kRate = 44100.0;

// A padded, 16-byte aligned SampleBuffer of noise-like content (set-up only: allocates).
struct Buffer
{
    std::vector<float> storage[2];
    SampleBuffer sb;
    Buffer(int frames, int channels)
    {
        sb.frames = frames;
        sb.channels = static_cast<std::uint8_t>(channels);
        sb.sampleRate = kRate;
        sb.contentId = 0;
        sb.refs.store(0);
        sb.ch[0] = sb.ch[1] = nullptr;
        std::uint32_t seed = 12345u;
        for (int c = 0; c < channels; ++c)
        {
            auto& v = storage[c];
            v.assign(static_cast<std::size_t>(frames + 2 * SampleBuffer::pad + 4), 0.0f);
            std::size_t offset = 0;
            while ((reinterpret_cast<std::uintptr_t>(v.data() + offset + SampleBuffer::pad) & 15u) != 0)
                ++offset;
            float* base = v.data() + offset + SampleBuffer::pad;
            for (int i = 0; i < frames; ++i)
            {
                seed = seed * 1664525u + 1013904223u;
                base[i] = static_cast<float>(static_cast<std::int32_t>(seed)) * (0.5f / 2147483648.0f);
            }
            sb.ch[c] = base;
        }
    }
};

struct alignas(16) Block
{
    alignas(16) float l[kBlockFrames];
    alignas(16) float r[kBlockFrames];
};

struct Rig
{
    Buffer buffer;
    std::array<TrackVoice, 16> voices;
    std::array<Block, 16> dst {};
    std::array<Block, 16> tail {};
    engine::voice::PlaybackParams params {};
    Rig(int channels, bool loop, std::int64_t loopLength) : buffer(200000, channels)
    {
        params.region = {1000, 1000, loop ? loopLength : 150000, loop, false};
        for (auto& v : voices)
        {
            v.prepare(kRate, kBlockFrames);
            v.setSample(&buffer.sb);
        }
    }
};

// Every voice retriggers at frame 0 of every block (the old note → tail, a 2 ms fade, so main + tail + the cut of
// the previous tail all play): the "all tracks retriggering" worst case of 09 §0.5.
void retriggering(bench::State& state, int voices, int channels, float cents, bool reverse,
                  engine::voice::InterpolatorKind quality = engine::voice::InterpolatorKind::Hermite4)
{
    auto rig = std::make_unique<Rig>(channels, false, 0);
    rig->params.region.reverse = reverse;
    for (auto& v : rig->voices)
        v.setQuality(quality);
    state.run([&] {
        for (int t = 0; t < voices; ++t)
        {
            auto& v = rig->voices[static_cast<std::size_t>(t)];
            v.start(rig->params, cents, 0, 0.8f);
            auto& d = rig->dst[static_cast<std::size_t>(t)];
            auto& tl = rig->tail[static_cast<std::size_t>(t)];
            v.render(d.l, d.r, tl.l, tl.r);
        }
        bench::doNotOptimize(rig->dst[0].l[17]);
        bench::clobberMemory();
    });
}

// Looping voices that stay near their loop end: half of every block is the crossfaded seam (2 reads).
void seam(bench::State& state, int voices, int channels, float cents)
{
    auto rig = std::make_unique<Rig>(channels, true, 128);
    for (auto& v : rig->voices)
        v.start(rig->params, cents, 0, 1.0f);
    state.run([&] {
        for (int t = 0; t < voices; ++t)
        {
            auto& d = rig->dst[static_cast<std::size_t>(t)];
            auto& tl = rig->tail[static_cast<std::size_t>(t)];
            rig->voices[static_cast<std::size_t>(t)].render(d.l, d.r, tl.l, tl.r);
        }
        bench::doNotOptimize(rig->dst[0].l[17]);
        bench::clobberMemory();
    });
}
} // namespace

// ---- the 09 §0.5 budget: 16 voices + 16 tails, pitched, stereo ----
BENCH(bench_voice_16_pitched_stereo, 120000) { retriggering(state, 16, 2, 100.0f, false); }
BENCH(bench_voice_16_pitched_mono, 120000) { retriggering(state, 16, 1, 100.0f, false); }
BENCH(bench_voice_16_octave_down_stereo, 120000) { retriggering(state, 16, 2, -1200.0f, false); }
BENCH(bench_voice_16_pitched_reverse_stereo, 120000) { retriggering(state, 16, 2, 100.0f, true); }
BENCH(bench_voice_16_seam_pitched_stereo, 120000) { seam(state, 16, 2, 100.0f); }
BENCH(bench_voice_8_pitched_stereo, 60000) { retriggering(state, 8, 2, 100.0f, false); }
BENCH(bench_voice_1_pitched_stereo, 7500) { retriggering(state, 1, 2, 100.0f, false); }

// ---- the unpitched fast path (copy): 16 voices ≤ 30 µs ----
BENCH(bench_voice_16_unpitched_stereo, 30000) { retriggering(state, 16, 2, 0.0f, false); }
BENCH(bench_voice_16_unpitched_mono, 30000) { retriggering(state, 16, 1, 0.0f, false); }
BENCH(bench_voice_16_unpitched_reverse_stereo, 30000) { retriggering(state, 16, 2, 0.0f, true); }

// ---- render / quality mode (sinc16): not a live budget; the whole engine's share bounds it ----
BENCH(bench_voice_16_sinc16_pitched_stereo, 700000)
{
    retriggering(state, 16, 2, 100.0f, false, engine::voice::InterpolatorKind::Sinc16);
}

// ---- preview (BROWSE) ≤ 6 µs ----
BENCH(bench_voice_preview_stereo, 6000)
{
    auto buffer = std::make_unique<Buffer>(200000, 2);
    auto pv = std::make_unique<engine::voice::PreviewVoice>();
    pv->prepare(kRate);
    pv->install(&buffer->sb);
    pv->setGain(0.7f);
    auto out = std::make_unique<Block>();
    state.run([&] {
        if (! pv->active()) // played to the end: start it again (a new install), so every block plays
        {
            pv->install(nullptr);
            pv->install(&buffer->sb);
        }
        pv->render(out->l, out->r);
        bench::doNotOptimize(out->l[3]);
        bench::clobberMemory();
    });
}

// ---- the Hermite kernel alone: 64 stereo frames, NEON vs the scalar reference ----
BENCH(bench_voice_hermite4_kernel_stereo, 3750)
{
    auto buffer = std::make_unique<Buffer>(100000, 2);
    auto out = std::make_unique<Block>();
    namespace k = engine::voice::kernels;
    const k::Fixed step = k::toFixed(1.0594630943592953);
    k::Fixed pos = k::toFixed(1000.25);
    state.run([&] {
        float* dst[2] = {out->l, out->r};
        k::hermite4(buffer->sb.ch, 2, pos, step, dst, kBlockFrames);
        pos = pos + step * kBlockFrames > k::toFixed(90000.0) ? k::toFixed(1000.25) : pos + step * kBlockFrames;
        bench::doNotOptimize(out->l[5]);
        bench::clobberMemory();
    });
}

BENCH(bench_voice_hermite4_kernel_stereo_scalar, 15000)
{
    auto buffer = std::make_unique<Buffer>(100000, 2);
    auto out = std::make_unique<Block>();
    namespace k = engine::voice::kernels;
    const k::Fixed step = k::toFixed(1.0594630943592953);
    k::Fixed pos = k::toFixed(1000.25);
    state.run([&] {
        float* dst[2] = {out->l, out->r};
        k::scalar::hermite4(buffer->sb.ch, 2, pos, step, dst, kBlockFrames);
        pos = pos + step * kBlockFrames > k::toFixed(90000.0) ? k::toFixed(1000.25) : pos + step * kBlockFrames;
        bench::doNotOptimize(out->l[5]);
        bench::clobberMemory();
    });
}
