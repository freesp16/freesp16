// engine::Engine — the real-time engine frame (spec 01-architecture/audio-engine.md, threading.md). What
// createEngine() returns.
//
//   device period (any size, 32…512 frames and beyond, not necessarily a multiple of 64)
//     └─ process(AudioIO): FTZ + ScopedRealtimeThread, then the sample-accurate block adapter:
//          output: frames are served from the current 64-frame block; a new block is rendered when it is used up
//          input:  frames are collected into the next block's input (a constant 64-frame input latency)
//        renderBlock(), fixed 64 frames, in the order of audio-engine.md "Per-block processing order":
//           1 drain UI→engine SPSC (ParamChange, TransportCmd, ObjectSwap install + retire, TriggerNow)
//             + TriggerSink (MPSC) live triggers
//           2 MIDI in · 3 transport · 4 sync · 5 scene switch · 6 sequencer / trigger router · 7 MIDI out
//           8 16 tracks × (source → insert 1 → insert 2 → strip) → main bus · 9 send · 10 master FX · 11 master out
//          12 output router · 13 output volume + meters (NEON peak / sum of squares)
//       14 publish playhead + meters once per period (triple buffers)
//
// Ownership and threads: constructed, prepared and destroyed on the message thread (prepare allocates nothing — every
// buffer is a fixed-size member, the engine itself is the one allocation); process() on the audio thread only. Objects
// installed by ObjectSwap are never freed on the audio thread: the old pointer goes to the RetireQueue and the message
// thread frees it in EngineBridge::retirePending() after >= 2 blocks (threading.md "Object swap protocol").
#pragma once

