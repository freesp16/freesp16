// The stages of the engine block, in the order of audio-engine.md "Per-block processing order" (steps 2–13; step 1 —
// the FIFO drain — and step 14 — publishing the feeds — are engine::Engine's own). Each stage is a plain member object
// of engine::Engine, called directly (no virtual graph, no virtual call per sample). Every stage has its final place,
// its prepare() and its process(); a stage is filled in (or replaced with a class of the same shape) without touching
// the block loop's order:
//
//    2 MIDI in              timestamped MIDI events of this block (engine/midi/MidiEngine.h processIn)
//    3 TransportStage       PPQ range [p0, p1), tempo, start/stop edges, pre-count, metronome
//    4 SyncFollowerStage    external tempo/phase correction
//    5 SceneSwitchStage     queued scene/pattern switch, song sections
//    6 SequencerStage       step events → TriggerRouter (+ live triggers, choke, pad modes, scales → the 16
//                           TrackSources)
//    7 MIDI out             notes/CC/clock out (engine/midi/MidiEngine.h processOut)
//    8 TrackStage × 16      source → envelope → insert 1 → insert 2
//   8b strips + buses       engine::mixer::Mixer::processTracks (pan → mute → send tap → fader)
//    9 SendStage            send bus → Send FX (Mixer hook) → return fader
//   10 MasterFxStage        the Mixer's masterFx hook, applied in step 11's call
//   11 MasterOutStage       master FX out + return (+ monitor) → master fader → aux → (soft limiter) → master meter
//                           (the output trim, MIX-060, is step 13's)
//   12 OutputRouterStage    main / assignable pairs / phones (engine/mixer/OutputRouterStage.h)
//   13 OutputStage          output trim per output + mute-all → ports (meters: Engine, NEON)
//
// Empty means *transparent*: no allocation, no per-sample work, unity gain (NFR-030: unity paths are bit-exact).
#pragma once

#include "engine/EngineApi.h"
#include "engine/stages/BlockBuffers.h"

#include "engine/dsp/simd/Kernels.h"
#include "engine/mixer/Mixer.h"
#include "engine/mixer/OutputRouterStage.h" // 12–13
#include "engine/scene/SceneSwitchStage.h" // 5
#include "engine/sequencer/SequencerStage.h"
#include "engine/transport/TransportStage.h"
#include "engine/trigger/TriggerRouter.h"

#include <cstdint>
#include <cstring>

namespace engine::stages
{
// 2 — MIDI input: engine::midi::MidiEngine::processIn (engine/midi/MidiEngine.h), a member of Engine.

// 3 — Transport + metronome (engine/transport/TransportStage.h): PPQ range [p0, p1), tempo,
// start/stop/hard-stop edges, pre-count (BlockBuffers::timing.preCount); renderClick adds the metronome into aux.
using TransportStage = transport::TransportStage;

// 4 — external sync (empty: transparent).
struct SyncFollowerStage
{
    void prepare(double) noexcept {}
    void process(BlockContext&, BlockBuffers&) noexcept {}
};

// 5 — scene/pattern switcher: stages::SceneSwitchStage, engine/scene/SceneSwitchStage.h.

// 6 — sequencer (engine/sequencer/SequencerStage.h): pushes this block's step events (frame, kind, locks)
// into the trigger router, which merges them with the live pad/MIDI hits and applies all of them in frame
// order right after; records the router's live hits (LiveHitSink).
using SequencerStage = sequencer::SequencerStage;

// 7 — MIDI out: engine::midi::MidiEngine::processOut: MIDI tracks, clock / transport / SPP out.

// 8 — one insert slot (empty; the FX slot bank does the work): an idle slot costs one branch ("idle FX slot ≈ 0").
struct InsertSlot
{
    bool active = false;
    void prepare(double) noexcept {}
    void process(const BlockContext&, StereoBlock&) noexcept {}
};

// 8 — one track: source (engine::trigger::TrackSource, step 8a: voice × envelope / Thru input, rendered by Engine) →
// insert 1 → insert 2; the strip (pan, mute ramp, send tap, fader) and the bus sums are
// engine::mixer::Mixer::processTracks, called by Engine for all tracks at once.
struct TrackStage
{
    InsertSlot insert[kInsertsPerTrack];

    void prepare(double sampleRate) noexcept
    {
        for (auto& i : insert)
            i.prepare(sampleRate);
    }
    // Inserts only; the strips and bus sums run for all tracks at once in the mixer.
    void process(const BlockContext& ctx, StereoBlock& buffer, bool active) noexcept
    {
        if (! active)
            return;
        for (auto& i : insert)
            if (i.active)
                i.process(ctx, buffer);
    }
};

// 9 — send bus → Send FX → return fader. Send FX Off (hook null) returns silence (audio-engine.md "Buses").
struct SendStage
{
    void prepare(double) noexcept {}
    void process(BlockContext&, BlockBuffers& b, mixer::Mixer& m, const mixer::Hooks& h) noexcept
    {
        m.processSend(b, h);
    }
};

// 10 — Master FX (empty: transparent).
struct MasterFxStage
{
    void prepare(double) noexcept {}
    void process(BlockContext&, BlockBuffers&) noexcept {}
};

// 11 — master out = master FX out + send return (+ input monitor) → master fader → aux (preview, metronome) →
// optional master soft limiter → master meter → output trim. The Master FX hook runs first, in place.
struct MasterOutStage
{
    void prepare(double) noexcept {}
    void process(BlockContext&, BlockBuffers& b, mixer::Mixer& m, const mixer::Hooks& h) noexcept
    {
        m.processMaster(b, h);
    }
};

// 12 — output router (engine/mixer/OutputRouterStage.h): Main / OUT 3-4 / 5-6 / 7-8 from the master
// and the strips' post-fader blocks (M + 6CH / 8CH, OUT ASSIGN), the headphone cue (+ preview, metronome, input
// monitor); monitor() runs before step 11 (the input monitor into the master).
using OutputRouterStage = mixer::OutputRouterStage;

// 13 — output trim per logical output (MIX-060 / MIX-023) and the mute-all fade → BlockBuffers::port;
// the meters of step 13 are computed by Engine.
struct OutputStage
{
    void prepare(double) noexcept {}
    void process(BlockContext& ctx, BlockBuffers& b, OutputRouterStage& router) noexcept { router.output(ctx, b); }
};
} // namespace engine::stages
