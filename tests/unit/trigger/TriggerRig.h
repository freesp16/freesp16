// Test rig of the trigger router (tests only): 16 TrackSources on one AmpEnvelopeBank, a Mixer for the lock / mute
// state, and a block driver that runs router + sources the way engine::Engine's steps 6 and 8a do, on a thread marked
// real-time (any allocation aborts under APP_RT_CHECKS).
#pragma once

#include "tests/unit/voice/VoiceHarness.h"

#include "core/util/Realtime.h"
#include "engine/mixer/Mixer.h"
#include "engine/modulation/AmpEnvelopeBank.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/trigger/TrackSource.h"
#include "engine/trigger/TriggerRouter.h"

#include <array>
#include <memory>
#include <vector>

namespace trigtest
{
using namespace engine::trigger;
using engine::kBlockFrames;
using engine::kTracks;
using voicetest::TestBuffer;

inline std::vector<float> dc(int frames, float v = 0.5f) { return std::vector<float>(static_cast<std::size_t>(frames), v); }

struct Rig
{
    engine::modulation::AmpEnvelopeBank bank;
    std::array<TrackSource, kTracks> src;
    engine::mixer::Mixer mixer;
    TriggerRouter router;
    engine::BlockContext ctx {};
    std::array<engine::stages::StereoBlock, kTracks> out {};
    std::uint32_t active = 0;
    // Per track: every rendered frame of the left channel (0 for silent blocks), for frame-exact checks.
    std::array<std::vector<float>, kTracks> history;

    Rig()
    {
        bank.prepare(voicetest::kRate);
        mixer.prepare(voicetest::kRate, kBlockFrames);
        for (int t = 0; t < kTracks; ++t)
        {
            auto& s = src[static_cast<std::size_t>(t)];
            s.attach(&bank, t);
            s.prepare(voicetest::kRate, kBlockFrames);
        }
        router.attach(src, &mixer);
        router.prepare(voicetest::kRate, kBlockFrames);
        ctx.sampleRate = voicetest::kRate;
        ctx.frames = kBlockFrames;
        ctx.bpm = 120.0;
    }

    void param(int track, TrackField f, float v, int scene = 0) { router.setTrackParam(scene, track, f, v); }
    void env(int track, const engine::modulation::AmpEnvParams& p) { bank.setParams(track, p, 120.0); }

    // One block: the router applies its events, then every source renders (inL/inR: Thru input).
    std::uint32_t block(const float* inL = nullptr, const float* inR = nullptr)
    {
        {
            core::rt::ScopedRealtimeThread realtime;
            ctx.triggerMask = 0;
            router.process(ctx);
            active = 0;
            for (int t = 0; t < kTracks; ++t)
            {
                auto& b = out[static_cast<std::size_t>(t)];
                if (src[static_cast<std::size_t>(t)].render(ctx, inL, inR, b.l, b.r))
                    active |= 1u << t;
            }
        }
        for (int t = 0; t < kTracks; ++t) // not real-time: the history grows
        {
            const bool on = (active >> t & 1u) != 0;
            auto& h = history[static_cast<std::size_t>(t)];
            for (int i = 0; i < kBlockFrames; ++i)
                h.push_back(on ? out[static_cast<std::size_t>(t)].l[i] : 0.0f);
        }
        ++ctx.blockIndex;
        return active;
    }
    void blocks(int n)
    {
        for (int i = 0; i < n; ++i)
            block();
    }

    static TriggerEvent event(int track, TriggerKind kind, int frame, TriggerSource source = TriggerSource::Sequencer,
                              int velocity = 127)
    {
        TriggerEvent e {};
        e.track = static_cast<std::uint8_t>(track);
        e.velocity = static_cast<std::uint8_t>(velocity);
        e.kind = kind;
        e.source = source;
        e.frame = static_cast<std::uint8_t>(frame);
        e.locks = nullptr;
        return e;
    }
};

inline std::unique_ptr<Rig> makeRig() { return std::make_unique<Rig>(); }
} // namespace trigtest
