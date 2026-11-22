// Helpers for the routing unit tests: heap rigs (a BlockBuffers is ~10 KB, never on a test's stack),
// a mixer + routing stage chain that runs steps 8b–13 the way engine::Engine does, and signal helpers.
#pragma once

#include "engine/mixer/HeadphoneRouter.h"
#include "engine/mixer/InputMonitor.h"
#include "engine/mixer/Mixer.h"
#include "engine/mixer/OutputRouter.h"
#include "engine/mixer/OutputRouterStage.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace routingtest
{
constexpr int kN = engine::kBlockFrames;
using engine::mixer::AssignMode;
using engine::mixer::OutAssign;

inline constexpr OutAssign pair(int p) { return {static_cast<std::int8_t>(p), -1}; }
inline constexpr OutAssign mono(int ch) { return {-1, static_cast<std::int8_t>(ch)}; }

// A bare OutputRouter fed with 16 track blocks (filled by the test) and a master block.
struct RouterRig
{
    engine::mixer::OutputRouter router;
    std::unique_ptr<std::array<engine::stages::StereoBlock, engine::kTracks>> tracks =
        std::make_unique<std::array<engine::stages::StereoBlock, engine::kTracks>>();
    engine::stages::StereoBlock master {};
    engine::mixer::OutputBuffers out {};
    std::uint32_t valid = 0xFFFF;

    RouterRig() { router.prepare(44100.0, {}); }
    void setTrack(int t, float l, float r)
    {
        for (int i = 0; i < kN; ++i)
        {
            (*tracks)[static_cast<std::size_t>(t)].l[i] = l;
            (*tracks)[static_cast<std::size_t>(t)].r[i] = r;
        }
    }
    void setMaster(float l, float r)
    {
        for (int i = 0; i < kN; ++i)
        {
            master.l[i] = l;
            master.r[i] = r;
        }
    }
    void block()
    {
        const float* m[2] = {master.l, master.r};
        router.process({tracks->data(), valid}, m, out);
    }
    // Channel n (0…7) of this block, frame i (0 when the channel is silent).
    float at(int n, int i) const
    {
        const float* c = out.channel(n);
        return c == nullptr ? 0.0f : c[i];
    }
};

// Steps 8b–13 the way the engine runs them: mixer strips + buses, input monitor, master, router, output stage.
struct ChainRig
{
    std::unique_ptr<engine::mixer::Mixer> mixer = std::make_unique<engine::mixer::Mixer>();
    std::unique_ptr<engine::mixer::OutputRouterStage> stage = std::make_unique<engine::mixer::OutputRouterStage>();
    std::unique_ptr<engine::stages::BlockBuffers> b = std::make_unique<engine::stages::BlockBuffers>();
    engine::mixer::Hooks hooks {};
    engine::BlockContext ctx {};

    ChainRig()
    {
        mixer->setTrimInMaster(false);
        stage->prepare(44100.0);
        stage->setPresent(0x1F); // Main, OUT 3-4 … 7-8, Phones
    }
    // One block; track t gets `value(t, i)` on both channels when its bit is in `active`.
    template <class Fn> void block(std::uint32_t active, Fn value)
    {
        b->activeTracks = active;
        b->auxActive = false;
        for (int t = 0; t < engine::kTracks; ++t)
            for (int i = 0; i < kN; ++i)
                b->track[static_cast<std::size_t>(t)].l[i] = b->track[static_cast<std::size_t>(t)].r[i] = value(t, i);
        mixer->processTracks(*b, hooks);
        mixer->processSend(*b, hooks);
        stage->monitor(*b, *mixer);
        mixer->processMaster(*b, hooks);
        stage->process(ctx, *b, *mixer);
        stage->output(ctx, *b);
    }
    void dc(std::uint32_t active, float v = 0.5f)
    {
        block(active, [v](int, int) { return v; });
    }
    const float* port(engine::OutPort p, int side) const
    {
        return b->port[static_cast<std::size_t>(2 * static_cast<int>(p) + side)];
    }
    float peak(engine::OutPort p) const
    {
        float m = 0.0f;
        for (int s = 0; s < 2; ++s)
            if (const float* x = port(p, s))
                for (int i = 0; i < kN; ++i)
                    m = std::fabs(x[i]) > m ? std::fabs(x[i]) : m;
        return m;
    }
};

struct Noise // xorshift in [-a, a)
{
    std::uint32_t s = 0x2468ACE1u;
    float next(float a = 1.0f)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return a * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
    }
};
} // namespace routingtest
