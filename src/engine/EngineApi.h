// The engine's public contract — the only engine header services, ui, io and platform may include. Spec:
// 01-architecture/threading.md (channels between threads, object swap), audio-engine.md (64-frame engine block,
// per-block order), state-and-undo.md (commands reach the engine through the bridge, EDT-U4), 03-ui/control-surface.md
// UI-118 (pad hits go straight to the engine).
//
//   UI/message thread ──EngineBridge::post (SPSC, 4096)──▶ audio thread      ParamChange, TransportCmd, ObjectSwap, TriggerNow
//   pad/MIDI reader   ──TriggerSink::push (lock-free)───▶ audio thread      TriggerNow (UI-118)
//   audio thread      ──triple buffers──────────────────▶ UI               PlayheadFeed, MeterFeed (once per frame)
//   audio thread      ──SPSC─────────────────────────────▶ UI               EngineEvent (lossless, drained per frame)
//   platform::AudioDevice ──AudioProcessor::process(AudioIO)──▶ engine     once per device period (NFR-020/021)
//
// Real-time rules: everything the audio thread calls is noexcept and never allocates, locks, logs or does I/O; the
// message types are trivially copyable; no virtual call per sample (per period / per block only).
#pragma once

#include "core/params/ParamId.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace core
{
class ChangeSet;
}
namespace core::model
{
struct Project;
}

