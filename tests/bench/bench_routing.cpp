// Output routing benchmarks (budget: ≤ 4 k cycles per block worst case — 16 assign adds
// of 128 floats ≈ 1.4 k NEON, headphone router 400, monitor 300, fades only while active — and ≤ 300 cycles in the
// common case, M + 6CH with nothing assigned). Budgets in A9 cycles at 1 GHz; the host is far faster, so
// these numbers rank and guard regressions; device numbers come from scripts/emulator-bench.sh bench_routing and the
// hardware.
//
//   bench_routing_common         M + 6CH, nothing assigned, no headphone port: steps 12–13 (OUT 1-2 = the master
//                                pointer, unity trim: no copy at all).
//   bench_routing_common_phones  the same with the headphone port (cue MASTER, HEADPHONES 100: one copy + one scale).
//   bench_routing                worst case: 16 tracks assigned (8 pairs, 8 mono), every assignment fading, the mode
//                                dip running, the headphones on a pair with preview/metronome + the input monitor
//                                (gate ramping), the input monitor on and its level ramping, the trim ramping and the
//                                mute-all fade running — steps 11a (monitor) + 12 + 13.
#include "engine/mixer/OutputRouterStage.h"
#include "tests/bench/harness/Bench.h"

#include <cstring>
#include <memory>

namespace
{
constexpr int kN = engine::kBlockFrames;
using engine::mixer::RoutingParam;

struct Rig
{
    std::unique_ptr<engine::mixer::Mixer> mixer = std::make_unique<engine::mixer::Mixer>();
    std::unique_ptr<engine::mixer::OutputRouterStage> stage = std::make_unique<engine::mixer::OutputRouterStage>();
    std::unique_ptr<engine::stages::BlockBuffers> b = std::make_unique<engine::stages::BlockBuffers>();
    engine::mixer::Hooks hooks {};
    engine::BlockContext ctx {};
    engine::stages::StereoBlock input {};

    explicit Rig(std::uint8_t ports)
    {
        mixer->setTrimInMaster(false);
        stage->prepare(44100.0);
        stage->setPresent(ports);
        std::uint32_t s = 7;
        for (int i = 0; i < kN; ++i)
        {
            s = s * 1664525u + 1013904223u;
            const float x = 0.4f * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
            b->master.l[i] = x;
            b->master.r[i] = -x;
            b->aux.l[i] = b->aux.r[i] = 0.5f * x;
            input.l[i] = input.r[i] = 0.25f * x;
            for (auto& t : b->track)
            {
                t.l[i] = x;
                t.r[i] = 0.5f * x;
            }
        }
        b->activeTracks = 0xFFFF;
        mixer->processTracks(*b, hooks); // the strips' post-fader mask: all 16 (unity strips)
    }
};
} // namespace

BENCH(bench_routing_common, 300)
{
    Rig rig(0x01);
    auto& st = *rig.stage;
    state.run([&] {
        st.monitor(*rig.b, *rig.mixer);
        st.process(rig.ctx, *rig.b, *rig.mixer);
        st.output(rig.ctx, *rig.b);
        bench::doNotOptimize(rig.b->port[0]);
    });
}

BENCH(bench_routing_common_phones, 700)
{
    Rig rig(0x11);
    auto& st = *rig.stage;
    state.run([&] {
        st.monitor(*rig.b, *rig.mixer);
        st.process(rig.ctx, *rig.b, *rig.mixer);
        st.output(rig.ctx, *rig.b);
        bench::doNotOptimize(rig.b->port[8]);
    });
}

BENCH(bench_routing, 4000)
{
    Rig rig(0x1F);
    auto& st = *rig.stage;
    rig.b->inputL = rig.input.l;
    rig.b->inputR = rig.input.r;
    rig.b->auxActive = true;
    st.set(RoutingParam::Monitor, 0, 1.0f);
    st.set(RoutingParam::AssignMode, 0, 1.0f); // 8CH: every track has an output
    st.set(RoutingParam::HeadphoneCue, 0, 3.0f); // OUT 5-6
    std::uint32_t n = 0;
    state.run([&] {
        ++n;
        const bool odd = (n & 1u) != 0;
        // Keep every fade running: new assignments, mode, trim, monitor level and mute-all each block.
        for (int t = 0; t < engine::kTracks; ++t)
        {
            const int idx = t < 8 ? 1 + ((t + (odd ? 1 : 0)) & 3) : 5 + ((t + (odd ? 3 : 0)) & 7);
            st.set(RoutingParam::OutAssign, static_cast<std::uint16_t>(t), static_cast<float>(idx));
        }
        if ((n & 3u) == 0)
            st.set(RoutingParam::AssignMode, 0, odd ? 1.0f : 0.0f);
        st.set(RoutingParam::MonitorLevel, 0, odd ? 90.0f : 110.0f);
        st.setOutputTrim(odd ? 900.0f : 1000.0f);
        st.router().setMuteAll(odd); // a 64-sample fade every block
        st.monitor(*rig.b, *rig.mixer);
        st.process(rig.ctx, *rig.b, *rig.mixer);
        st.output(rig.ctx, *rig.b);
        bench::doNotOptimize(rig.b->port[2]);
        bench::doNotOptimize(rig.b->port[8]);
    });
}
