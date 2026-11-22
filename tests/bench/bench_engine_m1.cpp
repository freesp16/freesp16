// End-to-end pad/kit engine block (budget "total ≤ 181 µs (25 %)" = 181 000 A9 cycles at 1 GHz):
// engine::Engine::process for one 64-frame block with everything kit playback has running at once —
//   16 Sample tracks, each with a stereo SampleBuffer installed through ObjectSwap, looping, pitched (+0.37 semitone:
//   every voice interpolates), two tracks retriggered every block (staggered: release tails + the sample-accurate
//   envelope handoff in the router path), the 16 envelopes in attack / release segments (CURVE on half of them),
//   GATE on half of the tracks (pad up 4 blocks after the hit), two choke groups,
//   the mixer with every strip's volume ramping (16 ParamChanges through the FIFO per block), pan off-centre, sends on,
//   master limiter on, 18 meters,
//   and the live hits arriving through the TriggerSink (UI-118 path) like pad hits.
// Host numbers rank and guard regressions (ns = cycles at 1 GHz); A9 numbers: scripts/emulator-bench.sh
// bench_engine_m1 (relative only) and hardware (open). The voice share is checked by bench_voice (≤ 120 µs), the
// envelopes by bench_ampenv (≤ 10 µs), the mixer by bench_mixer (≤ 45 µs), routing by bench_trigger (≤ 6 µs); the
// FX slot bank is built as in the app with every slot Off (≈ 1 k cycles idle, bench_fx_idle_bank).
#include "tests/bench/harness/Bench.h"

#include "core/audio/SampleBuffer.h"
#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/Engine.h"
#include "engine/voice/VoiceSwaps.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
constexpr int kTracks = engine::kTracks;
constexpr int kFrames = 1 << 17; // ≈ 3 s per sample

core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

// A stereo SampleBuffer over owned storage (pad zero frames on both sides, 16-byte aligned).
struct Sample
{
    std::vector<float> storage[2];
    core::audio::SampleBuffer buf;
    explicit Sample(int seed)
    {
        buf.frames = kFrames;
        buf.channels = 2;
        buf.sampleRate = 44100.0;
        buf.contentId = static_cast<std::uint64_t>(seed);
        buf.refs.store(0);
        for (int c = 0; c < 2; ++c)
        {
            auto& v = storage[c];
            v.assign(static_cast<std::size_t>(kFrames + 2 * core::audio::SampleBuffer::pad + 8), 0.0f);
            std::size_t off = 0;
            while ((reinterpret_cast<std::uintptr_t>(v.data() + off + core::audio::SampleBuffer::pad) & 15u) != 0)
                ++off;
            float* base = v.data() + off + core::audio::SampleBuffer::pad;
            const double w = 2.0 * 3.141592653589793 * (110.0 + 37.0 * seed + 3.0 * c) / 44100.0;
            for (int i = 0; i < kFrames; ++i)
                base[i] = 0.2f * static_cast<float>(std::sin(w * i));
            buf.ch[c] = base;
        }
    }
};

struct Rig
{
    std::vector<std::unique_ptr<Sample>> samples;
    std::unique_ptr<engine::Engine> engine;
    std::vector<float> l = std::vector<float>(engine::kBlockFrames), r = std::vector<float>(engine::kBlockFrames);
    std::uint32_t block = 0;

    Rig()
    {
        for (int t = 0; t < kTracks; ++t)
            samples.push_back(std::make_unique<Sample>(t));
        engine::EngineOptions o;
        o.testTone = false;
        // The app's set-up: the FX slot bank is built and routed, every slot Off (the idle bank's cost is in
        // the number; FX budgets are bench_fx*).
        engine::EngineSetup setup {[](std::string_view s) { return core::builtinParams().idOf(s).value_or(core::ParamId {}); }};
        for (const auto& spec : core::builtinParams().all())
            setup.paramIds.push_back(spec.id);
        setup.fxInternalIndex = core::fx::internalIndexTable(core::builtinParams());
        engine = std::make_unique<engine::Engine>(setup, o);
        engine->prepare(44100.0, engine::kBlockFrames);
        auto& b = engine->bridge();
        for (int t = 0; t < kTracks; ++t)
        {
            const auto scope = static_cast<std::uint16_t>(t);
            core::audio::retain(&samples[static_cast<std::size_t>(t)]->buf);
            b.post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, t), &samples[static_cast<std::size_t>(t)]->buf});
            b.post(engine::ParamChange {id("playback.loop"), scope, 1.0f, 0});
            b.post(engine::ParamChange {id("playback.pitch"), scope, 0.37f + 0.1f * static_cast<float>(t), 0});
            b.post(engine::ParamChange {id("env.attack"), scope, 70.0f + static_cast<float>(t), 0});
            b.post(engine::ParamChange {id("env.hold"), scope, 60.0f, 0});
            b.post(engine::ParamChange {id("env.release"), scope, 80.0f + static_cast<float>(t), 0});
            b.post(engine::ParamChange {id("env.attackCurve"), scope, static_cast<float>(t & 1), 0});
            b.post(engine::ParamChange {id("env.releaseCurve"), scope, static_cast<float>(t & 1), 0});
            b.post(engine::ParamChange {id("track.triggerMode"), scope, static_cast<float>(t & 1), 0});
            b.post(engine::ParamChange {id("track.chokeGroup"), scope, t == 14 ? 1.0f : (t == 15 ? 2.0f : 0.0f), 0});
            b.post(engine::ParamChange {id("track.pan"), scope, static_cast<float>(t * 8 - 64), 0});
            b.post(engine::ParamChange {id("track.sendAmount"), scope, 40.0f, 0});
        }
        b.post(engine::ParamChange {id("mixer.masterLimiter"), 0xFFFF, 1.0f, 0});
        for (int t = 0; t < kTracks; ++t) // everything sounding before the timed blocks
            engine->triggers().push({static_cast<std::uint8_t>(t), 120, 60, static_cast<std::int8_t>(t)});
        for (int i = 0; i < 400; ++i)
            step();
        const auto& m = engine->bridge().meters(); // set-up check: all 16 strips carry signal
        for (int t = 0; t < kTracks; ++t)
            if (m.trackPeak[static_cast<std::size_t>(t)][0] <= 0.0f)
                std::abort();
    }

    void step() noexcept
    {
        auto& b = engine->bridge();
        for (int t = 0; t < kTracks; ++t) // every fader ramps (100-sample ramps restarted every block)
            b.post(engine::ParamChange {id("track.volume"), static_cast<std::uint16_t>(t),
                                        static_cast<float>(80 + ((block + static_cast<std::uint32_t>(t)) & 15)), 0});
        for (int k = 0; k < 2; ++k) // two hits per block, staggered over the 16 tracks
        {
            const int t = static_cast<int>((block * 2 + static_cast<std::uint32_t>(k)) % kTracks);
            engine->triggers().push({static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(90 + 2 * t), 60,
                                     static_cast<std::int8_t>(t)});
            const int up = (t + 8) % kTracks; // GATE tracks: pad up a few blocks after their hit
            b.post(engine::TriggerNow {static_cast<std::uint8_t>(up), 0, 60, static_cast<std::int8_t>(up)});
        }
        float* out[2] = {l.data(), r.data()};
        engine->process(out, 2, engine::kBlockFrames);
        ++block;
        b.retirePending();
    }
};
} // namespace

BENCH(bench_engine_m1, 181000)
{
    auto rig = std::make_unique<Rig>();
    state.run(
        [&]
        {
            rig->step();
            bench::doNotOptimize(rig->l[17]);
        });
    bench::doNotOptimize(rig->engine->voiceStarts());
}
