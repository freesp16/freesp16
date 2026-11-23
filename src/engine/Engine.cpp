#include "engine/Engine.h"

#include "core/util/GainLaw.h"
#include "core/util/Realtime.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "engine/voice/VoiceSwaps.h"

#include <algorithm>
#include <cstring>
#include <variant>

namespace engine
{
namespace
{
using T = bridge::ParamTarget;
constexpr auto targetAt(T first, int field) noexcept { return static_cast<T>(static_cast<int>(first) + field); }
constexpr int fieldOf(T target, T first) noexcept { return static_cast<int>(target) - static_cast<int>(first); }
static_assert(static_cast<int>(T::EnvFirst) - static_cast<int>(T::PlaybackFirst) == static_cast<int>(voice::PlaybackField::Count));
static_assert(static_cast<int>(T::TrackFirst) - static_cast<int>(T::EnvFirst) == static_cast<int>(modulation::AmpEnvField::Count));
static_assert(static_cast<int>(T::PadFirst) - static_cast<int>(T::TrackFirst) == static_cast<int>(trigger::TrackField::Count));
static_assert(static_cast<int>(T::PreviewVolume) - static_cast<int>(T::PadFirst) == static_cast<int>(trigger::PadField::Count));
static_assert(static_cast<int>(T::LfoLast) - static_cast<int>(T::LfoFirst) + 1 == static_cast<int>(modulation::LfoField::Count));
constexpr std::string_view kPlaybackIds[] = {"playback.pitch", "playback.start", "playback.loopStart",
                                             "playback.length", "playback.loop", "playback.timeStretch",
                                             "playback.reverse", "playback.bypass"};
static_assert(std::size(kPlaybackIds) == static_cast<std::size_t>(voice::PlaybackField::Count));

// The ribbon's PITCH / USER / pitch-bend contributions go into the LFO's ModMatrix (Engine::modMatrix(), applied once
// per block after step 6). The call is resolved at compile time: a no-op while the engine has no matrix, wired
// automatically when it has one (tests/unit/ribbon/test_ribbon_wiring.cpp checks it).
template <class E>
void publishRibbon(E& engine) noexcept
{
    if constexpr (requires { engine.modMatrix(); })
        engine.ribbon().publish(engine.modMatrix());
}
} // namespace

Engine::Engine(const EngineSetup& setup, const EngineOptions& options) : options_(options)
{
    if (setup.resolveParam)
    {
        static constexpr struct
        {
            const char* id;
            T target;
        } kBindings[] = {
            {"track.volume", T::TrackVolume},
            {"track.pan", T::TrackPan},
            {"track.sendAmount", T::TrackSendAmount},
            {"track.mute", T::TrackMute},
            {"track.solo", T::TrackSolo},
            {"track.sendChannel", T::TrackSendChannel},
            {"mixer.masterVolume", T::MasterVolume},
            {"mixer.sendReturnVolume", T::SendReturnVolume},
            {"mixer.headphoneVolume", T::HeadphoneVolume},
            {"mixer.softLimiter", T::InputSoftLimiter},
            {"mixer.masterLimiter", T::MasterLimiter},
            {"mixer.outputTrim", T::OutputTrim},
            {"mixer.muteMode", T::MuteMode},
            {"scene.queue", T::SceneQueue}, // scene.* (scene.cpp)
            {"project.patternQuantize", T::PatternQuantize},
            {"scene.switchMode", T::SceneSwitchMode},
            {"scene.bpm", T::SceneBpm},
            {"scene.bpmPolicy", T::SceneBpmPolicy},
            {"pattern.length", T::PatternLength},
            {"pattern.mutes", T::PatternMutes},
            {"pattern.solos", T::PatternSolos},
        };
        for (const auto& b : kBindings)
            params_.bind(setup.resolveParam(b.id), b.target);
        // The voice, envelope, track/scale and pad-routing parameters, and the preview volume. An id the
        // registry does not know (yet) resolves to an invalid ParamId and stays unbound.
        for (const auto id : kPlaybackIds)
            if (const auto f = voice::playbackFieldOf(id); f != voice::PlaybackField::Count)
                params_.bind(setup.resolveParam(id), targetAt(T::PlaybackFirst, static_cast<int>(f)));
        for (const auto& p : modulation::kAmpEnvParamIds)
            params_.bind(setup.resolveParam(p.id), targetAt(T::EnvFirst, static_cast<int>(p.field)));
        for (const auto& p : trigger::kTrackParamIds)
            params_.bind(setup.resolveParam(p.id), targetAt(T::TrackFirst, static_cast<int>(p.field)));
        for (const auto& p : trigger::kPadParamIds)
            params_.bind(setup.resolveParam(p.id), targetAt(T::PadFirst, static_cast<int>(p.field)));
        params_.bind(setup.resolveParam("browse.previewVolume"), T::PreviewVolume);
        for (int i = 0; i < static_cast<int>(transport::TransportParam::Count); ++i)
            params_.bind(setup.resolveParam(transport::kParamIds[i]),
                         static_cast<T>(static_cast<int>(T::TransportTempo) + i));
        for (const auto& p : modulation::kLfoParamIds)
            params_.bind(setup.resolveParam(p.id), targetAt(T::LfoFirst, static_cast<int>(p.field)));
    }
    // Every type of the built-in registry in every slot (all feature flags: the UI decides what
    // it lists; engine::fx::engineTypes gives the same lists to whoever maps ids to type indices), routed by the
    // ParamSpec ids of the registry. Allocates here and in prepare(), message thread only.
    if (! setup.paramIds.empty())
    {
        const auto& registry = fx::EffectRegistry::builtin();
        const auto flags = fx::allFeatureFlags(registry);
        fxBank_.build(registry, setup.paramIds, flags, setup.fxInternalIndex);
        const auto& routes = fxBank_.routes();
        for (std::size_t i = 0; i < routes.size(); ++i)
        {
            const core::ParamId id {static_cast<std::uint16_t>(i)};
            if (routes.find(id).kind != fx::FxRoute::Kind::None)
                params_.bind(id, T::Fx);
        }
        // Every scene starts from the bank's stored defaults (the FX values of scenes other than the current
        // one are kept here, the bank ignores them).
        fxScenes_.types = static_cast<int>(std::max({fxBank_.insertTypes().size(), fxBank_.sendTypes().size(),
                                                     fxBank_.masterTypes().size()}));
        fxScenes_.stored.assign(static_cast<std::size_t>(scene::kScenes) * fx::kFxSlots *
                                    static_cast<std::size_t>(fxScenes_.types) * 6,
                                0.0f);
        for (int sc = 0; sc < scene::kScenes; ++sc)
        {
            fxScenes_.scene[static_cast<std::size_t>(sc)].scene = static_cast<std::uint8_t>(sc);
            for (int sl = 0; sl < fx::kFxSlots; ++sl)
                for (int t = 0; t < fxScenes_.types; ++t)
                    for (int k = 0; k < 6; ++k)
                        fxScenes_.at(sc, sl, t)[k] = fxBank_.stored(sl, t, k);
        }
    }
    testTone_.setEnabled(options.testTone);
    testToneAllowed_ = options.testTone;
    acceptSwaps(scene::kChainSlot.kind, &scene::destroyChainPlan); // step 5's pattern chain
    acceptSwaps(scene::kArrangementSlot.kind, &scene::destroyArrangementPlan); // step 5's song
    arrangement_.bindParams(setup.resolveParam, params_); // arrangement.play / loop / start
    voice::acceptVoiceSwaps(*this); // SwapSlot::TrackSample / Preview (the sample library posts them)
    voice::acceptSliceSwaps(*this); // SwapSlot::SliceMap (the app posts one table per edit)
    for (int t = 0; t < kTracks; ++t)
        sources_[static_cast<std::size_t>(t)].attach(&env_, t);
    triggerRouter_.attach(sources_, &mixer_);
    triggerRouter_.setSlicePads(&slicePads_); // SLICE pad mode
    // Step 6's patterns (SwapSlot::Pattern objects), engine events, the router's live hits, seq.* params.
    acceptSwaps(SwapSlot::Pattern, &sequencer::destroyPattern);
    sequencer_.attach(swaps_, feeds_, triggerRouter_);
    sequencer_.bindParams(setup.resolveParam, params_);
    ribbon_.bindParams(setup.resolveParam); // ribbon.* → ParamTarget::Ribbon
    for (int i = 0; i < ribbon::RibbonEngine::kParams; ++i)
        params_.bind(ribbon_.paramId(i), T::Ribbon);
    perf_.bindParams(setup.resolveParam, params_); // seq.retrigCount / retrigSpeed / transpose
    afs_.bindParams(setup.resolveParam);           // afs.* → ParamTarget::Afs
    afs_.setAnalogMain(options.analogFilterOnMain); // MASTER route: the unit's own filter after the main DAC
    for (int i = 0; i < afs::AnalogFilterSlot::kParams; ++i)
        params_.bind(afs_.paramId(i), T::Afs);
    router_.bindParams(setup.resolveParam, params_); // routing.* → ParamTarget::Routing
    if (setup.resolveParam) // render.muteLive (REN-005), offered to Engine::apply before the routing ids
    {
        renderMuteId_ = setup.resolveParam("render.muteLive");
        renderOfflineId_ = setup.resolveParam("render.offline");
        params_.bind(renderMuteId_, T::Routing);
        params_.bind(renderOfflineId_, T::Routing);
    }
    sampling_.bindParams(setup.resolveParam, params_); // sampling.* / live.* → ParamTarget::Sampling
    acceptSwaps(SwapSlot::LiveTake, &core::sampling::releaseCaptureHub); // the capture hub (15/15)
    mixer_.setTrimInMaster(false);                   // step 13 trims every output (MIX-023)
    // midi.* → ParamTarget::Midi, the FX page positions of CC 46–51 / 63–68, the objects MIDI drives.
    midi_.bindParams(setup.resolveParam, params_);
    midi_.buildFxPages(fxBank_, setup.resolveParam);
    midiHooks_.attach(*this);
    midi_.attach({&triggerRouter_, &transport_, &sceneSwitch_.switcher, &arrangement_, &ribbon_, &perf_, &midiHooks_,
                  &midiHooks_, &MidiHooks::pitchPreserving, this});
    toggle_.bindParams(setup.resolveParam); // ext.toggle.track → ParamTarget::Ext (TRK-012)
    if (toggle_.paramId().valid())
        params_.bind(toggle_.paramId(), T::Ext);
    preview_.setGain(core::GainLaw::gain(50)); // browse.previewVolume's default (0–127 GainLaw index)
    modSink_.attach(&params_, &m1_, &mixer_);  // the track destinations of the ModMatrix
    prepare(44100.0, 512); // usable before the device prepares it (tests, render)
}

Engine::~Engine()
{
    // Message thread, the device stopped: free what the audio thread no longer owns, then what is still installed.
    bridge_.drainAll();
    if (hasCarry_)
        if (const auto* s = std::get_if<ObjectSwap>(&carry_); s != nullptr && s->object != nullptr)
            if (const auto destroy = swaps_.destroyer(s->slot.kind))
                destroy(s->object);
    hasCarry_ = false;
    swaps_.clear();
}

void Engine::prepare(double sampleRate, int maxDeviceFrames)
{
    (void) maxDeviceFrames; // the adapter serves any period from the fixed block: nothing scales with it
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    midi_.prepare(sampleRate_); // steps 2 + 7
    transport_.prepare(sampleRate_);
    sync_.prepare(sampleRate_);
    sceneSwitch_.prepare(sampleRate_);
    sequencer_.prepare(sampleRate_);
    perf_.prepare(sampleRate_);
    for (auto& t : tracks_)
        t.prepare(sampleRate_);
    send_.prepare(sampleRate_);
    masterFx_.prepare(sampleRate_);
    masterOut_.prepare(sampleRate_);
    router_.prepare(sampleRate_);
    sampling_.prepare(sampleRate_);
    output_.prepare(sampleRate_);
    testTone_.prepare(sampleRate_);
    mixer_.prepare(sampleRate_, kBlockFrames);
    env_.prepare(sampleRate_);
    const int scene = triggerRouter_.scene();
    for (int t = 0; t < kTracks; ++t)
    {
        auto& src = sources_[static_cast<std::size_t>(t)];
        src.prepare(sampleRate_, kBlockFrames);
        src.setPlayback(m1_.playback.at(scene, t));
        env_.setParams(t, m1_.envParams[scene][t], transport_.bpm());
    }
    triggerRouter_.prepare(sampleRate_, kBlockFrames);
    ribbon_.prepare(sampleRate_);
    preview_.prepare(sampleRate_);
    stretch_.prepare(sampleRate_, kBlockFrames); // the selected backend's 16 stretchers (allocates)
    for (int t = 0; t < kTracks; ++t)
        sources_[static_cast<std::size_t>(t)].voice().attachStretch(&stretch_, t);
    fxBank_.prepare(sampleRate_, kBlockFrames); // every effect instance's buffers (message thread, device stopped)
    modMatrix_.prepare(kTracks, modulation::kModDestsPerTrack); // fixed arrays: no allocation
    lfo_.prepare(sampleRate_);
    afs_.prepare(sampleRate_, kBlockFrames); // both processors (message thread, device stopped)
    mixerHooks_ = {};
    buffers_ = {};
    for (auto& b : input_)
        b.clear();
    inputFill_ = 0;
    inputSeen_ = false;
    blockPos_ = kBlockFrames; // the first process() renders a block at once
    feeds_.meters().reset();
    load_.prepare(sampleRate_, kBlockFrames);
}

void Engine::process(float* const* out, int numChannels, int numFrames) noexcept
{
    std::array<float*, 2 * stages::kPorts> pairs {};
    const int n = std::clamp(numChannels, 0, 2 * stages::kPorts);
    for (int c = 0; c < n; ++c)
        pairs[static_cast<std::size_t>(c)] = out[c];
    process(AudioIO {pairs.data(), (n + 1) / 2, nullptr, 0, numFrames, 0});
}

void Engine::process(const AudioIO& io) noexcept
{
    // Top of the callback: this thread is real-time for the call (any allocation aborts under
    // APP_RT_CHECKS) and runs flush-to-zero (FPSCR.FZ: no VFP denormal traps on the A9).
    core::rt::ScopedRealtimeThread realtime;
    core::rt::enableFlushToZero();

    const int frames = io.frames > 0 ? io.frames : 0;
    // The audio clock anchor of this period — engine frame of its first output frame ↔ hostTimeNs.
    midi_.beginPeriod(static_cast<std::int64_t>(blocks_.load(std::memory_order_relaxed)) * kBlockFrames -
                          (kBlockFrames - blockPos_),
                      static_cast<std::int64_t>(io.hostTimeNs), frames);
    const int pairs = std::clamp(io.numOutPorts, 0, stages::kPorts);
    const float* inL = io.numIn > 0 && io.in != nullptr ? io.in[0] : nullptr;
    const float* inR = io.numIn > 1 && io.in != nullptr ? io.in[1] : inL;
    afs_.beginPeriod(io); // FilterSend port and return inputs of this device
    // The logical outputs this device has (a null pair = absent: assigned tracks fold into Main).
    std::uint8_t present = 0;
    for (int p = 0; p < pairs; ++p)
        if (io.out != nullptr && io.out[2 * p] != nullptr && io.out[2 * p + 1] != nullptr)
            present = static_cast<std::uint8_t>(present | 1u << p);
    if (present != portsPresent_)
    {
        portsPresent_ = present;
        router_.setPresent(present);
    }

    int done = 0;
    while (done < frames)
    {
        if (blockPos_ == kBlockFrames)
        {
            // The input block filled during the previous block becomes this block's input (64-frame latency).
            if (inputSeen_)
            {
                buffers_.inputL = input_[static_cast<std::size_t>(inputFill_)].l;
                buffers_.inputR = input_[static_cast<std::size_t>(inputFill_)].r;
                inputFill_ ^= 1;
            }
            else
                buffers_.inputL = buffers_.inputR = nullptr;
            inputSeen_ = false;

            // LoadMeter: cycles per 64-frame block against the block period.
            load_.begin();
            renderBlock();
            load_.end(kBlockFrames);
            blockPos_ = 0;
        }
        const int n = std::min(frames - done, kBlockFrames - blockPos_);
        const auto bytes = sizeof(float) * static_cast<std::size_t>(n);
        auto& fill = input_[static_cast<std::size_t>(inputFill_)];
        afs_.capture(done, blockPos_, n); // the loop's return, one block late like the input
        if (inL != nullptr)
        {
            std::memcpy(fill.l + blockPos_, inL + done, bytes);
            std::memcpy(fill.r + blockPos_, inR + done, bytes);
            inputSeen_ = true;
        }
        else if (inputSeen_) // input vanished mid-block: the rest of the block is silence
        {
            std::memset(fill.l + blockPos_, 0, bytes);
            std::memset(fill.r + blockPos_, 0, bytes);
        }
        for (int c = 0; c < 2 * pairs; ++c)
        {
            float* dst = io.out[c];
            if (dst == nullptr)
                continue; // absent port
            if (const float* src = buffers_.port[static_cast<std::size_t>(c)])
                std::memcpy(dst + done, src + blockPos_, bytes);
            else
                std::memset(dst + done, 0, bytes);
        }
        blockPos_ += n;
        done += n;
    }

    // Step 14, once per device period: playhead + meters (triple buffers), block count for the retire grace period.
    const std::uint64_t blocks = blocks_.load(std::memory_order_relaxed);
    PlayheadFeed p {};
    p.ppq = transport_.ppq();
    p.bpm = midi_.displayTempo(transport_.bpm()); // the followed clock's tempo (SYN-012 display)
    p.playing = transport_.playing();
    sceneSwitch_.publish(p, transport_.ppq()); // scene, pattern, step, pending scene/pattern
    p.preCount = transport_.preCount();
    for (int t = 0; t < kTracks; ++t) // PLY-045: play position 0–1, negative = idle
        p.trackPosition[static_cast<std::size_t>(t)] = sources_[static_cast<std::size_t>(t)].playPosition();
    feeds_.meters().setAfsOverLevel(afs_.overLevel());
    feeds_.publish(blocks, p);
    fxBank_.publish(feeds_.fxWrite()); // the effect panels' feed, same period
    feeds_.fxPublish();
    bridge_.setBlocksRendered(blocks);
}

void Engine::renderBlock() noexcept
{
    BlockContext& ctx = ctx_;
    ctx.sampleRate = sampleRate_;
    ctx.frames = kBlockFrames;
    ctx.blockIndex = blocks_.load(std::memory_order_relaxed);
    ctx.triggerMask = 0;
    ctx.triggerOffset.fill(0);
    buffers_.activeTracks = 0;
    buffers_.auxActive = false;

    drainMessages(ctx);               // 1  UI→engine FIFO + live triggers
    installSwaps();                   //    the installed sample buffers reach the voices (pointer compares)
    sampling_.install(swaps_.as<sampling::CaptureHub>(sampling::kCaptureHubSlot)); // the capture hub
    sampling_.input(buffers_);        //    input stage: soft limiter + meter → the post-limiter input
    midi_.processIn(ctx);             // 2  MIDI in
    transport_.process(ctx, buffers_); // 3
    sync_.process(ctx, buffers_);     // 4
    chain_.setSuspended(arrangement_.suspendsChain()); // 5: song mode is on, the chain waits
    chain_.process(ctx, buffers_.timing, sceneSwitch_.switcher, swaps_.as<scene::ChainPlan>(scene::kChainSlot)); // 5
    arrangement_.process(ctx, buffers_.timing, sceneSwitch_.switcher, transport_,
                         swaps_.as<scene::ArrangementPlan>(scene::kArrangementSlot)); // 5  the next row at a pattern end
    const double tempoBefore = transport_.bpm();                     //    (song mode: "--" keeps this tempo)
    sceneSwitch_.process(ctx, buffers_, transport_, mixer_, feeds_); // 5  timing, mixer scene/masks, scene BPM
    arrangement_.afterSwitch(ctx, buffers_.timing, transport_, tempoBefore); // the row's BPM, the song status
    if (buffers_.timing.sceneChanged)
        switchScene(buffers_.timing.scene); //    the 16 sources, the router and the FX follow, from this block's start
    env_.setTempo(ctx.bpm);            //    sync lengths follow the tempo (recomputed only when it changed)
    sequencer_.process(ctx, buffers_, triggerRouter_); // 6  sequencer steps → router
    perf_.process(ctx, buffers_.timing, triggerRouter_); //    ratchets due this block → router
    ribbon_.collect(ctx, triggerRouter_);              //    strip REPEAT hits (on the transport grid)
    sampling_.schedule(ctx, buffers_.timing, sceneSwitch_.switcher, &triggerRouter_); // live boundaries, loop trigger
    toggle_.flush(triggerRouter_);                     //    releases of dropped Toggle latches
    triggerRouter_.process(ctx);                       //    + live hits: sort, choke, pad modes → track sources
    ribbon_.observe(triggerRouter_.blockEvents(), triggerRouter_.padState()); // the held pads
    publishRibbon(*this);                              //    → the ModMatrix, before it applies the block
    perf_.afterRouter(triggerRouter_);                 //    ratchets choked by a repeat
    sequencer_.applyFxLocks(triggerRouter_, fxBank_);  //    FX1/FX2 step locks or stored values (SEQ-026, FX-079)
    if (buffers_.timing.sceneChanged)                  //    the new scene's bases (sources, strips)
        modMatrix_.touchAll();
    for (const auto& e : triggerRouter_.blockEvents()) //    a trigger's locks changed the track's bases
        modMatrix_.touch(e.track);
    lfo_.process(ctx, triggerRouter_.scene(), triggerRouter_.blockEvents(), modMatrix_); // LFO → matrix
    perf_.publishMods(modMatrix_);                     //    transpose → matrix (SEQ-035)
    afs_.beginBlock(triggerRouter_.blockEvents(), lfo_.lfo(afs_.track() < 0 ? 0 : afs_.track()).value());
    modSink_.beginBlock(triggerRouter_.scene(), ctx.bpm);
    modMatrix_.applyBlock(fxBank_, modSink_);          //    once per block, after the locks, before the voices
    if (sequencer_.seq.takeStartRequest())             //    padStartsSequence (SEQ-047): the hit starts the transport
        transport_.command(TransportCmd {TransportCmd::Play, 0.0});
    midi_.processOut(ctx, buffers_);  // 7  MIDI out
    if (transport_.secondStopThisBlock())
        fxBank_.silenceAndClear(); // FX-036: the second STOP cuts every tail
    fxBank_.beginBlock(ctx, midiTrackMask()); // tempo/trigger context (this block's triggers), FX-089, FX-002
    applyThruBypass();                        // Thru BYPASS → both inserts bypassed (MIX-051)

    // 8  tracks: sources (8a), inserts, strip → main bus
    renderSources(ctx);
    buffers_.activeTracks |= fxBank_.claimTailTracks(buffers_); // silent tracks whose inserts still ring
    buffers_.activeTracks |= afs_.claimTrack(buffers_);          // the AFS track rings / returns
    fxBank_.fillHooks(mixerHooks_);                              // idle slots → null hooks (≈ 0 cycles)
    afs_.wrapHooks(mixerHooks_);                                 // pre-FX → insert → post-FX
    for (int t = 0; t < kTracks; ++t)
        tracks_[static_cast<std::size_t>(t)].process(ctx, buffers_.track[static_cast<std::size_t>(t)],
                                                     (buffers_.activeTracks >> t & 1u) != 0);
    mixTracks(ctx);

    if (preview_.active()) // BROWSE preview → aux, added after the master fader (LIB-011)
    {
        buffers_.aux.clear();
        preview_.render(buffers_.aux.l, buffers_.aux.r);
        buffers_.auxActive = true;
    }

    transport_.renderClick(ctx, buffers_);                  //    metronome → aux (after every aux writer that clears it)
    send_.process(ctx, buffers_, mixer_, mixerHooks_);      // 9  send bus → Send FX → return
    masterFx_.process(ctx, buffers_);                       // 10 (the mixer's masterFx hook runs in step 11's call)
    router_.monitor(buffers_, mixer_);                      //    input monitor → master + headphones
    masterOut_.process(ctx, buffers_, mixer_, mixerHooks_); // 11 master → fader → aux → limiter → meter
    afs_.processMaster(buffers_);                           //    Master route (after the master meter)
    router_.process(ctx, buffers_, mixer_); // 12 main / OUT 3-4 / 5-6 / 7-8 / headphones
    output_.process(ctx, buffers_, router_); // 13 output trim + mute-all → ports
    sampling_.capture(ctx, buffers_, router_.resampleTap(0), router_.resampleTap(1), transport_.startFrame()); // 13 taps
    afs_.routeOutputs(buffers_);            //    FilterSend port while the external loop runs (after 13,
                                            //    which resets the port)
    computeMeters();                   // 13 meters
    afs_.endBlock();                   //    OVER hold, ping
    toggle_.endBlock(sources_, ctx.blockIndex); // latches of tracks that fell silent clear

    blocks_.store(ctx.blockIndex + 1, std::memory_order_relaxed);
}

void Engine::drainMessages(BlockContext& ctx) noexcept
{
    if (hasCarry_)
    {
        if (! apply(carry_, ctx))
            return; // still blocked: keep the order, try again next block
        hasCarry_ = false;
    }
    EngineMessage m;
    for (int budget = options_.maxMessagesPerBlock; budget > 0 && bridge_.pop(m); --budget)
        if (! apply(m, ctx))
        {
            carry_ = m;
            hasCarry_ = true;
            return;
        }
    TriggerNow t;
    while (triggerRouter_.pending() < trigger::TriggerRouter::kMaxLiveEvents && triggers_.pop(t))
        addLiveTrigger(t, ctx);
}

bool Engine::apply(const EngineMessage& m, BlockContext& ctx) noexcept
{
    if (const auto* c = std::get_if<ParamChange>(&m))
    {
        if (sequencer_.onParam(*c)) // seq.*
            return true;
        if (ribbon_.onParam(*c)) // ribbon.*
            return true;
        if (afs_.onParam(*c)) // afs.*
            return true;
        if (perf_.onParam(*c)) // seq.retrigCount / retrigSpeed / transpose
            return true;
        if (renderMuteId_.valid() && c->param == renderMuteId_) // render.muteLive (REN-005)
        {
            router_.router().setMuteAll(c->value >= 0.5f);
            return true;
        }
        if (renderOfflineId_.valid() && c->param == renderOfflineId_) // render.offline: no test tone in a render
        {
            testTone_.setEnabled(testToneAllowed_ && c->value < 0.5f);
            return true;
        }
        if (router_.onParam(*c)) // routing.*
            return true;
        if (sampling_.onParam(*c)) // sampling.* / live.* (observes mixer.softLimiter)
            return true;
        if (arrangement_.onParam(*c)) // arrangement.play / loop / start
            return true;
        if (midi_.onParam(*c)) // midi.*
            return true;
        if (toggle_.onParam(*c, triggerRouter_)) // ext.toggle.track; track.triggerMode of a Toggle track (TRK-012)
            return true;
        if (c->scope <= 0xFF) // a track base may have changed; its modulation is re-applied this block
            modMatrix_.touch(c->scope & 15);
        switch (const T target = params_.find(c->param))
        {
            case T::TrackVolume:
            case T::TrackPan:
            case T::TrackSendAmount:
            case T::TrackMute:
            case T::TrackSolo:
            case T::TrackSendChannel: applyStrip(target, *c); break;
            case T::MasterVolume:
            case T::SendReturnVolume:
            case T::HeadphoneVolume:
            case T::InputSoftLimiter:
            case T::MasterLimiter:
            case T::OutputTrim:
            case T::MuteMode: applyMixerGlobal(target, c->value); break;
            case T::SceneQueue:
                if (arrangement_.refusesSwitch(c->value)) // no manual switch in song mode (ARR-007)
                    break;
                [[fallthrough]];
            case T::PatternQuantize:
            case T::SceneSwitchMode:
            case T::SceneBpm:
            case T::SceneBpmPolicy:
            case T::PatternLength:
            case T::PatternMutes:
            case T::PatternSolos: sceneSwitch_.param(target, *c); break; // 5
            case T::TransportTempo:
            case T::TransportMetronome:
            case T::TransportMetronomeVolume:
            case T::TransportNudge:
            case T::TransportNudgeAmount:
                transport_.setParam(static_cast<transport::TransportParam>(static_cast<int>(target) -
                                                                           static_cast<int>(T::TransportTempo)),
                                    c->value);
                break;
            case T::None: break;
            case T::Fx:
                mirrorFx(*c);
                fxBank_.onParamChange(*c);
                thruBypassForce_ = true; // an FX1/FX2 bypass may have changed under the Thru BYPASS
                break;
            default:
                if (const int f = fieldOf(target, T::LfoFirst); f >= 0 && f < static_cast<int>(modulation::LfoField::Count))
                    lfo_.setParam(static_cast<modulation::LfoField>(f), c->scope, c->value);
                else
                    applyTrackSourceParam(target, *c);
                break;
        }
        return true;
    }
    if (const auto* t = std::get_if<TransportCmd>(&m))
    {
        transport_.command(*t);
        return true;
    }
    if (const auto* s = std::get_if<ObjectSwap>(&m))
    {
        // Install at block start; the replaced object is retired, never freed here (threading.md steps 3–4).
        const void* old = swaps_.get(s->slot);
        if (old == s->object)
            return true;
        if (old != nullptr && ! bridge_.retire(old, s->slot.kind, ctx.blockIndex))
            return false; // retire queue full: keep the old object installed, retry next block
        swaps_.exchange(s->slot, s->object);
        return true;
    }
    if (const auto* t = std::get_if<TriggerNow>(&m))
        return addLiveTrigger(*t, ctx);
    return true;
}

bool Engine::addLiveTrigger(const TriggerNow& t, BlockContext& ctx) noexcept
{
    if (ribbon_.onLive(t, triggerRouter_.padState())) // the strip stream (core/util/ribbon/Ribbon.h pad codes)
        return true;
    if (t.track >= kTracks)
        return true;
    TriggerNow hit = t; // the Toggle latch drops or rewrites live hits of Toggle tracks (TRK-012)
    if (triggerRouter_.pending() < trigger::TriggerRouter::kMaxLiveEvents &&
        ! toggle_.filter(hit, triggerRouter_, ctx.blockIndex))
        return true;
    if (! triggerRouter_.pushLive(hit)) // 64 events this block: the rest waits for the next block, in order
        return false;
    triggersReceived_.store(triggersReceived_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return true;
}

// Playback, envelope, track / scale and pad-routing parameters and the preview
// volume. Track scope = (scene << 4) | track; changes of the current scene reach the voice / envelope at once (the
// sounding note follows pitch and region, the envelope applies new times from its next segment, ENV docs).
void Engine::applyTrackSourceParam(bridge::ParamTarget target, const ParamChange& c) noexcept
{
    const int scene = (c.scope >> 4) & 15, track = c.scope & 15;
    const bool trackScope = c.scope <= 0xFF;
    const bool current = trackScope && scene == triggerRouter_.scene();
    if (target >= T::PlaybackFirst && target < T::EnvFirst)
    {
        const auto f = static_cast<voice::PlaybackField>(fieldOf(target, T::PlaybackFirst));
        if (trackScope && m1_.playback.set(f, c.scope, c.value) && current)
            sources_[static_cast<std::size_t>(track)].setPlayback(m1_.playback.at(scene, track));
    }
    else if (target >= T::EnvFirst && target < T::TrackFirst)
    {
        const auto f = static_cast<modulation::AmpEnvField>(fieldOf(target, T::EnvFirst));
        if (trackScope && modulation::applyParam(m1_.envParams[scene][track], f, c.value) && current)
            env_.setParams(track, m1_.envParams[scene][track], transport_.bpm());
    }
    else if (target >= T::TrackFirst && target < T::PadFirst)
    {
        if (trackScope)
            triggerRouter_.setTrackParam(scene, track, static_cast<trigger::TrackField>(fieldOf(target, T::TrackFirst)),
                                         c.value);
    }
    else if (target >= T::PadFirst && target < T::PreviewVolume)
        triggerRouter_.setPadParam(static_cast<trigger::PadField>(fieldOf(target, T::PadFirst)), c.value);
    else if (target == T::PreviewVolume)
    {
        const int i = c.value <= 0.0f ? 0 : static_cast<int>(c.value + 0.5f);
        preview_.setGain(core::GainLaw::gain(i > 127 ? 127 : i));
    }
}

// 8a: the 16 track sources (voice × envelope, Thru input) into BlockBuffers::track[t], the BROWSE preview
// installed, and the TEMPORARY test tone on track 1 while track 1 has no sample.
void Engine::renderSources(BlockContext& ctx) noexcept
{
    std::uint64_t starts = 0;
    for (const auto& e : triggerRouter_.blockEvents())
    {
        if (e.kind != trigger::TriggerKind::Full)
            continue;
        const auto& src = sources_[static_cast<std::size_t>(e.track)];
        if (src.hasSample() && src.mode() == model::TrackMode::Sample)
            ++starts;
        else if (testToneAllowed_ && (e.source == trigger::TriggerSource::Pad || e.source == trigger::TriggerSource::Midi))
            testTone_.trigger(e.velocity); // TEMPORARY: a live hit on an empty track pings (walking-skeleton behaviour)
    }
    if (starts != 0)
        voiceStarts_.store(voiceStarts_.load(std::memory_order_relaxed) + starts, std::memory_order_relaxed);

    // A queued backend swap, the governor (FX load + last block's MASTER TEMPO voices), the priming
    // budget; the transport tempo and each track's sample BPM reach the voices.
    stretch_.beginBlock(voice::stretch::governorInput(fxBank_));
    const int stretchScene = triggerRouter_.scene();
    for (int t = 0; t < kTracks; ++t)
        sources_[static_cast<std::size_t>(t)].voice().setTempo(ctx.bpm, stretch_.sampleTempo(stretchScene, t));
    std::uint32_t active = 0;
    for (int t = 0; t < kTracks; ++t)
    {
        auto& src = sources_[static_cast<std::size_t>(t)];
        auto& b = buffers_.track[static_cast<std::size_t>(t)];
        if (src.render(ctx, buffers_.inputL, buffers_.inputR, b.l, b.r))
            active |= 1u << t;
    }
    // TEMPORARY test tone: track 1's source while track 1 is an empty Sample track (EngineOptions::testTone).
    const auto& first = sources_[0];
    if (! (active & 1u) && ! first.hasSample() && first.mode() == model::TrackMode::Sample
        && testTone_.process(buffers_.track[0]))
        active |= 1u;
    buffers_.activeTracks = active;
}

std::uint16_t Engine::midiTrackMask() const noexcept
{
    std::uint16_t mask = 0;
    for (int t = 0; t < kTracks; ++t)
        if (sources_[static_cast<std::size_t>(t)].mode() == model::TrackMode::Midi)
            mask = static_cast<std::uint16_t>(mask | (1u << t));
    return mask;
}

// Our choice: a scene switch reaches the track sources, the trigger router and the FX slots at the start of the block
// that holds the boundary (the router applies a block's events in one pass, so the new pattern's step 1 on the
// boundary frame must already find the new scene's tracks; old-pattern events of the same block before the frame —
// a late offset at most — play with the new scene's tracks too). The mixer's strips switch on the frame itself
// (Mixer::setScene). Resident scenes: pointers and parameter blocks by index, nothing is copied or allocated; a
// sounding voice whose sample changes fades out in 2 ms (TrackSource::setSample).
void Engine::switchScene(int scene) noexcept
{
    scene &= 15;
    triggerRouter_.setScene(scene);
    router_.setScene(scene); // the scene's output mode, headphone cue and OUT ASSIGNs
    thruBypassForce_ = true;
    for (int t = 0; t < kTracks; ++t)
    {
        auto& src = sources_[static_cast<std::size_t>(t)];
        src.setPlayback(m1_.playback.at(scene, t));
        env_.setParams(t, m1_.envParams[scene][t], transport_.bpm());
        src.setSample(voice::asSampleBuffer(swaps_.get(voice::trackSampleSlot(scene, t))));
    }
    if (fxScenes_.types == 0)
        return; // no FX bank (EngineSetup without ids)
    auto& state = fxScenes_.scene[static_cast<std::size_t>(scene)];
    state.quirkMask = fxScenes_.quirk;
    for (int sl = 0; sl < fx::kFxSlots; ++sl)
    {
        auto& slot = state.slot[static_cast<std::size_t>(sl)];
        if (const float* v = fxScenes_.at(scene, sl, slot.type))
            for (int k = 0; k < 6; ++k)
                slot.params[k] = v[k];
    }
    fxBank_.setActiveScene(static_cast<std::uint8_t>(scene));
    fxBank_.applyScene(state);
}

// The FX ParamChanges of every scene into fxScenes_ (the same routes as FxSlotBank::onParamChange).
void Engine::mirrorFx(const ParamChange& c) noexcept
{
    if (fxScenes_.types == 0)
        return;
    const fx::FxRoute& r = fxBank_.routes().find(c.param);
    const int iv = static_cast<int>(c.value < 0.0f ? c.value - 0.5f : c.value + 0.5f);
    int sc = 0, sl = 0;
    switch (r.kind)
    {
        case fx::FxRoute::Kind::None:
        case fx::FxRoute::Kind::Meter: return;
        case fx::FxRoute::Kind::Quirk:
            fxScenes_.quirk = c.value >= 0.5f ? fx::kQuirkProfileReference : fx::kQuirkProfileSixteen;
            router_.setReferenceQuirk(c.value >= 0.5f); // the headphone selector quirk (F209)
            return;
        case fx::FxRoute::Kind::Mode:
            if (c.scope < scene::kScenes)
                fxScenes_.scene[c.scope].fxMode = static_cast<std::uint8_t>(iv < 0 ? 0 : iv);
            return;
        default: break;
    }
    switch (r.slotKind)
    {
        case fx::SlotKind::Insert:
            if (c.scope > 0xFF)
                return;
            sc = c.scope >> 4;
            sl = fx::insertSlot(c.scope & 15, r.unit);
            break;
        case fx::SlotKind::Send:
        case fx::SlotKind::Master:
            if (c.scope >= scene::kScenes)
                return;
            sc = c.scope;
            sl = r.slotKind == fx::SlotKind::Send ? fx::kSendSlot : fx::kMasterSlot;
            break;
    }
    auto& slot = fxScenes_.scene[static_cast<std::size_t>(sc)].slot[static_cast<std::size_t>(sl)];
    switch (r.kind)
    {
        case fx::FxRoute::Kind::SlotType:
            slot.type = static_cast<std::int8_t>(iv - 1 < -1 ? -1 : (iv - 1 >= fxScenes_.types ? -1 : iv - 1));
            break;
        case fx::FxRoute::Kind::SlotBypass: slot.bypass = c.value >= 0.5f ? 1 : 0; break;
        case fx::FxRoute::Kind::Knob:
            if (float* v = fxScenes_.at(sc, sl, r.type); v != nullptr && r.index >= 0 && r.index < 6)
                v[r.index] = c.value < 0.0f ? 0.0f : (c.value > 1.0f ? 1.0f : c.value);
            break;
        case fx::FxRoute::Kind::Option:
            if (r.type == slot.type && r.index >= 0 && r.index < fx::kSlotOptions) // the current type's options
            {
                slot.options[r.index] = static_cast<std::int16_t>(iv);
                slot.optionMask = static_cast<std::uint8_t>(slot.optionMask | (1u << r.index));
            }
            break;
        default: break;
    }
}

// Thru BYPASS (MIX-051, FX-045, OQ-20): a Thru track with BYPASS on runs around both inserts — their FX bypass is
// held on (the shell's click-free crossfade); off again, each insert gets back its stored per-scene bypass. Re-applied
// when the mask, a track mode, an FX bypass or the scene changes; one compare per block otherwise.
void Engine::applyThruBypass() noexcept
{
    const std::uint16_t want = router_.thruBypassMask();
    std::uint16_t mask = 0;
    for (std::uint32_t w = want, t = 0; w != 0; ++t, w >>= 1)
        if ((w & 1u) != 0 && sources_[t].mode() == model::TrackMode::Thru)
            mask = static_cast<std::uint16_t>(mask | 1u << t);
    const bool force = router_.thruBypassChanged() || thruBypassForce_;
    thruBypassForce_ = false;
    if (! force && mask == thruBypassApplied_)
        return;
    const std::uint32_t changed = force ? 0xFFFFu : static_cast<std::uint32_t>(mask ^ thruBypassApplied_);
    thruBypassApplied_ = mask;
    if (fxScenes_.types == 0)
        return; // no FX bank (EngineSetup without ids)
    const auto& state = fxScenes_.scene[static_cast<std::size_t>(triggerRouter_.scene() & 15)];
    for (std::uint32_t c = changed, t = 0; c != 0; ++t, c >>= 1)
        if ((c & 1u) != 0)
            for (int u = 0; u < 2; ++u)
            {
                const int sl = fx::insertSlot(static_cast<int>(t), u);
                const bool stored = state.slot[static_cast<std::size_t>(sl)].bypass != 0;
                fxBank_.slot(sl).setBypass(stored || ((mask >> t) & 1u) != 0);
            }
}

void Engine::installSwaps() noexcept
{
    const int scene = triggerRouter_.scene();
    for (int t = 0; t < kTracks; ++t) // a changed buffer fades the sounding note out (2 ms), later hits play the new one
        sources_[static_cast<std::size_t>(t)].setSample(voice::asSampleBuffer(swaps_.get(voice::trackSampleSlot(scene, t))));
    for (int t = 0; t < kTracks; ++t) // the current scene's slice tables (a swap = a pointer change)
    {
        const voice::SliceMap* map = voice::asSliceMap(swaps_.get(voice::sliceMapSlot(scene, t)));
        sources_[static_cast<std::size_t>(t)].setSliceMap(map);
        slicePads_.setMap(t, map);
    }
    preview_.install(voice::asSampleBuffer(swaps_.get(voice::previewSlot())));
}

// Track strip parameters (scope = (scene << 4) | track). The ramps are the spec's fixed lengths (MIX-002: 100 samples,
// mute 128), so ParamChange::rampSamples is not used here; the change starts at the block's first frame.
void Engine::applyStrip(bridge::ParamTarget target, const ParamChange& c) noexcept
{
    if (c.scope > 0xFF)
        return; // not a track scope
    const int scene = c.scope >> 4, track = c.scope & 0xF;
    mixer::StripParams p = mixer_.strip(scene, track);
    const float v = c.value;
    const auto index = [v](int lo, int hi) {
        const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
        return i < lo ? lo : (i > hi ? hi : i);
    };
    switch (target)
    {
        case bridge::ParamTarget::TrackVolume: p.volume = static_cast<std::uint8_t>(index(0, 127)); break;
        case bridge::ParamTarget::TrackPan: p.pan = static_cast<std::int8_t>(index(-64, 64)); break;
        case bridge::ParamTarget::TrackSendAmount: p.send = static_cast<std::uint8_t>(index(0, 127)); break;
        case bridge::ParamTarget::TrackMute: p.mute = v >= 0.5f; break;
        case bridge::ParamTarget::TrackSolo: p.solo = v >= 0.5f; break;
        case bridge::ParamTarget::TrackSendChannel: p.sendChannel = static_cast<std::uint8_t>(index(0, 1)); break;
        default: return;
    }
    mixer_.setStrip(scene, track, p, 0);
}

void Engine::applyMixerGlobal(bridge::ParamTarget target, float value) noexcept
{
    using G = mixer::GlobalParam;
    switch (target)
    {
        case bridge::ParamTarget::MasterVolume: mixer_.setGlobal(G::MasterVolume, value); break;
        case bridge::ParamTarget::SendReturnVolume: mixer_.setGlobal(G::SendReturnVolume, value); break;
        case bridge::ParamTarget::HeadphoneVolume:
            mixer_.setGlobal(G::HeadphoneVolume, value);
            router_.setHeadphoneVolume(value); // MIX-041
            break;
        case bridge::ParamTarget::InputSoftLimiter: mixer_.setGlobal(G::SoftLimiter, value); break;
        case bridge::ParamTarget::MasterLimiter: mixer_.setGlobal(G::MasterLimiter, value); break;
        case bridge::ParamTarget::MuteMode: mixer_.setGlobal(G::MuteMode, value); break;
        case bridge::ParamTarget::OutputTrim:
            mixer_.setGlobal(G::OutputTrim, value);
            router_.setOutputTrim(value); // every logical output (MIX-023)
            break;
        default: break;
    }
}

void Engine::mixTracks(BlockContext&) noexcept
{
    // 8b: the strips (pan → mute ramp → send tap → fader) and the main/send bus sums. A single unity track
    // reaches the main bus bit-exactly (NFR-030).
    mixer_.processTracks(buffers_, mixerHooks_);
}

void Engine::computeMeters() noexcept
{
    // The mixer measured its 18 meters in the same pass as the gains (NEON peak / sum of squares): copy, no second pass.
    auto& m = feeds_.meters();
    const auto& mb = mixer_.meters();
    m.addMasterValues(mb.peak[mixer::kMeterMaster][0], mb.peak[mixer::kMeterMaster][1], mb.sumSq[mixer::kMeterMaster][0],
                      mb.sumSq[mixer::kMeterMaster][1], kBlockFrames);
    for (int t = 0; t < kTracks; ++t)
        m.addTrackPeak(t, mb.peak[t][0], mb.peak[t][1]);
    m.addSendPeak(mb.peak[mixer::kMeterSend][0], mb.peak[mixer::kMeterSend][1]);
    if (buffers_.inputL != nullptr)
        m.addInput(buffers_.inputL, buffers_.inputR, kBlockFrames);
}

// The MIDI stage's view of the engine (audio thread, per incoming CC / note).
void Engine::MidiHooks::attach(Engine& e) noexcept
{
    engine = &e;
    syncOption.fill(-1);
    const auto types = e.fxBank_.insertTypes();
    for (std::size_t t = 0; t < types.size() && t < syncOption.size(); ++t)
        if (types[t] != nullptr)
            for (std::size_t i = 0; i < types[t]->options.size() && i < static_cast<std::size_t>(fx::kSlotOptions); ++i)
            {
                const std::string_view o = types[t]->options[i];
                if (o.size() >= 5 && o.substr(o.size() - 5) == ".sync")
                    syncOption[t] = static_cast<std::int8_t>(i);
            }
}

int Engine::MidiHooks::insertType(int track, int unit) const noexcept
{
    if (engine == nullptr || engine->fxScenes_.types == 0)
        return -1;
    const auto& state = engine->fxScenes_.scene[static_cast<std::size_t>(engine->triggerRouter_.scene() & 15)];
    return state.slot[static_cast<std::size_t>(fx::insertSlot(track & 15, unit & 1))].type;
}

bool Engine::MidiHooks::insertSync(int track, int unit, int type) const noexcept
{
    if (engine == nullptr || type < 0 || type >= static_cast<int>(syncOption.size()) || syncOption[static_cast<std::size_t>(type)] < 0)
        return false;
    const int i = syncOption[static_cast<std::size_t>(type)];
    const auto& slot = engine->fxScenes_.scene[static_cast<std::size_t>(engine->triggerRouter_.scene() & 15)]
                           .slot[static_cast<std::size_t>(fx::insertSlot(track & 15, unit & 1))];
    return (slot.optionMask >> i & 1u) != 0 && slot.options[i] != 0;
}

void Engine::MidiHooks::applyParam(const ParamChange& c) noexcept
{
    if (engine != nullptr)
        engine->apply(EngineMessage {c}, engine->ctx_);
}

bool Engine::MidiHooks::pitchPreserving(const void* e, int track) noexcept
{
    const auto* engine = static_cast<const Engine*>(e);
    return engine != nullptr &&
           engine->m1_.playback.at(engine->triggerRouter_.scene(), track).stretch == model::StretchMode::MasterTempo;
}

void OfflineDriver::run(Engine& engine, int frames, std::span<float* const> out)
{
    std::array<float*, 2 * stages::kPorts> ptrs {};
    const int channels = static_cast<int>(std::min<std::size_t>(out.size(), ptrs.size()));
    const int period = periodFrames > 0 ? periodFrames : kBlockFrames;
    core::rt::ScopedRealtimeThread realtime; // the engine runs under the real-time contract, as on a device
    for (int done = 0; done < frames;)
    {
        const int n = std::min(period, frames - done);
        for (int c = 0; c < channels; ++c)
            ptrs[static_cast<std::size_t>(c)] = out[static_cast<std::size_t>(c)] != nullptr ? out[static_cast<std::size_t>(c)] + done : nullptr;
        engine.process(AudioIO {ptrs.data(), (channels + 1) / 2, nullptr, 0, n, 0});
        done += n;
    }
}
} // namespace engine
