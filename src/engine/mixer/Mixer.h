// engine::mixer — the track mixer, send bus, master and meters (spec 01-architecture/audio-engine.md "Track strip",
// "Buses"; 02-features/mixer-and-routing.md MIX-001…MIX-023, MIX-060; research docs/effects/signal-flow.md and the MIT
// models tools/fx/track_mix.py, bus_mix.py).
//
//   per track (steps 8b): source → insert hook → pan (linear balance) → mute ramp (128) → send tap (pre-fader,
//                         GainLaw × 0.5) → fader (GainLaw) → main bus / send bus; ramps 100 samples (MIX-002)
//   send (step 9):        send bus → Send FX hook (null = Send FX Off = the return is silence) → return fader
//   master (steps 10–11): main bus → Master FX hook → + send return (after the master FX) → master fader → + aux
//                         (preview, metronome) → master soft limiter (optional, default off) → meter →
//                         output trim (MIX-060) → BlockBuffers::master
//
// Threads: prepare() and the constructor on the message thread; every other member on the audio thread (the engine's
// FIFO drain calls the setters at block start, the trigger router calls applyLocks/hardMute before process()).
// Performance: 16 strips + buses + 18 meters ≤ 45 µs per block on the A9 — structure of arrays
// over a strip's four gains (one 4-lane ramp vector, one vld4q per 4 frames), NEON kernels with scalar references
// (MixerKernels.h), silent tracks skipped with their ramps kept, idle unity strips bit-transparent (NFR-030), no
// virtual call per sample (the hooks are called once per block).
//
// Contract notes: the mixer reads the track buffers from stages::BlockBuffers, not from the BlockContext.
// `applyLocks` takes the LockSet type as a template parameter; it is call-compatible with
// `applyLocks(int, const trigger::LockSet*, int)`.
#pragma once

#include "engine/EngineApi.h"
#include "engine/mixer/MixerKernels.h"
#include "engine/mixer/Ramps.h"
#include "engine/stages/BlockBuffers.h"

#include <array>
#include <cstdint>

namespace engine::mixer
{
constexpr int kScenes = 16;
constexpr int kMeters = kTracks + 2; // 16 tracks, send return, master
constexpr int kMeterSend = kTracks;
constexpr int kMeterMaster = kTracks + 1;

// Lock slots of the mixer's parameters (core::ParamIndex, sequencer.md "Lockable parameters").
constexpr int kLockVolume = 0, kLockPan = 5, kLockSend = 6, kLockMute = 10;

class BusInsert
{
public:
    virtual ~BusInsert() = default;
    virtual void process(float* l, float* r) noexcept = 0; // one 64-frame block in place
};

struct Hooks // null = no FX; the FX slot bank installs them (virtual per block only)
{
    std::array<BusInsert*, kTracks> insert {}; // track: source → insert → pan
    BusInsert* sendFx = nullptr;               // null = Send FX Off = the return is silence (audio-engine.md)
    BusInsert* masterFx = nullptr;             // null = pass-through
};

struct StripParams // per [scene][track]
{
    std::uint8_t volume = 100, send = 0;
    std::int8_t pan = 0;
    bool mute = false, solo = false;
    std::uint8_t sendChannel = 0; // MIX-011: 1 = analog-filter send; the mixer feeds the send bus either way
};

// Per block. peak = max |x|, sumSq = Σ x² of the block's 64 frames, per channel; clip bit m = peak of meter m ≥ 1.0.
// Tracks are metered post-fader, the send return after its fader, the master after the limiter and before the trim.
struct MeterBlock
{
    float peak[kMeters][2];
    float sumSq[kMeters][2];
    std::uint32_t clip;
};

enum class GlobalParam : std::uint8_t
{
    MasterVolume,     // 0–127, GainLaw (100 = 0 dB)
    SendReturnVolume, // 0–127, GainLaw (sendTrackVolume, 100 = 0 dB)
    HeadphoneVolume,  // 0–127, kept for the output router (GainLaw × 0.5)
    SoftLimiter,      // input soft limiter on/off (bSoftLimitter, OQ-13): kept for the sampling input stage
    MasterLimiter,    // master soft limiter on/off (default off)
    OutputTrim,       // 0–1023 (MIX-060)
    MuteMode          // 0 STOP SOUND (mute/solo gate the strip), 1 STOP STEPS (the strip sounds; the sequencer's steps of
                      // those tracks are skipped, TriggerRouter) — our addition
};

// Static soft-knee waveshaper (MIX-021, our design): y = x for |x| ≤ −3 dBFS; above,
// T + (C − T)·tanh((|x| − T)/(C − T)) with T = −3 dBFS and the ceiling C = −0.3 dBFS, from a 257-entry table with
// linear interpolation (no libm per sample). Never exceeds C; continuous with slope 1 at T; per sample, no state.
class SoftLimiter
{
public:
    static constexpr int kTable = 256;   // intervals
    static constexpr float kRange = 8.0f; // tanh argument covered by the table (tanh 8 ≈ 1 − 2e−7)
    SoftLimiter() noexcept;               // builds the table (libm; message thread)
    void process(float* l, float* r, int n) noexcept;
    float threshold() const noexcept { return threshold_; }
    float ceiling() const noexcept { return ceiling_; }
    float shape(float x) const noexcept; // one sample (scalar reference of process)

private:
    void processChannel(float* x, int n) const noexcept;
    float threshold_ = 0.0f, ceiling_ = 0.0f, scale_ = 0.0f;
    alignas(16) float table_[kTable + 1] {};
};

// MIX-060 output trim: 0 = mute, 1 = −60 dB, linear in dB to 1023 = 0 dB.
float outputTrimGain(int index1023) noexcept;

class Mixer
{
public:
    Mixer() noexcept;

