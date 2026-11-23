// Trigger-routing benchmark (budget: "trigger routing, choke, pad notes ≤ 6 µs per block
// on the A9 for 64 events" = 6 000 A9 cycles at 1 GHz. Host numbers are ns at 1 GHz (harness convention): they
// rank and guard regressions; A9 numbers come from scripts/emulator-bench.sh bench_trigger (relative only) and
// hardware (open). The TrackSource composition cost (voice render, envelope gains) is counted in bench_voice and
// bench_ampenv, and the whole pad/kit block in bench_engine_m1.
//
//   bench_trigger       worst case: 64 events per block = 16 tracks × 4 — 48 sequencer events pushed out of frame
//                       order (Full / Half / Release / LockOnly mix) + 16 live pad
//                       hits in SCALE mode (pad → scale degree) — two choke groups of 8 tracks (each Full chokes the 7
//                       other tracks of its group), GATE on half of the tracks, a LiveHitSink; every event reaches its
//                       TrackSource (envelope + voice queues), which are not rendered here.
//   bench_trigger_locks the same with step-lock sets (volume, pan, send, trigger mode) on half of the sequencer events:
//                       + Mixer::applyLocks (strip retarget + MIX-003 gate update per locked event, the mixer's cost)
//   bench_trigger_play  typical live play: 16 pad hits per block in PLAY mode, no choke, no locks.
#include "tests/bench/harness/Bench.h"

#include "engine/mixer/Mixer.h"
#include "engine/modulation/AmpEnvelopeBank.h"
#include "engine/trigger/TrackSource.h"
#include "engine/trigger/TriggerRouter.h"

#include <array>
#include <memory>

namespace
{
using namespace engine::trigger;
constexpr int kTracks = engine::kTracks;

struct Hits final : LiveHitSink
{
    int n = 0;
    void onLiveHit(const TriggerEvent&, bool) noexcept override { ++n; }
};

struct Rig
{
    engine::modulation::AmpEnvelopeBank bank;
    std::array<TrackSource, kTracks> src;
    engine::mixer::Mixer mixer;
    TriggerRouter router;
    Hits hits;
    engine::BlockContext ctx {};
    std::array<float, 64> lockValues {};
    std::array<std::uint8_t, 64> lockChange {};
    LockSet locks {};
    alignas(16) float l[engine::kBlockFrames] {}, r[engine::kBlockFrames] {};
    std::uint32_t block = 0;
    bool withLocks = false;

    explicit Rig(bool worst, bool locked = false) : withLocks(locked)
    {
        bank.prepare(44100.0);
        mixer.prepare(44100.0, engine::kBlockFrames);
        for (int t = 0; t < kTracks; ++t)
        {
            src[static_cast<std::size_t>(t)].attach(&bank, t);
            src[static_cast<std::size_t>(t)].prepare(44100.0, engine::kBlockFrames);
        }
        router.attach(src, &mixer);
        router.prepare(44100.0, engine::kBlockFrames);
        router.setLiveHitSink(&hits);
        ctx.bpm = 120.0;
        if (worst)
        {
            for (int t = 0; t < kTracks; ++t)
            {
                router.setTrackParam(0, t, TrackField::ChokeGroup, t < 8 ? 1.0f : 2.0f);
                router.setTrackParam(0, t, TrackField::TriggerMode, static_cast<float>(t & 1));
                router.setTrackParam(0, t, TrackField::ScaleType, static_cast<float>(t % 14));
            }
            router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Scale));
            locks.mask = 1u << 0 | 1u << 5 | 1u << 6 | 1u << 9;
            lockValues[0] = 90;
            lockValues[5] = -10;
            lockValues[6] = 30;
            locks.values = lockValues.data();
            locks.change = lockChange.data();
        }
    }

    void push(bool worst) noexcept
    {
        if (! worst)
        {
            for (int t = 0; t < kTracks; ++t)
                router.pushLive(engine::TriggerNow {static_cast<std::uint8_t>(t), 100, 60, static_cast<std::int8_t>(t)});
            return;
        }
        router.setPadParam(PadField::Selected, static_cast<float>(block & 15)); // live hits first (frame 0, step 1)
        for (int p = 0; p < kTracks; ++p)
            router.pushLive(engine::TriggerNow {static_cast<std::uint8_t>(p), 110, 60, static_cast<std::int8_t>(p)});
        static constexpr TriggerKind kinds[3] = {TriggerKind::Full, TriggerKind::Half, TriggerKind::Release};
        for (int k = 0; k < 3; ++k)
            for (int t = 0; t < kTracks; ++t)
            {
                TriggerEvent e {};
                e.track = static_cast<std::uint8_t>(t);
                e.velocity = 100;
                e.kind = (k == 2 && (t & 3) == 0) ? TriggerKind::LockOnly : kinds[k];
                e.source = TriggerSource::Sequencer;
                e.frame = static_cast<std::uint8_t>((63 - t * 4 - k * 7 + static_cast<int>(block)) & 63); // unsorted
                e.locks = (withLocks && (t & 1)) ? &locks : nullptr;
                router.push(e);
            }
    }

    void drain() noexcept // the sources' per-block event queues are consumed by their render (warm-up only)
    {
        for (auto& s : src)
            s.render(ctx, l, r);
    }
};

void run(bench::State& state, bool worst, bool locks = false)
{
    auto rig = std::make_unique<Rig>(worst, locks);
    for (int i = 0; i < 50; ++i)
    {
        rig->push(worst);
        rig->router.process(rig->ctx);
        rig->drain();
    }
    // Timed: push 64 events + route them. The sources are not rendered here (their cost is bench_voice's and
    // bench_ampenv's): their per-block queues fill up and keep their latest events, which costs what draining would.
    state.run(
        [&]
        {
            rig->push(worst);
            rig->router.process(rig->ctx);
            bench::doNotOptimize(rig->router.blockEvents().data());
            ++rig->block;
        });
}
} // namespace

BENCH(bench_trigger, 6000) { run(state, true); }
BENCH(bench_trigger_locks, 6000) { run(state, true, true); }
BENCH(bench_trigger_play, 6000) { run(state, false); }