namespace engine
{
namespace model = ::core::model; // the bridge's model side (never dereferenced by real-time code)

constexpr int kBlockFrames = 64; // fixed engine block (audio-engine.md "Global numbers")
constexpr int kTracks = 16;
constexpr std::size_t kMessageQueueSize = 4096; // threading.md: UI → audio FIFO entries

// ---- UI → audio messages (threading.md "Channels between threads") ----

// scope = (scene << 4) | track for track/step parameters, scene index for scene parameters, 0 for project.
// Values are float: sample positions above 2^24 frames (~6.3 min at 44.1 kHz) travel as an ObjectSwap (region object).
struct ParamChange { core::ParamId param; std::uint16_t scope; float value; std::uint16_t rampSamples; };
struct TransportCmd { enum Kind : std::uint8_t { Play, Stop, Pause, HardStop, Continue, SetTempo, Locate } kind; double value; };
struct SwapSlot { enum Kind : std::uint8_t { TrackSample, Preview, Pattern, SceneConfig, ProjectConfig, LiveTake,
                                              SliceMap } kind;   // SliceMap: appended (the order is stable)
                  std::uint8_t scene, index; };                     // index = track or pattern
// Prepared off-thread, installed at block start; the replaced object goes to the RetireQueue (core/util). A post the
// engine does not accept (post() returns false) leaves ownership with the sender.
struct ObjectSwap { SwapSlot slot; const void* object; };
// A live trigger (pad, MIDI note): track 0–15, velocity 1–127, MIDI note (scale/slice pad modes; 60 = root),
// pad 0–15 or -1 when not from a pad.
struct TriggerNow { std::uint8_t track, velocity, note; std::int8_t pad; };
using EngineMessage = std::variant<ParamChange, TransportCmd, ObjectSwap, TriggerNow>; // trivially copyable members
static_assert(std::is_trivially_copyable_v<EngineMessage>, "engine messages cross threads by copy");

// ---- audio → UI ----

// Playhead snapshot, published once per device period (threading.md "Audio → UI"). Read once per UI frame.
struct PlayheadFeed
{
    std::uint64_t blockIndex = 0;     // engine blocks rendered so far (0 = engine not running)
    double ppq = 0, bpm = 120;        // transport position in quarter notes, tempo
    bool playing = false, preCount = false;
    bool externalClock = false;       // SYN-010: the transport follows MIDI clock (bpm = the followed tempo)
    bool clockLost = false;           // SYN-013: following, but no tick for 500 ms (or none yet)
    std::uint8_t scene = 0, pattern = 0, step = 0;
    std::int8_t pendingScene = -1, pendingPattern = -1; // queued switch (SCN-022 blink), -1 = none
    std::array<float, kTracks> trackPosition {};        // PLY-045: play position 0–1 per track, negative = idle
    // Hits per track (Full / Half triggers from any source: pads, MIDI, sequencer steps, retrigger, repeat; a muted
    // track's skipped step is none), wrapping. The UI flashes a pad when its count moved since the last frame.
    std::array<std::uint16_t, kTracks> trackHits {};
};

// The analog filter's OVER lamp threshold on its drive output: +5.1 dBFS (AFS-011 [OBS]).
inline constexpr float kAfsOverThreshold = 1.7988709f;

// Meter snapshot (linear peak / RMS of the last period; the UI converts to dB and decays, threading.md "Timing").
struct MeterFeed
{
    std::uint64_t blockIndex = 0;
    std::array<float, 2> masterPeak {}, masterRms {};
    std::array<float, 2> sendPeak {}, inputPeak {};
    std::array<std::array<float, 2>, kTracks> trackPeak {};
    float afsOverLevel = 0.0f; // the analog-filter drive output's held peak, linear (OVER at ≥ kAfsOverThreshold)
};

// Lossless audio → UI events (recorded steps, live-sampling marks, switch done), SPSC, drained once per UI frame.
struct EngineEvent
{
    enum class Kind : std::uint8_t { RecordedHit, ErasedStep, SwitchDone, LiveSampleMark, Overload } kind;
    std::uint8_t track = 0, step = 0;
    std::int16_t value = 0;   // velocity, scene*16+pattern, mark index … per kind
    std::int8_t offset24 = 0; // micro-timing of a recorded hit, −24…+24 = ±1 step (SEQ-005)
    std::uint8_t origin = 0;  // RecordedHit: kOriginLive (a pad / MIDI hit) or kOriginRepeat (touch strip REPEAT)
    static constexpr std::uint8_t kOriginLive = 0, kOriginRepeat = 1;
    // RecordedHit: a pitched hit (a MIDI note, a SCALE pad) and the semitones it played at (TriggerEvent::semitones);
    // the recorder stores them as the step's PITCH lock (SEQ-041: the step replays what was heard).
    std::int8_t semitones = 0;
    bool pitched = false;
};

namespace fx
{
struct FxMeterFeed; // engine/fx/FxSlotState.h
}

class EngineBridge {                                                // message thread side
public:
    virtual ~EngineBridge() = default;
    virtual bool post(const EngineMessage&) = 0;                                // SPSC, 4096 entries (threading.md)
    virtual void apply(const core::ChangeSet&, const model::Project&) = 0;      // model diff → messages
    virtual const PlayheadFeed& playhead() const = 0;  virtual const MeterFeed& meters() const = 0;  // triple-buffered snapshots
    virtual void retirePending() = 0;                                           // frees objects the audio thread retired
    // Lossless audio → UI events (recorded steps, live-sampling marks, switch-done), SPSC, drained once per UI frame.
    virtual bool popEvent(EngineEvent& out) = 0;
    // The effect panels' feed (engine/fx/FxSlotState.h), triple-buffered like
    // meters(); null when the engine has no FX bank (fakes, the measuring bridge).
    virtual const fx::FxMeterFeed* fxMeters() const { return nullptr; }
};

// Per-block context every engine stage receives (the stages run in audio-engine.md order; the fields this frozen
// context cannot carry are in stages::BlockBuffers).
struct BlockContext {
    double sampleRate; int frames;                  // always 64
    double ppqStart, ppqEnd, bpm; bool playing, startedThisBlock, stoppedThisBlock;
    std::uint32_t triggerMask; std::array<std::int16_t, 16> triggerOffset;
    std::uint64_t blockIndex = 0;
};

// ---- what a platform::AudioDevice drives ----

// Logical stereo output ports, in the order of platform::AudioPort (platform/Platform.h checks they agree). The
// engine renders planar float per logical port and never knows the physical channel layout.
enum class OutPort : std::uint8_t { Main, Out34, Out56, Out78, Phones, FilterSend, Count };

// out[2 * port] = left, out[2 * port + 1] = right of logical port `port` (OutPort order); a null pair = the device
// has no such port (the engine skips it). in: planar input channels (null when numIn == 0). frames: any count (the
// engine runs fixed 64-frame blocks with a FIFO adapter). hostTimeNs: monotonic time at the period start (MIDI sync).
struct AudioIO { float* const* out; int numOutPorts;            // planar L/R per platform::AudioPort, 2 * ports
                 const float* const* in; int numIn; int frames; std::uint64_t hostTimeNs; };
class AudioProcessor { public: virtual ~AudioProcessor() = default;
    virtual void prepare(double sampleRate, int maxDeviceFrames) = 0;   // message thread, allocates
    virtual void process(const AudioIO&) noexcept = 0; };              // audio thread, any frame count
// Live triggers straight to the engine (UI-118). Safe to call from several producer threads at once (pad reader, MIDI
// readers); never blocks; false when the queue is full (the hit is dropped, the UI still shows it).
class TriggerSink { public: virtual ~TriggerSink() = default; virtual bool push(const TriggerNow&) noexcept = 0; };

// ---- the engine as the composition root sees it ----

// Set-up the composition root hands to the engine (message thread, before prepare). resolveParam maps a ParamSpec id
// ("track.volume") to its ParamId, so the engine binds the parameters it implements without including the registry.
struct EngineSetup
{
    std::function<core::ParamId(std::string_view id)> resolveParam; // rt-ok: message-thread set-up only
    // The FX slot bank's route table needs every ParamSpec id in registry order (index =
    // ParamId.value) and the internal index k of each FX knob (core::fx::internalIndexTable, -1 elsewhere); the
    // engine never sees the registry, so the composition root fills both. Empty = no FX routes (every slot Off).
    std::vector<std::string_view> paramIds {};   // rt-ok: message-thread set-up only
    std::vector<std::int8_t> fxInternalIndex {}; // rt-ok: message-thread set-up only
    // The temporary 440 Hz test tone on an empty track 1 and the ping on empty pads (EngineOptions::testTone). The app
    // turns it off unless the `test_tone=1` setting asks for it (the smoke checks); hardware feedback: no sine at start.
    bool testTone = true;
};

// Engine load as the LoadMeter measures it per 64-frame block. Cycles are A9 cycles at
// 1 GHz (1 ns = 1 cycle); windows cover the last 10–20 s, blocks/overruns count since prepare().
struct EngineLoad
{
    double avgCycles = 0, p95Cycles = 0, maxCycles = 0, loadPercent = 0;
    std::uint64_t blocks = 0, overruns = 0;
};

class AudioEngine : public AudioProcessor
{
public:
    virtual std::string_view name() const noexcept = 0;  // "engine" for engine::Engine
    virtual EngineBridge& bridge() noexcept = 0;         // message thread side
    virtual TriggerSink& triggers() noexcept = 0;        // pad/MIDI reader threads (UI-118)
    virtual std::uint64_t blocksRendered() const noexcept = 0;
    virtual EngineLoad load() const noexcept = 0;        // message thread, lock-free
};

// engine/EngineFactory.cpp: the engine the app runs (engine::Engine).
std::unique_ptr<AudioEngine> createEngine(const EngineSetup& setup = {});
} // namespace engine
