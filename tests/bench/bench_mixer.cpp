// Mixer benchmarks (performance budget): "16 strips + send/master buses + 18 meters ≤ 45 µs per block on
// the A9, all ramps running". Budgets in A9 cycles at 1 GHz (45 µs = 45 000 cycles); on the host the gate is that A9
// budget (the host is far faster — the numbers rank and guard regressions; device numbers come from
// scripts/emulator-bench.sh bench_mixer, relative only, and from hardware when available).
//
//   bench_mixer         worst case: every block, all 16 strips get new volume/pan/send targets (four 100-sample ramps
//                       each, restarted every block), every track's mute flips (128-sample fades always running), Send FX
//                       and Master FX hooks installed (empty), master fader ramping, master limiter on with the master
//                       above its threshold, 18 meters. Includes copying 16 fresh 64-frame stereo sources into the
//                       track buffers (what the voices write; ~8 KB).
//   bench_mixer_block_start  the same with every change at frame 0 (FIFO / per-block modulation).
//   bench_mixer_idle    typical: 16 active tracks at unity (no ramps, no send), no FX — the bit-transparent path.
//   bench_mixer_strip_neon / _scalar   one strip with ramping gains, mute ramp and send tap: the kernel alone.
#include "engine/mixer/Mixer.h"
#include "engine/mixer/MixerKernels.h"
#include "tests/bench/harness/Bench.h"

#include <cstring>
#include <memory>

namespace
{
constexpr int kN = engine::kBlockFrames;

struct Empty final : engine::mixer::BusInsert
{
    void process(float*, float*) noexcept override {}
};

struct Rig
{
    std::unique_ptr<engine::mixer::Mixer> mixer = std::make_unique<engine::mixer::Mixer>();
    std::unique_ptr<engine::stages::BlockBuffers> b = std::make_unique<engine::stages::BlockBuffers>();
    std::unique_ptr<engine::stages::BlockBuffers> src = std::make_unique<engine::stages::BlockBuffers>();
    engine::mixer::Hooks hooks {};
    engine::BlockContext ctx {};

    Rig()
    {
        std::uint32_t s = 1;
        for (auto& t : src->track)
            for (int i = 0; i < kN; ++i)
            {
                s = s * 1664525u + 1013904223u;
                t.l[i] = 0.4f * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
                t.r[i] = -t.l[i];
            }
        b->activeTracks = 0xFFFFu;
    }
    void loadSources() noexcept { std::memcpy(b->track.data(), src->track.data(), sizeof(b->track)); }
};
} // namespace

namespace
{
// Every block: new volume/pan/send/mute targets on all 16 strips at frame `frame(t)`, master fader change, then the block.
template <class Frame> void allRamps(bench::State& state, Frame frame)
{
    Rig rig;
    Empty sendFx, masterFx;
    rig.hooks.sendFx = &sendFx;
    rig.hooks.masterFx = &masterFx;
    auto& m = *rig.mixer;
    m.setGlobal(engine::mixer::GlobalParam::MasterLimiter, 1.0f);
    std::uint32_t n = 0;
    state.run([&] {
        ++n;
        const bool odd = (n & 1u) != 0;
        for (int t = 0; t < engine::kTracks; ++t)
        {
            engine::mixer::StripParams p;
            p.volume = static_cast<std::uint8_t>(odd ? 127 : 90);
            p.pan = static_cast<std::int8_t>(odd ? -40 : 40);
            p.send = static_cast<std::uint8_t>(odd ? 127 : 60);
            p.mute = odd; // flips every block: every mute fade keeps running
            m.setStrip(0, t, p, frame(t));
        }
        m.setGlobal(engine::mixer::GlobalParam::MasterVolume, odd ? 127.0f : 110.0f, 0);
        rig.loadSources();
        m.process(rig.ctx, *rig.b, rig.hooks);
        bench::doNotOptimize(rig.b->master.l[0]);
        bench::doNotOptimize(m.meters().peak[17][0]);
    });
}
} // namespace

// Changes at staggered frames inside the block (sequencer locks): the ramps' general path, the absolute worst case.
BENCH(bench_mixer, 45000)
{
    allRamps(state, [](int t) { return t * 3; });
}

// Changes at the block start (ParamChange from the FIFO, LFO-style per-block updates): the ramps' whole-block path.
BENCH(bench_mixer_block_start, 45000)
{
    allRamps(state, [](int) { return 0; });
}

BENCH(bench_mixer_idle, 20000)
{
    Rig rig;
    auto& m = *rig.mixer;
    state.run([&] {
        m.process(rig.ctx, *rig.b, rig.hooks); // unity strips write nothing back: no source copy needed
        bench::doNotOptimize(rig.b->master.l[0]);
    });
}

namespace
{
struct StripRig
{
    alignas(16) float l[kN], r[kN], mainL[kN] {}, mainR[kN] {}, sendL[kN] {}, sendR[kN] {}, gains[4 * kN], mute[kN];
    StripRig()
    {
        for (int i = 0; i < kN; ++i)
        {
            l[i] = r[i] = 0.5f;
            mute[i] = 1.0f - static_cast<float>(i) / 128.0f;
        }
        for (int i = 0; i < 4 * kN; ++i)
            gains[i] = 1.0f - static_cast<float>(i) * 1e-4f;
    }
    engine::mixer::kernels::StripArgs args() noexcept
    {
        engine::mixer::kernels::StripArgs a;
        a.l = l, a.r = r, a.mainL = mainL, a.mainR = mainR, a.sendL = sendL, a.sendR = sendR;
        a.gains = gains, a.mute = mute, a.rampGains = true;
        return a;
    }
};
} // namespace

#if APP_MIXER_HAVE_NEON
BENCH(bench_mixer_strip_neon, 2500)
{
    auto s = std::make_unique<StripRig>();
    state.run([&] {
        const auto m = engine::mixer::kernels::neon::strip(s->args(), kN);
        bench::doNotOptimize(m);
    });
}
#endif

BENCH(bench_mixer_strip_scalar, 10000)
{
    auto s = std::make_unique<StripRig>();
    state.run([&] {
        const auto m = engine::mixer::kernels::scalar::strip(s->args(), kN);
        bench::doNotOptimize(m);
    });
}
