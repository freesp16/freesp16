// engine::afs — the analog-filter slot (spec 02-features/analog-filter-slot.md AFS-001…AFS-012,
// 01-architecture/audio-engine.md steps 8, 11, 12; research docs/effects/signal-flow.md
// "Analog-filter loop" and "DSF drive", F107, F187–F191).
//
// One instance, a plain member of engine::Engine. The slot sits at one point of the signal path:
//   Master       after the master out (step 11: master fader, limiter, meter, trim) — "MASTER → master meter →
//                ANALOG FILTER → MAIN OUT" (audio-engine.md, M p.46)
//   TrackPreFx   track t: source → AFS → FX1 → FX2 → pan (AFS-002 PRE FX)
//   TrackPostFx  track t: source → FX1 → FX2 → AFS → pan (AFS-002 POST FX, "return replaces the track's path before pan")
// Track points are reached through a wrapper around the mixer's insert hook of that track (wrapHooks): pre point →
// the FX bank's insert → post point, one virtual call per block.
//
// Signal path with ACTIVE on (AFS-003, AFS-010, AFS-011): drive gain 1 + 3·(i/1023)² with a 100-sample linear ramp
// (F107: step = (target − current)/100, 0 … +12.04 dB) → OVER detector (peak of the drive output above +5.1 dBFS,
// held 0.5 s [OBS]) → processor → (unity output level, our choice). Processors:
//   Model     the 2× oversampled ZDF ladder + high-pass (ModelProcessor, engine/fx/dsp/afs_ladder.h), AFS-004
//   External  the send/return loop, AFS-005: the driven signal goes to the FilterSend output port and the slot's
//             output is the return, read from audio input channels 2/3 (the reference's return is device input 2/3,
//             F189). On the device the codec's FilterSend pair feeds the unit's own analog filter (signal-flow.md
//             device pair 5, channels 10/11; OQ-14); a device without that port, or without return inputs — the Linux
//             desktop — runs the Model instead (our choice).
//   Analog    the unit's own analog filter in the main output path (MASTER route on a device whose platform switches
//             the filter board in: Platform::analogFilterOnMain). Only the drive and OVER run here: the LP CUTOFF and
//             RESONANCE pots act on the analog filter and are never read by the app, the HP CUTOFF pot sets the board's
//             high-pass DAC, and the app's filter-select line puts the filter after the main DAC while ACTIVE is on
//             (research signal-flow.md "DSF drive": dsfDriveMaster feeds the main DAC, the reference's MASTER insert
//             has no digital send). A digital filter here would add a second high-pass and leave the LP pot dead.
// ACTIVE off bypasses every stage, bit-transparent (AFS-012, NFR-030), except the DRIVE gain, which stays on the main
// output as on the reference (F107; unity at DRIVE 0, so still bit-transparent there; hardware feedback). A change of route, insert order, ACTIVE or the
// processor in use dips the signal 128 samples down and 128 up (AFS-006, F187: "a 256-sample dip, not an overlapping
// crossfade"): gain c/128, c = 127 … 0 then 1 … 128, applied at every path the change touches (the old and the new
// point: the master, or the track's path after its inserts); the switch happens at the block boundary where the gain
// reached 0, the processors restart from silence there.
//
// Modulation (AFS-004): an AD envelope (2 ms attack, afs.envDecay) triggered by the Full triggers of the
// assigned track (Master route: of every track), afs.envAmount ±6 octaves on the low-pass cutoff; the assigned
// track's LFO (its value, −1…1, at the block start) × afs.lfoAmount ±4 octaves (Master route: no LFO).
//
// Latency (external loop, AFS-005): afs.ping mutes the send for 2176 frames (the loop empties of program: a
// ≤ 2048-frame ring), then sends a −20 dBFS click (8-sample Hann burst) and captures 4096 frames of the return; the
// lag of the cross-correlation maximum, computed on the audio thread in 512-lag slices (≈ 4k multiply-adds per block,
// instead of a worker: no thread hop, bounded), is published as measuredLatency() (frames, the block adapter's
// 64 included; −1 = failed) for the app to store per device (afs.latency). A switch to the loop fades the send in over
// 128 samples, so its return starts from silence whatever the latency. Compensation of the other tracks (delay
// the main-bus sum by the latency on Track routes) is not implemented: it needs the mixer to sum the AFS
// track apart from the others.
//
// Threads: prepare() on the message thread (allocates nothing: every buffer is a member); the rest on the audio
// thread, except the atomics read by over(), overLevel(), measuredLatency() and pingCount().
#pragma once