#include "engine/EngineApi.h"
#include "engine/LoadMeter.h"
#include "engine/afs/AnalogFilterSlot.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/bridge/RealtimeBridge.h"
#include "engine/bridge/SwapTable.h"
#include "engine/feeds/Feeds.h"
#include "engine/fx/FxSlotBank.h"
#include "engine/midi/MidiEngine.h"
#include "engine/mixer/Mixer.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/stages/Stages.h"
#include "engine/stages/TestToneStage.h"
#include "engine/modulation/AmpEnvelopeBank.h"
#include "engine/modulation/AmpEnvelopeParams.h"
#include "engine/modulation/LfoStage.h"
#include "engine/modulation/ModMatrix.h"
#include "engine/ribbon/RibbonEngine.h"
#include "engine/sampling/SamplingStage.h"
#include "engine/scene/PatternChain.h"
#include "engine/scene/SectionSequencer.h"
#include "engine/sequencer/perf/PerfStage.h"
#include "engine/voice/stretch/StretcherBank.h"
#include "engine/trigger/SourceSet.h"
#include "engine/trigger/TrackSource.h"
#include "engine/trigger/TriggerRouter.h"
#include "engine/trigger/ext/toggle/ToggleLatch.h"
#include "engine/voice/PlaybackParams.h"
#include "engine/voice/PreviewVoice.h"
#include "engine/voice/SliceMapPads.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace engine
{
struct EngineOptions
{
    // TEMPORARY (kept for the smoke checks): the 440 Hz −12 dBFS tone on track 1. It renders only
    // while track 1 is a Sample track with no sample installed in SwapSlot::TrackSample(scene, 0) and its own source
    // is silent; a live hit on a track without a sample adds the 880 Hz ping (so a pad press is audible).
    bool testTone = true;
    int maxMessagesPerBlock = 512;     // bound on step 1's drain; the rest waits for the next block, in order
};

class Engine final : public AudioEngine
{
public:
    static constexpr int blockSize = kBlockFrames;

    explicit Engine(const EngineSetup& setup = {}, const EngineOptions& options = {});
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    // ---- AudioProcessor ----
    void prepare(double sampleRate, int maxDeviceFrames) override; // message thread, device stopped
    void process(const AudioIO& io) noexcept override;              // audio thread, any frame count
    // Planar convenience (tests, render): out[c] for c < numChannels in OutPort pair order (0/1 = Main L/R, …).
    void process(float* const* out, int numChannels, int numFrames) noexcept;

    // ---- AudioEngine ----
    std::string_view name() const noexcept override { return "engine"; }
    EngineBridge& bridge() noexcept override { return bridge_; }
    TriggerSink& triggers() noexcept override { return triggers_; }
    std::uint64_t blocksRendered() const noexcept override { return blocks_.load(std::memory_order_relaxed); }
    EngineLoad load() const noexcept override
    {
        const auto s = load_.snapshot();
        return {s.avgCycles, s.p95Cycles, s.maxCycles, s.loadPercent, s.blocks, s.overruns};
    }

    // ---- set-up (message thread, before the device starts) ----
    // Accept ObjectSwaps of `kind`; `destroy` frees a retired object (called on the message thread only).
    void acceptSwaps(SwapSlot::Kind kind, bridge::DestroyFn destroy) noexcept { swaps_.accept(kind, destroy); }
    // The object installed in a slot (audio thread from the stages; tests after the device stopped).
    const void* installed(const SwapSlot& slot) const noexcept { return swaps_.valid(slot) ? swaps_.get(slot) : nullptr; }
    double sampleRate() const noexcept { return sampleRate_; }
    // Live triggers (TriggerSink + posted TriggerNow) the audio thread has taken so far (diagnostics, tests).
    std::uint64_t triggersReceived() const noexcept { return triggersReceived_.load(std::memory_order_relaxed); }
    // Voice starts (Full triggers routed to a Sample track that has a sample) so far (diagnostics, tests).
    std::uint64_t voiceStarts() const noexcept { return voiceStarts_.load(std::memory_order_relaxed); }
    // Step 6 / 8a objects (tests and benches drive them after the device stopped; the sequencer and the touch strip
    // reach them through the stages).
    trigger::TriggerRouter& triggerRouter() noexcept { return triggerRouter_; }
    const trigger::TrackSource& trackSource(int track) const noexcept { return sources_[static_cast<std::size_t>(track & 15)]; }
    const modulation::AmpEnvelopeBank& envelopes() const noexcept { return env_; }
    // The touch strip (modes, HOLD, REPEAT hits, the ribbon's ModMatrix contributions); tests and benches
    // after the device stopped.
    ribbon::RibbonEngine& ribbon() noexcept { return ribbon_; }
    // Ratchets and transpose (step 6, after the sequencer).
    sequencer::perf::PerfStage& perf() noexcept { return perf_; }
    // Song mode (step 5; the MIDI stage reads songPosition16th / calls locate16th).
    scene::SectionSequencer& arrangement() noexcept { return arrangement_; }
    // The FX slot bank's 34 slots: built from EngineSetup::paramIds (empty = no routes, every slot Off).
    fx::FxSlotBank& fxBank() noexcept { return fxBank_; }
    const fx::FxSlotBank& fxBank() const noexcept { return fxBank_; }
    // The 16 track LFOs and the one per-block modulation sum (the ratchets / transpose and the touch strip set their
    // sources in it from their stages, before Engine applies it; tests and benches after the device stopped).
    modulation::ModMatrix& modMatrix() noexcept { return modMatrix_; }
    const modulation::LfoStage& lfo() const noexcept { return lfo_; }
    // The analog-filter slot (master / track pre- or post-FX; OVER lamp, external-loop latency for the app).
    afs::AnalogFilterSlot& afs() noexcept { return afs_; }
    const afs::AnalogFilterSlot& afs() const noexcept { return afs_; }
    // Steps 12–13 (output router, headphone cue, input monitor, output trim; the resample tap for sampling and the
    // render: routing().resampleTap(ch)); tests and benches after the device stopped.
    stages::OutputRouterStage& routing() noexcept { return router_; }
    const stages::OutputRouterStage& routing() const noexcept { return router_; }
    // Steps 2 and 7 (MIDI in, clock follower, MIDI tracks, clock out) and its hub (core/util/midi: the
    // port threads and the sender); tests and benches after the device stopped.
    ::engine::midi::MidiEngine& midi() noexcept { return midi_; }
    const ::engine::midi::MidiEngine& midi() const noexcept { return midi_; }
    // The input stage (soft limiter, meter), the capture ring writer, sampling and live sampling (the
    // hub arrives as ObjectSwap{SwapSlot::LiveTake, 15, 15}); tests and benches after the device stopped.
    sampling::SamplingStage& sampling() noexcept { return sampling_; }
    const sampling::SamplingStage& sampling() const noexcept { return sampling_; }
    // Cycles per 64-frame block of renderBlock() against the block period; snapshot() from the
    // message thread (lock-free).
    const LoadMeter& loadMeter() const noexcept { return load_; }
    // The 16 MASTER TEMPO stretchers (audio.stretchBackend → select(), sample BPMs → setSampleTempo();
    // message thread), the governor and the priming budget (audio thread, step 8a).
    voice::stretch::StretcherBank& stretchBank() noexcept { return stretch_; }
    // Constant input → output latency of the block adapter (frames).
    static constexpr int inputLatencyFrames() noexcept { return kBlockFrames; }

private:
    void renderBlock() noexcept;
    void drainMessages(BlockContext& ctx) noexcept;
    bool apply(const EngineMessage& m, BlockContext& ctx) noexcept; // false = cannot be applied this block (carried)
    bool addLiveTrigger(const TriggerNow& t, BlockContext& ctx) noexcept;
    void mixTracks(BlockContext& ctx) noexcept;
    void applyStrip(bridge::ParamTarget target, const ParamChange& c) noexcept;
    void applyMixerGlobal(bridge::ParamTarget target, float value) noexcept;
    void applyTrackSourceParam(bridge::ParamTarget target, const ParamChange& c) noexcept; // playback, env, track, pads
    void renderSources(BlockContext& ctx) noexcept; // 8a
    void installSwaps() noexcept;                   // 1  installed SampleBuffers → voices / preview
    void computeMeters() noexcept;
    std::uint16_t midiTrackMask() const noexcept; // FX-002: the inserts of MIDI tracks are disabled
    void switchScene(int scene) noexcept;          // sources, router and FX follow a scene switch (step 5)
    void mirrorFx(const ParamChange& c) noexcept;  // the FX values of every scene, for switchScene
    void applyThruBypass() noexcept;               // Thru BYPASS = both inserts bypassed (MIX-051)
    // What the MIDI stage needs of the engine — incoming CCs take the message path (apply), the FX slots'
    // current type and SYNC option (CC 46–51 / 63–68), MASTER TEMPO per track (MID-021).
    struct MidiHooks final : ::engine::midi::FxCcContext, ::engine::midi::ParamSink
    {
        Engine* engine = nullptr;
        std::array<std::int8_t, ::engine::midi::CcMap::kMaxTypes> syncOption {}; // per insert type: its "sync" option, −1 none
        void attach(Engine& e) noexcept;
        int insertType(int track, int unit) const noexcept override;
        bool insertSync(int track, int unit, int type) const noexcept override;
        void applyParam(const ParamChange& c) noexcept override;
        static bool pitchPreserving(const void* engine, int track) noexcept;
    } midiHooks_;

    EngineOptions options_;
    double sampleRate_ = 44100.0;

    // Message-thread configuration, read-only once running.
    bridge::ParamBindings params_;
    bridge::SwapTable swaps_;

    // Audio → UI, UI → audio.
    feeds::EngineFeeds feeds_;
    bridge::RealtimeBridge bridge_ {params_, swaps_, feeds_};
    bridge::TriggerQueue triggers_;
    EngineMessage carry_ {};     // a popped message that could not be applied yet (step 1 keeps the order)
    bool hasCarry_ = false;


    // Stages, in block order (audio-engine.md); plain members, called directly.
    ::engine::midi::MidiEngine midi_; // 2 + 7
    stages::TransportStage transport_;
    stages::SyncFollowerStage sync_;
    stages::SceneSwitchStage sceneSwitch_;
    scene::PatternChainPlayer chain_; // 5: queues the chain's next pattern at each pattern end
    scene::SectionSequencer arrangement_; // 5: song mode — the next row at each row's last pattern end
    stages::SequencerStage sequencer_;
    sequencer::perf::PerfStage perf_;                 // 6: ratchets → router, transpose → pitch
    std::array<stages::TrackStage, kTracks> tracks_ {};
    stages::SendStage send_;
    stages::MasterFxStage masterFx_;
    stages::MasterOutStage masterOut_;
    stages::OutputRouterStage router_; // 12–13
    stages::OutputStage output_;
    sampling::SamplingStage sampling_; // input stage (after 1), live boundaries (6), capture (13)
    std::uint8_t portsPresent_ = 0xFF;   // bit = OutPort the device has (from AudioIO, per period)
    std::uint16_t thruBypassApplied_ = 0; // tracks whose inserts the Thru BYPASS holds bypassed
    bool thruBypassForce_ = true;        //   the FX bypass state changed (param / scene): re-apply the override
    stages::TestToneStage testTone_; // TEMPORARY: track 1 while it has no sample (EngineOptions::testTone)
    bool testToneAllowed_ = true;    // EngineOptions::testTone: off = no tone and no ping, render.offline or not
    // Step 6 router and step 8a sources (voice × envelope lane t / Thru input) with their
    // [scene][track] parameter blocks (a scene switch is an index change).
    trigger::SourceSet m1_; // out-of-line constructor (GCC 11 build time, see SourceSet.h)
    trigger::TriggerRouter& triggerRouter_ = m1_.router;
    std::array<trigger::TrackSource, kTracks>& sources_ = m1_.sources;
    modulation::AmpEnvelopeBank& env_ = m1_.env;
    voice::PreviewVoice& preview_ = m1_.preview;
    ribbon::RibbonEngine ribbon_;    // step 1 (ribbon.*, the strip stream) and step 6 (REPEAT, held pads)
    mixer::Mixer mixer_;             // strips (8b), send (9), master (10–11), meters (13)
    mixer::Hooks mixerHooks_ {};     // the FX bank's hooks for this block (FxSlotBank::fillHooks; idle slots null)
    fx::FxSlotBank fxBank_;          // inserts (8b), Send FX (9), Master FX (10)
    modulation::LfoStage lfo_;       // 16 track LFOs → modMatrix_ (after step 6, before step 8)
    modulation::ModMatrix modMatrix_;
    modulation::TrackModSink modSink_;
    afs::AnalogFilterSlot afs_;      // hooks into step 8b (track pre/post FX) and step 11 (master), port 12
    // Resident FX values of the 16 scenes (FX-004; the bank holds the current scene's only): every Fx ParamChange is
    // mirrored here whatever its scene, and a scene switch hands the bank that scene's FxSlotState (applyScene).
    struct FxScenes
    {
        std::array<fx::FxSlotState, scene::kScenes> scene {};
        std::vector<float> stored; // [scene][slot][type][k]: the knob values of every type (sized in the constructor)
        int types = 0;
        std::uint32_t quirk = 0;
        float* at(int sc, int slot, int type) noexcept
        {
            return type < 0 || type >= types ? nullptr
                                             : stored.data() + ((static_cast<std::size_t>(sc) * fx::kFxSlots +
                                                                 static_cast<std::size_t>(slot)) * static_cast<std::size_t>(types) +
                                                                static_cast<std::size_t>(type)) * 6;
        }
    } fxScenes_;

    LoadMeter load_; // around renderBlock()
    voice::stretch::StretcherBank stretch_; // one stretcher per track voice (lane = track)
    voice::SlicePadLocks slicePads_; // SLICE pad mode → slice-number locks
    stages::BlockBuffers buffers_ {};
    BlockContext ctx_ {};

    // Block adapter (device period ↔ 64-frame blocks).
    int blockPos_ = kBlockFrames;                    // frames of the current block already played
    std::array<stages::StereoBlock, 2> input_ {};    // [fill, render]: input frames of the next / current block
    int inputFill_ = 0;
    bool inputSeen_ = false;
    std::atomic<std::uint64_t> blocks_ {0};
    std::atomic<std::uint64_t> triggersReceived_ {0};
    std::atomic<std::uint64_t> voiceStarts_ {0};
    core::ParamId renderMuteId_ {}; // render.muteLive → router_.router().setMuteAll (REN-005)
    core::ParamId renderOfflineId_ {}; // render.offline → the TEMPORARY test tone off (offline renders)
    trigger::ext::ToggleLatch toggle_; // TRK-012 (ext.trk.toggle): latches pad hits before the router

public:
    // The Toggle trigger mode's latch (tests after the device stopped).
    const trigger::ext::ToggleLatch& toggleLatch() const noexcept { return toggle_; }
};

// Runs an Engine without a device (tests, offline render): `frames` frames in device periods of `periodFrames`
// on the calling thread, marked real-time (any allocation aborts under APP_RT_CHECKS). `out` holds planar channels in
// OutPort pair order (out[0]/out[1] = Main L/R, …), each at least `frames` long; fewer channels = absent ports. The
// engine must be prepared (prepare(rate, >= periodFrames)).
class OfflineDriver
{
public:
    int periodFrames = 256;
    void run(Engine&, int frames, std::span<float* const> out);
};
} // namespace engine