    void prepare(double sampleRate, int maxBlock); // message thread: settles every ramp at its current target
    void setStrip(int scene, int track, const StripParams&, int frame) noexcept; // ramps start at `frame`
    void setGlobal(GlobalParam p, float value) noexcept { setGlobal(p, value, 0); }
    void setGlobal(GlobalParam, float value, int frame) noexcept;
    void setPatternMuteSolo(std::uint16_t mute, std::uint16_t solo) noexcept { setPatternMuteSolo(mute, solo, 0); }
    void setPatternMuteSolo(std::uint16_t mute, std::uint16_t solo, int frame) noexcept; // SCN-012
    void setScene(int scene, int frame) noexcept; // the strips follow the scene's parameters
    // Volume, pan, send and mute lock slots of a trigger (LockSet = engine::trigger::LockSet): a locked
    // slot overrides the strip's value from `frame`; an unlocked slot reverts to it unless the previous lock said Hold
    // (or Glide: held, not ramped). null = no locks (pads).
    template <class LockSet> void applyLocks(int track, const LockSet* locks, int frame) noexcept;
    // per-block modulation of a strip value (engine::modulation::ModMatrix track destinations), an offset in
    // the value's index units added to the stored or locked value and clamped to its range (FX-084 law); ramps from
    // frame 0 over MIX-002's lengths. 0 = unmodulated. With `hasOverride`, `override` (index units) replaces the stored
    // or locked value before the offset is added (a touch strip USER item), so it lands on its value on a locked step.
    enum class StripMod : std::uint8_t { Volume, Pan, Send };
    void setModulation(int track, StripMod which, float offset, bool hasOverride = false, float override = 0.0f) noexcept;
    // Cuts the track at once (tails included, no ramp) for the current block; it fades back in over 128 samples from
    // the next block if it is still audible (audio-engine.md "Hard mute"). For choke / mode changes.
    void hardMute(int track) noexcept;

    // The whole mixer for one block: processTracks + processSend + processMaster. Reads b.track[] / b.activeTracks /
    // b.aux, writes b.mainBus, b.sendBus, b.master (and the strips' post-fader signal back into b.track[]). b.sendBus is
    // valid after processTracks only when sendBusUsed() (no track tapped it: not cleared, to save the pass).
    void process(const BlockContext&, stages::BlockBuffers& b, const Hooks& h) noexcept
    {
        processTracks(b, h);
        processSend(b, h);
        processMaster(b, h);
    }
    void processTracks(stages::BlockBuffers& b, const Hooks& h) noexcept; // step 8b: strips → main / send bus
    void processSend(stages::BlockBuffers& b, const Hooks& h) noexcept;   // step 9:  send bus → FX → return
    void processMaster(stages::BlockBuffers& b, const Hooks& h) noexcept; // steps 10–11 (+ trim of step 13)