#include "core/params/ParamId.h"
#include "engine/EngineApi.h"
#include "engine/fx/dsp/afs_ladder.h"
#include "engine/mixer/Mixer.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/trigger/TriggerEvent.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace engine::afs
{
enum class Route : std::uint8_t { Master, TrackPreFx, TrackPostFx };
enum class Processor : std::uint8_t { Model, External, Analog };

// Per-block control of a processor (normalised knob values; the modulation already in octaves).
struct AfsControl
{
    float drive01 = 0.0f, lpCutoff01 = 1.0f, resonance01 = 0.0f, hpCutoff01 = 0.0f;
    float envAmount = 0.0f; // octaves at full envelope (afs.envAmount × 6)
    float lfo = 0.0f;       // octaves this block (LFO value × afs.lfoAmount × 4)
    bool twoPole = false;
    std::uint8_t trig = 0;  // 0 = no trigger this block, else the envelope restarts at frame trig − 1
    float envDecayMs = 200.0f; // the envelope's decay time (our addition)
};

class AfsProcessor
{
public:
    virtual ~AfsProcessor() = default;
    virtual void prepare(double sampleRate, int maxBlock) = 0;
    virtual void reset() noexcept = 0;
    virtual void process(float* l, float* r, int frames, const AfsControl&) noexcept = 0;
    virtual int latencySamples() const noexcept = 0;
};

// Processor A: the digital model (AFS-004). process() takes exactly kBlockFrames frames.
class ModelProcessor final : public AfsProcessor
{
public:
    static constexpr int kTable = 1024;          // cutoff positions 0…1023 (+1 guard)
    static constexpr float kEnvOctaves = 6.0f;   // afs.envAmount ±100 %
    static constexpr float kLfoOctaves = 4.0f;   // afs.lfoAmount ±100 %
    static constexpr float kFeedbackMax = 4.6f;  // resonance 100 % (self-oscillation from 95 %: k = 4)

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override;
    void process(float* l, float* r, int frames, const AfsControl&) noexcept override;
    int latencySamples() const noexcept override { return fx::dsp::afs::kLatency; }

    void useScalarKernels(bool on) noexcept { scalar_ = on; } // tests: the scalar reference path
    // Laws (tests, UI formatters use the same numbers through the catalogue).
    static double cutoffHz(double pos1023) noexcept;    // 20 · 1000^(pos/1023)
    static double hpCutoffHz(int index1023) noexcept;   // 0 = off, 1…1023 → 20 · 100^((i − 1)/1022)
    static float feedback(float resonance01) noexcept;  // k: 4·r/0.95 to 95 %, then linear to 4.6
    // State after the last block (tests).
    float smoothedPosition() const noexcept { return pos_; }
    float smoothedFeedback() const noexcept { return kSm_; }
    float envelope() const noexcept { return env_; }

private:
    void coefficients(const AfsControl& c) noexcept; // the per-internal-sample coefficient pass (scalar, shared)

    double fs_ = 44100.0;
    bool scalar_ = false;
    alignas(16) float table_[kTable + 1] {};            // G = g/(1 + g) at position j
    alignas(16) fx::dsp::afs::LadderCoefs coefs_ {};
    alignas(16) float over_[2 * fx::dsp::afs::kOver] {}; // interleaved L/R internal frames
    alignas(16) float ladderOut_[2 * fx::dsp::afs::kOver] {};
    alignas(16) float noise_[2 * fx::dsp::afs::kOver * 8] {}; // 8 blocks of −120 dBFS seed, cycled
    int noisePos_ = 0;
    fx::dsp::afs::HalfbandState hb_ {};
    fx::dsp::afs::LadderState ladder_ {};
    fx::dsp::afs::HighPass hp_ {};
    float hpPos_ = -1.0f;       // smoothed HP index (block rate), −1 = off
    float hpSet_ = -2.0f;       // the index the coefficients were computed for
    float pos_ = 1023.0f, kSm_ = 0.0f, smooth_ = 0.0f;
    float env_ = 0.0f, envAttack_ = 0.0f;
    bool attacking_ = false, primed_ = false;
};

// Processor B: the external send/return loop (AFS-005).
class ExternalProcessor final : public AfsProcessor
{
public:
    static constexpr int kPingFrames = 4096;
    static constexpr int kFlushFrames = 2048 + 2 * kBlockFrames; // the longest loop the measurement accepts, emptied
    static constexpr int kSendFade = 128;
    static constexpr int kClickLength = 8;
    static constexpr int kLagsPerBlock = 512;

    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override;
    // l/r: the driven signal in, the return out (silence when no return this block).
    void process(float* l, float* r, int frames, const AfsControl&) noexcept override;
    int latencySamples() const noexcept override { return latency_.load(std::memory_order_relaxed) > 0 ? latency_.load(std::memory_order_relaxed) : 0; }

    void setReturn(const float* l, const float* r) noexcept // this block's return (null = none); re-arms the tick
    {
        retL_ = l;
        retR_ = r;
        ticked_ = false;
    }
    const float* sendL() const noexcept { return send_.l; }
    const float* sendR() const noexcept { return send_.r; }
    void startPing() noexcept;   // audio thread
    void failPing() noexcept;    // no loop available: the result is −1
    bool pinging() const noexcept { return ping_ != Ping::Idle; }
    void advanceIdle() noexcept; // once per block (process() calls it): the ping keeps time / captures / analyses
    int measuredLatency() const noexcept { return latency_.load(std::memory_order_acquire); } // −1 failed, 0 none
    std::uint32_t pingCount() const noexcept { return pings_.load(std::memory_order_acquire); }

    // The cross-correlation lag of `click` in `captured` (frames), or −1 when nothing above −60 dBFS came back.
    static int correlate(const float* captured, int n, const float* click, int clickLen, int fromLag, int toLag,
                         float& bestValue, int& bestLag) noexcept;
    static const float* click() noexcept;

private:
    enum class Ping : std::uint8_t { Idle, Flush, Capture, Analyse };
    void finishAnalysis() noexcept;
    stages::StereoBlock send_ {};
    const float* retL_ = nullptr;
    const float* retR_ = nullptr;
    Ping ping_ = Ping::Idle;
    int flushed_ = 0, captured_ = 0, nextLag_ = 0, bestLag_ = -1, sendRamp_ = 0;
    float best_ = 0.0f;
    bool clickSent_ = false, ticked_ = false;
    alignas(16) float capture_[kPingFrames] {};
    std::atomic<int> latency_ {0};
    std::atomic<std::uint32_t> pings_ {0};
};

// The afs.* parameters the slot implements (core/params/catalogue/afs.cpp), in field order.
enum class AfsField : std::uint8_t
{
    Active,      // "afs.active"       0/1
    InsertTrack, // "afs.insertTrack"  0 = MASTER, 1…16 = TRACK n
    InsertOrder, // "afs.insertOrder"  0 PRE FX, 1 POST FX
    Processor,   // "afs.processor"    0 MODEL, 1 EXTERNAL
    Drive,       // "afs.drive"        0–1023
    LpCutoff,    // "afs.lpCutoff"     0–1023
    Resonance,   // "afs.resonance"    0–1023
    HpCutoff,    // "afs.hpCutoff"     0–1023 (0 = off)
    Slope,       // "afs.slope"        0 = 24 dB (4-pole), 1 = 12 dB (2-pole tap)
    EnvAmount,   // "afs.envAmount"    −100…100 %
    EnvDecay,    // "afs.envDecay"     0–127 (10 ms … 4 s)
    LfoAmount,   // "afs.lfoAmount"    −100…100 %
    Ping,        // "afs.ping"         any value = measure the loop latency now (internal)
    Count
};
inline constexpr std::string_view kAfsParamIds[] = {"afs.active",    "afs.insertTrack", "afs.insertOrder", "afs.processor",
                                                    "afs.drive",     "afs.lpCutoff",    "afs.resonance",   "afs.hpCutoff",
                                                    "afs.slope",     "afs.envAmount",   "afs.envDecay",    "afs.lfoAmount",
                                                    "afs.ping"};
static_assert(std::size(kAfsParamIds) == static_cast<std::size_t>(AfsField::Count));

// afs.envDecay index → milliseconds: 10 · 400^(i/127) (10 ms … 4 s), our law.
double envDecayMs(int index127) noexcept;
// The drive law (F107): 1 + 3·(i/1023)².
inline float driveGain(int index1023) noexcept
{
    const float x = static_cast<float>(index1023 < 0 ? 0 : (index1023 > 1023 ? 1023 : index1023)) / 1023.0f;
    return 1.0f + 3.0f * (x * x);
}
constexpr float kOverThreshold = kAfsOverThreshold; // +5.1 dBFS on the drive output (AFS-011 [OBS])
constexpr int kDipSamples = 128;             // AFS-006

class AnalogFilterSlot
{
public:
    AnalogFilterSlot() noexcept;

    // ---- message thread ----
    void prepare(double sampleRate, int maxBlock); // both processors prepared; the configuration settles at once
    // The device switches its analog filter into the main output path (before prepare): the MASTER route with
    // PROCESSOR EXTERNAL then uses Processor::Analog instead of the digital model.
    void setAnalogMain(bool on) noexcept
    {
        analogMain_ = on;
        request();
    }
    bool analogMain() const noexcept { return analogMain_; }
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve) noexcept; // rt-ok: set-up only
    core::ParamId paramId(int field) const noexcept { return ids_[static_cast<std::size_t>(field)]; }
    static constexpr int kParams = static_cast<int>(AfsField::Count);
    // Lock-free reads for the UI / app (any thread).
    float overLevel() const noexcept { return overLevel_.load(std::memory_order_relaxed); } // held drive-out peak
    bool over() const noexcept { return overLevel() >= kOverThreshold; }                      // the OVER lamp
    int measuredLatency() const noexcept { return external_.measuredLatency(); }
    std::uint32_t pingCount() const noexcept { return external_.pingCount(); }

    // ---- audio thread ----
    // The contract setters: each one is a change with the AFS-006 dip when the point, ACTIVE or the processor
    // in use changes.
    void setRoute(Route, int track) noexcept;
    void setActive(bool) noexcept;
    void setProcessor(Processor) noexcept;
    void setField(AfsField, float value) noexcept; // one afs.* value in its native units
    bool onParam(const ParamChange& c) noexcept;   // an afs.* ParamChange (false = not an afs id)

    // Device period (Engine::process): which loop ports the device has, and the return frames of the period.
    void beginPeriod(const AudioIO& io) noexcept;
    void capture(int ioFrame, int blockFrame, int frames) noexcept; // after the block render, like the engine input
    // Block (Engine::renderBlock), in this order: beginBlock after the router's events (step 6) and the LFO; claimTrack
    // and wrapHooks after FxSlotBank::fillHooks; processMaster after step 11; routeOutputs after step 12.
    void beginBlock(std::span<const trigger::TriggerEvent> events, float lfoValue) noexcept;
    std::uint32_t claimTrack(stages::BlockBuffers& b) noexcept; // the assigned track keeps rendering while ACTIVE
    void wrapHooks(mixer::Hooks& h) noexcept;
    void processAt(Route, int track, float* l, float* r, int frames) noexcept; // called at the matching point
    void processMaster(stages::BlockBuffers& b) noexcept { processAt(Route::Master, -1, b.master.l, b.master.r, kBlockFrames); }
    void routeOutputs(stages::BlockBuffers& b) noexcept; // the FilterSend port while the external loop runs
    void endBlock() noexcept;

    // ---- inspection (tests; audio thread or device stopped) ----
    Route route() const noexcept { return cur_.route; }
    int track() const noexcept { return cur_.track; }       // −1 on Master
    bool active() const noexcept { return cur_.active; }
    Processor processorInUse() const noexcept { return cur_.proc; }
    bool externalAvailable() const noexcept { return sendPort_ && returnPort_; }
    bool dipping() const noexcept { return phase_ != Dip::None; }
    float dipGain() const noexcept { return static_cast<float>(count_) * (1.0f / kDipSamples); }
    int latencySamples() const noexcept;
    ModelProcessor& model() noexcept { return models_[static_cast<std::size_t>(live_)]; }
    ExternalProcessor& external() noexcept { return external_; }
    const AfsControl& control() const noexcept { return control_; }

private:
    struct Config
    {
        Route route = Route::Master;
        std::int8_t track = -1;
        bool active = false;
        Processor proc = Processor::Model; // the processor in use (External only while the loop exists)
        bool samePoint(const Config& o) const noexcept { return route == o.route && track == o.track; }
        bool operator== (const Config&) const = default;
    };
    enum class Dip : std::uint8_t { None, Down, Up };
    static constexpr int kMasterPath = 16; // dip path bits: 0…15 tracks (after their inserts), 16 the master

    class TrackHook final : public mixer::BusInsert
    {
    public:
        void process(float* l, float* r) noexcept override;
        AnalogFilterSlot* slot = nullptr;
        mixer::BusInsert* inner = nullptr;
        int track = 0;
    };

    Config wanted() const noexcept; // the configuration the parameters ask for
    void request() noexcept;        // compare wanted() with the target; start or extend the dip
    void switchConfig() noexcept;
    static std::uint32_t pathBit(const Config& c) noexcept { return c.route == Route::Master ? 1u << kMasterPath : 1u << (c.track & 15); }
    void run(float* l, float* r) noexcept; // drive → OVER → processor, at the current point
    void drive(float* l, float* r) noexcept; // the drive gain and its ramp (sets driveRan_)
    void meterOver(const float* l, const float* r) noexcept; // OVER: the drive output's peak held 0.5 s (overRan_)
    void warm(const float* l, const float* r) noexcept; // the target's model on a copy (the dip's last block down)
    void applyDip(float* l, float* r) const noexcept;

    double fs_ = 44100.0;
    // Two model instances: the live one, and the one warmed during the last block of a dip's way down at the target
    // point, so that the model enters the way up with its 31-sample half-band delay and its ladder already filled
    // (a cold start would put a step through the up-ramp). Swapped at the switch; one extra model block per dip.
    std::array<ModelProcessor, 2> models_ {};
    int live_ = 0;
    bool warmBlock_ = false, warmed_ = false;
    stages::StereoBlock warm_ {};
    ExternalProcessor external_;
    AfsControl control_ {};
    std::array<core::ParamId, kParams> ids_ {};
    std::array<TrackHook, kTracks> hooks_ {};

    // Parameter state (native units).
    bool active_ = false; // off until afs.active arrives (the model's afsEnabled default; the reference starts on)
    int insertTrack_ = 0, insertOrder_ = 0, processor_ = 1, drive_ = 0; // processor: afs.processor's default
    int envDecay_ = 64;
    float envAmount_ = 0.0f, lfoAmount_ = 0.0f;

    // Configuration and the AFS-006 dip.
    Config cur_ {}, target_ {};
    Dip phase_ = Dip::None;
    int count_ = kDipSamples;           // gain = count/128
    std::uint32_t dipPaths_ = 0;        // the paths the running dip applies to
    alignas(16) float dipGain_[kBlockFrames] {};
    bool dipBlock_ = false;             // this block's dip gain is not all 1

    // Drive (F107) and OVER (AFS-011).
    float driveCur_ = 1.0f, driveStep_ = 0.0f, driveTarget_ = 1.0f;
    int driveLeft_ = 0;
    alignas(16) float driveGain_[kBlockFrames] {};
    float overPeak_ = 0.0f;
    int overHold_ = 0, overHoldSamples_ = 22050;
    bool overRan_ = false;
    std::atomic<float> overLevel_ {0.0f};

    // External loop: ports of this period, the return adapter (one block late, like the engine's input).
    bool sendPort_ = false, returnPort_ = false;
    const float* retInL_ = nullptr;
    const float* retInR_ = nullptr;
    std::array<stages::StereoBlock, 2> ret_ {};
    int retFill_ = 0;
    bool retSeen_ = false, retValid_ = false;
    const float* retBlockL_ = nullptr;
    const float* retBlockR_ = nullptr;
    bool sent_ = false, visited_ = false, driveRan_ = false;
    bool pingWanted_ = false;
    bool analogMain_ = false; // Analog processor available on the MASTER route (setAnalogMain)
};
} // namespace engine::afs
