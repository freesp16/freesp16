// Which ParamIds the engine implements, and where each one goes (ParamChange carries a ParamId,
// never a string; EngineSetup::resolveParam maps ParamSpec ids to ParamIds at set-up, so the engine never
// includes the registry). Bound on the message thread before the device starts, then read-only:
//   message thread: EngineBridge::post refuses a ParamChange whose id is not bound (the sender learns at once)
//   audio thread:   the FIFO drain switches on the target — O(1) table lookup, no search
// A new engine parameter = one ParamTarget value + one bind() in Engine's constructor + one case in Engine::apply.
#pragma once

#include "core/params/ParamId.h"

#include <array>
#include <cstdint>

namespace engine::bridge
{
enum class ParamTarget : std::uint8_t
{
    None = 0,
    TrackVolume, // "track.volume" (0–127 index, GainLaw) → mixer fader
    // The track strip ((scene << 4) | track scope) and the project-wide mixer values.
    TrackPan,          // "track.pan" −64…64
    TrackSendAmount,   // "track.sendAmount" 0–127
    TrackMute,         // "track.mute" 0/1
    TrackSolo,         // "track.solo" 0/1
    TrackSendChannel,  // "track.sendChannel" 0/1
    TrackMuteMode,     // "track.muteMode" 0 STOP SOUND / 1 STOP STEPS
    MasterVolume,      // "mixer.masterVolume" 0–127
    SendReturnVolume,  // "mixer.sendReturnVolume" 0–127
    HeadphoneVolume,   // "mixer.headphoneVolume" 0–127
    InputSoftLimiter,  // "mixer.softLimiter" 0/1
    MasterLimiter,     // "mixer.masterLimiter" 0/1
    OutputTrim,        // "mixer.outputTrim" 0–1023
    // The voice, envelope, track/scale and pad-routing ids (bound in engine/Engine.cpp). Each family is
    // contiguous in its field enum's order, so field = target − first (static_asserts in Engine.cpp).
    PlaybackFirst,                // engine::voice::PlaybackField (playback.pitch … playback.bypass), 8
    EnvFirst = PlaybackFirst + 8, // engine::modulation::AmpEnvField (env.velocity … track.padVelocity), 10
    TrackFirst = EnvFirst + 10,   // engine::trigger::TrackField (track.mode … scale.area), 7
    PadFirst = TrackFirst + 7,    // engine::trigger::PadField (pads.mode, pads.selected, pads.shift), 3
    PreviewVolume = PadFirst + 3, // "browse.previewVolume" 0–127 (GainLaw) → PreviewVoice gain
    // Every id the FX slot bank routes (engine::fx::FxSlotRoutes: <slot>.type,
    // <slot>.bypass, <slot>.<effect>.<key>[.div], fx.mode, fx.quirkProfile, fx.meteredSlot) → FxSlotBank::onParamChange.
    Fx,
    // seq.swing / recordQuantize / padStartsSequence / offset / record / erase → engine/sequencer/SequencerStage.h
    Sequencer,
    // ribbon.* (core/params/catalogue/ribbon.cpp) → engine/ribbon/RibbonEngine.h onParam.
    Ribbon,
    // afs.* (core/params/catalogue/afs.cpp) → engine/afs/AnalogFilterSlot.h onParam.
    Afs,
    // routing.* (core/params/catalogue/routing.cpp) → engine/mixer/OutputRouterStage.h onParam.
    Routing,
    // sampling.* / live.* (core/params/catalogue/sampling.cpp) → engine/sampling/SamplingStage.h onParam.
    Sampling,
    // scene.* / pattern.* (core/params/catalogue/scene.cpp) → stages::SceneSwitchStage::param.
    SceneQueue,        // "scene.queue" (internal) scene × 16 + pattern (+256 apply BPM), −1 cancel, −2/−3 block/unblock
    PatternQuantize,   // "project.patternQuantize" 0 STEP, 1 BAR, 2 PATTERN
    SceneSwitchMode,   // "scene.switchMode" 0 RESTART, 1 CONTINUE
    SceneBpm,          // "scene.bpm" (scope = scene) 0 = none, else BPM
    SceneBpmPolicy,    // "scene.bpmPolicy" 0 APPLY, 1 ASK, 2 IGNORE
    PatternLength,     // "pattern.length" (scope = (scene << 4) | pattern) steps
    PatternMutes,      // "pattern.mutes" (internal, scope = (scene << 4) | pattern) bit t = track t muted
    PatternSolos,      // "pattern.solos" (internal, same scope) bit t = track t soloed
    // engine::transport::TransportParam order (kParamIds), → TransportStage::setParam.
    TransportTempo,           // "transport.tempo" 20.0–300.0 BPM
    TransportMetronome,       // "transport.metronome" 0/1
    TransportMetronomeVolume, // "transport.metronomeVolume" 0–127
    TransportNudge,           // "transport.nudge" −1/0/+1
    TransportNudgeAmount,     // "transport.nudgeAmount" 1–10 %
    // engine::modulation::LfoField order (lfo.speed … lfo.bypass, kLfoParamIds), → LfoStage::setParam.
    LfoFirst,
    LfoLast = LfoFirst + 9,   // lfo.bypass
    // midi.* (core/params/catalogue/midi.cpp) → engine/midi/MidiEngine.h onParam.
    Midi,
    // The P3 extensions' engine parameters (ext.*: ext.toggle.track → engine/trigger/ext/toggle), consumed
    // in Engine::apply before the dispatch; only posted while their feature flag is on.
    Ext,
    Last = Ext,
};

class ParamBindings
{
public:
    static constexpr std::size_t kMaxParams = 2048; // dense ParamId values (core::ParamRegistry order)

    bool bind(core::ParamId id, ParamTarget target) noexcept
    {
        if (! id.valid() || id.value >= kMaxParams)
            return false;
        targets_[id.value] = target;
        return true;
    }
    ParamTarget find(core::ParamId id) const noexcept
    {
        return id.value < kMaxParams ? targets_[id.value] : ParamTarget::None;
    }

private:
    std::array<ParamTarget, kMaxParams> targets_ {};
};
} // namespace engine::bridge
