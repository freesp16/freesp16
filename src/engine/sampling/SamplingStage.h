// engine::sampling::SamplingStage — sampling and live sampling inside engine::Engine (spec
// 02-features/sampling.md SMP-001…010, SMP-020…027; threading.md "Audio → Recorder").
// One plain member of the engine; three calls per block:
//
//   input(b)            after step 1: InputStage (soft limiter, clip, meter) — BlockBuffers::inputL/R become the
//                       post-limiter input for the Thru tracks, the input monitor and the meters
//   schedule(…)         step 6, before the trigger router: live sampling boundaries (start, loop-after-rec trigger)
//   capture(…)          step 13 ("LiveSampler/recorder taps"): the SOURCE block — LINE IN stereo / L / R (post-limiter)
//                       or RESAMPLE (OutputRouter::resampleTap, the master after the fader) — into the capture ring,
//                       the REC THRESHOLD / LENGTH state machine, the status for the services
//
// The capture ring lives in the CaptureHub the sampling session installs with ObjectSwap{SwapSlot::LiveTake, 15, 15}
// (core/util/sampling/CaptureHub.h). Without a hub nothing is captured: a REC waits in standby (the session posts the
// hub and the REC in one go, so both can arrive in the same drain); a replaced hub cancels the running take. The ring is
// written every block from its installation on (live sampling needs the past, SMP-024/028), whatever the state.
//
// Sampling state machine (SMP-002/005; sampling.rec scope = (scene << 4) | track):
//   rec 1        Idle/Done → Standby (takeSeq + 1; SOURCE, THRESHOLD, LENGTH, QUANTIZE are fixed from here,
//                SMP-001/002)
//   Standby      QUANTIZE n beats (ours; it replaces the THRESHOLD rule) → Recording at the next multiple of n
//                      beats of the pattern pass (gridFrame, capped at the pass end), latency compensated; stopped →
//                      at PLAY
//                OFF   → Recording at frame 0 of this block's capture
//                dB    → Recording at the first frame whose |x| exceeds the threshold, minus sampling.preRoll
//                PLAY  → Recording at the sequencer start edge (Transport::startFrame), latency compensated (OQ-21)
//   Recording    REC LENGTH n bars: end = start + n × 4 × 60 / BPM × fs exactly (BPM at the start) → Done
//                REC LENGTH OFF: rec 0 ends it at this block's first frame (the disk is the limit, SMP-005)
//   rec 0        Standby → Cancelled; Recording → Done (end = now)
//   drop 1       (ours, scope = the take's scene / track) Standby or Recording → Cancelled: the take is thrown away
// Latency (SMP-027): LINE IN lags the transport by the engine's input block (64 frames) + live.inputLatency (the
// device's reported input latency); synced starts (PLAY, live boundaries) add it, RESAMPLE has none.
//
// Threads: bindParams on the message thread; the rest on the audio thread. No allocation, no lock, no I/O.
// Budget: ≤ 2.5 k A9 cycles per block (limiter 300, ring write 120, meter 60, threshold 60, bookkeeping).
#pragma once

#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/sampling/CaptureRing.h"
#include "engine/sampling/InputStage.h"
#include "engine/sampling/LiveSampler.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/stages/BlockBuffers.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace engine::trigger
{
class TriggerRouter;
}

namespace engine::sampling
{
enum class SamplingParam : std::uint8_t
{
    Source,
    Threshold,
    Length,
    Rec,
    PreRoll,
    ScreenOpen,
    LiveMode,
    LiveLength,
    LiveLoopAfterRec,
    LiveRec,
    LiveNudge,
    LiveTakeReady,
    InputLatency,
    Quantize,
    Drop,
    Count
};
inline constexpr std::string_view kSamplingParamIds[] = {
    "sampling.source",  "sampling.threshold", "sampling.length", "sampling.rec",       "sampling.preRoll",
    "sampling.screenOpen", "live.mode",       "live.length",     "live.loopAfterRec", "live.rec",
    "live.nudge",       "live.takeReady",     "live.inputLatency", "sampling.quantize", "sampling.drop"};
static_assert(std::size(kSamplingParamIds) == static_cast<std::size_t>(SamplingParam::Count));

enum class Source : std::uint8_t { LineIn, LineInL, LineInR, Resample }; // sampling.source order (SMP-001)
// REC QUANTIZE (sampling.quantize): the wait of each index in beats (quarter notes); 0 = OFF.
inline constexpr int kQuantizeBeats[] = {0, 1, 2, 4, 8, 16};

inline constexpr SwapSlot kCaptureHubSlot {SwapSlot::LiveTake, core::sampling::kHubScene, core::sampling::kHubIndex};

class SamplingStage
{
public:
    // ---- message thread ----
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                    bridge::ParamBindings& b);
    void prepare(double sampleRate) noexcept;