    const MeterBlock& meters() const noexcept { return meters_; } // copied into MeterFeed by the engine
    const StripParams& strip(int scene, int track) const noexcept;
    // What the strip plays now: the stored value, the step's lock over it, then the modulation (tests, UI).
    StripParams applied(int track) const noexcept { return effective(track); }
    int global(GlobalParam p) const noexcept { return globals_[static_cast<std::size_t>(p)]; }
    // MUTE MODE = STOP STEPS: track t is muted (or not soloed) — its sequencer steps do not trigger, its strip sounds.
    bool stepsMuted(int t) const noexcept { return ((stepMuted_ >> (t & 15)) & 1u) != 0; }
    std::uint16_t audibleMask() const noexcept { return audible_; } // MIX-003 rule result (tests, UI)
    // bit t = track t's block in BlockBuffers::track holds its post-fader, post-pan signal this block
    // (the assign feed of the output router; a silent or fully muted track is not set). Valid after processTracks.
    std::uint16_t stripOutputMask() const noexcept { return stripOut_; }
    // The input monitor, added to the master after the send return and before the master fader
    // (audio-engine.md step 11); null = none. Set once per block before processMaster; the pointers must stay valid
    // until it returns.
    void setMonitorInput(const float* l, const float* r) noexcept
    {
        monitorL_ = l;
        monitorR_ = r;
    }
    // false = processMaster leaves the output trim to the output stage (step 13 trims every logical
    // output, MIX-023); true (default) = the master is trimmed here (the mixer on its own: tests).
    void setTrimInMaster(bool on) noexcept { trimInMaster_ = on; }
    const stages::StereoBlock& sendReturn() const noexcept { return return_; }
    bool sendReturnActive() const noexcept { return returnActive_; }
    bool sendBusUsed() const noexcept { return sendUsed_; }

private:
    struct LockState
    {
        std::uint8_t mask = 0, hold = 0; // bits: 0 volume, 1 pan, 2 send, 3 mute
        float volume = 0, pan = 0, send = 0;
        bool mute = false;
    };
    void applyLockValues(int track, std::uint8_t mask, std::uint8_t hold, const float* values, int frame) noexcept;
    void retarget(int track, int frame) noexcept;
    void updateGates(int frame) noexcept;
    StripParams effective(int track) const noexcept;
    void finishLanes(int track, const float* acc) noexcept;

    std::array<std::array<StripParams, kTracks>, kScenes> params_ {};
    std::array<LockState, kTracks> locks_ {};
    std::array<std::array<float, 3>, kTracks> mod_ {}; // StripMod offsets (index units)
    std::array<std::array<float, 3>, kTracks> override_ {}; // StripMod overrides (index units), when overridden_
    std::array<std::uint8_t, kTracks> overridden_ {};        // bit w = StripMod w has an override
    int scene_ = 0;

    // Strip state, structure of arrays: lane k of track t = ramps_[t][k] (PanL, PanR, Send, Fader).
    std::array<std::array<GainRamp, kernels::kLanes>, kTracks> ramps_ {};
    std::uint32_t busyRamps_ = 0; // bit t = a lane of track t is ramping or waiting for its frame
    std::array<MuteRamp, kTracks> mute_ {};
    std::uint16_t audible_ = 0xFFFF, patternMute_ = 0, patternSolo_ = 0, hardCut_ = 0;

    // Buses.
    std::array<int, 7> globals_ {100, 100, 100, 0, 0, 1023, 0};
    std::uint16_t stepMuted_ = 0; // STOP STEPS: the tracks mute / solo would silence, whose steps are skipped
    GainRamp returnRamp_, masterRamp_, trimRamp_;
    SoftLimiter limiter_;
    stages::StereoBlock return_ {};
    bool returnActive_ = false;
    bool sendUsed_ = false; // the send bus holds this block's taps (else its content is stale)
    std::uint16_t stripOut_ = 0;             // tracks whose post-fader block is valid
    const float* monitorL_ = nullptr;        // input monitor (step 11)
    const float* monitorR_ = nullptr;
    bool trimInMaster_ = true;               // false = the output stage trims

    MeterBlock meters_ {};
    alignas(16) float gains_[kernels::kLanes * kBlockFrames] {};
    alignas(16) float muteGain_[kBlockFrames] {};
    alignas(16) float busGain_[kBlockFrames] {};
    alignas(16) float busGain2_[kBlockFrames] {};
};

template <class LockSet> void Mixer::applyLocks(int track, const LockSet* locks, int frame) noexcept
{
    if (track < 0 || track >= kTracks)
        return;
    static constexpr int kSlots[4] = {kLockVolume, kLockPan, kLockSend, kLockMute};
    std::uint8_t mask = 0, hold = 0;
    float values[4] {};
    if (locks != nullptr)
        for (int k = 0; k < 4; ++k)
        {
            float v = 0.0f;
            if (locks->get(kSlots[k], v))
            {
                mask = static_cast<std::uint8_t>(mask | 1u << k);
                values[k] = v;
                if (locks->change != nullptr && locks->change[kSlots[k]] != 0) // 1 Hold, 2 Glide
                    hold = static_cast<std::uint8_t>(hold | 1u << k);
            }
        }
    applyLockValues(track, mask, hold, values, frame);
}
} // namespace engine::mixer