    // ---- audio thread ----
    // true = a sampling.* / live.* parameter (consumed). mixer.softLimiter is observed (the input stage) and not
    // consumed: the mixer keeps its copy.
    bool onParam(const ParamChange&) noexcept;
    bool set(SamplingParam, std::uint16_t scope, float value) noexcept; // the same without a ParamId (tests, bench)
    void setLimiter(bool on) noexcept { input_.setLimiter(on); }
    // The hub installed in kCaptureHubSlot (null = none); a new hub resets the state machines. Block start.
    void install(const CaptureHub* hub) noexcept;

    void input(stages::BlockBuffers& b) noexcept;
    void schedule(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                  const scene::ScenePatternSwitcher& switcher, trigger::TriggerRouter* router) noexcept;
    // startFrame = Transport::startFrame() (PLAY threshold); tapL/R = the resample tap of this block (null = silent).
    void capture(const BlockContext& ctx, const stages::BlockBuffers& b, const float* tapL, const float* tapR,
                 int startFrame) noexcept;

    // ---- inspection (tests, the engine's diagnostics) ----
    RecState state() const noexcept { return state_; }
    std::int64_t takeStart() const noexcept { return start_; }
    std::int64_t takeEnd() const noexcept { return end_; }
    std::uint32_t takeSeq() const noexcept { return seq_; }
    Source source() const noexcept { return source_; }
    int latency() const noexcept { return latencyFor(source_); }
    bool screenOpen() const noexcept { return screenOpen_; } // SMP-008 / SCN-051 (the MIDI stage reads it)
    const InputStage& inputStage() const noexcept { return input_; }
    LiveSampler& live() noexcept { return live_; }
    const LiveSampler& live() const noexcept { return live_; }
    CaptureHub* hub() const noexcept { return hub_; }

private:
    Source ringSource() const noexcept { return ringSource_; } // what the ring captures this block
    int latencyFor(Source s) const noexcept { return s == Source::Resample ? 0 : kBlockFrames + inputLatency_; }
    void rec(bool on, std::uint16_t scope) noexcept;
    void drop(std::uint16_t scope) noexcept;
    void publishTake() noexcept;

    InputStage input_;
    LiveSampler live_;
    CaptureHub* hub_ = nullptr; // the slot's object; the audio thread is its only writer (ring + status)
    std::array<core::ParamId, static_cast<std::size_t>(SamplingParam::Count)> ids_ {};
    core::ParamId softLimiterId_ {};
    double sampleRate_ = 44100.0;

    // Settings (SMP-001/002/005).
    Source source_ = Source::LineIn;
    int threshold_ = 0; // 0 OFF, 1…41 −60…−20 dB, 42 PLAY
    int lengthBars_ = 0; // 0 OFF, 1–4
    int preRollMs_ = 0;
    int quantize_ = 0; // sampling.quantize index
    int inputLatency_ = 0;
    bool screenOpen_ = false;
    bool liveLoop_ = false;
    Source ringSource_ = Source::LineIn; // fixed while a take (sampling or live) records

    // The take (fixed at REC).
    RecState state_ = RecState::Idle;
    Source takeSource_ = Source::LineIn;
    int takeThreshold_ = 0, takeBars_ = 0, takeScene_ = 0, takeTrack_ = 0;
    int takeBeats_ = 0;     // REC QUANTIZE in beats (0 = OFF)
    int gridFrame_ = -1;    // schedule(): this block's REC QUANTIZE boundary frame (−1 = none)
    std::int64_t start_ = -1, end_ = -1;
    std::uint32_t seq_ = 0;
    bool stopRequested_ = false;
    bool statusDirty_ = false;

    alignas(16) float zeros_[kBlockFrames] {};
};
} // namespace engine::sampling
