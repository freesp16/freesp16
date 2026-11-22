// services::sampling::SamplingSession — the message-thread side of sampling and live sampling (spec
// 02-features/sampling.md SMP-001…010, SMP-020…027). It owns the capture hub (ring +
// status), the Recorder thread and the live take buffers, and talks to the engine only through engine::EngineBridge
// (ParamChange sampling.* / live.*, ObjectSwap) — the engine contract (EngineApi.h).
//
//   ensureCapture(bpm)       allocates the ring — max(30 s, 8 bars + 2 beats at `bpm`), capped at 64 s (our cap;
//                            10.6 MB at 44.1 kHz for 30 s, 22.6 MB at the cap) —
//                            and installs it: ObjectSwap{SwapSlot::LiveTake, 15, 15}. From then on the engine writes
//                            the SOURCE into it every block. Re-allocates only when idle and the tempo needs more.
//   record(scene, track)     SMP-002: REC → standby: the Recorder is armed for the next take (Recordings/REC NNN.wav),
//                            then sampling.rec = 1 is posted; onTake(TakeInfo) arrives on the message thread when the
//                            engine ended the take (REC LENGTH, stopRecord) and the WAV + sidecar are in place.
//   stopRecord(scene, track) sampling.rec = 0 (REC again; in standby it cancels: onTake with cancelled = true).
//   frame()                  once per UI frame: pumps nothing itself (the Workers' pump does), watches the live take:
//                            a Done take is copied into a SampleBuffer on a worker (TakeBuilder) and installed on its
//                            track — ObjectSwap{TrackSample, scene, track} + live.takeReady (loop-after-rec starts at
//                            the next boundary, SMP-023); frees take buffers the engine has given back (refs 0).
//   setNudge(frames)         live.nudge (SMP-024); a positive nudge beyond the trailing margin captured at build time
//                            rebuilds the buffer once the ring holds it.
//   saveLive(file, done)     SMP-025 Save: the nudged window → 24-bit WAV + sidecar on a worker.
//
// Message thread only (the Recorder's callback is re-posted through Workers::post).
#pragma once

#include "core/params/ParamId.h"
#include "core/util/Result.h"
#include "core/util/sampling/CaptureHub.h"
#include "engine/EngineApi.h"
#include "services/library/Workers.h"
#include "services/sampling/Recorder.h"
#include "services/sampling/TakeBuilder.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace services::sampling
{
struct SessionConfig
{
    juce::File recordings;   // <library>/Recordings (SMP-007)
    double sampleRate = 44100.0;
    std::function<core::ParamId(std::string_view)> resolveParam; // ParamSpec id → ParamId (the engine's bindings)
};

class SamplingSession
{
public:
    static constexpr double kMinRingSeconds = 30.0, kMaxRingSeconds = 64.0;

    SamplingSession(engine::EngineBridge&, library::Workers&, SessionConfig);
    // The engine keeps its own hub reference; live take buffers it still holds are freed once it releases them (the
    // next session's frame(), or process exit), whichever is destroyed first.
    ~SamplingSession();
    SamplingSession(const SamplingSession&) = delete;
    SamplingSession& operator=(const SamplingSession&) = delete;

    static std::int64_t ringFramesFor(double bpm, double sampleRate) noexcept;
    // live.length index 0–5 fits the ring (window + 2 margins + 1 s slack); the UI disables the others.
    static bool liveLengthFits(int lengthIndex, double bpm, double sampleRate, std::int64_t ringFrames) noexcept;

    bool ensureCapture(double bpm);
    const core::sampling::CaptureHub* hub() const noexcept { return hub_; }

    // ---- sampling (SMP-001…007) ----
    core::Result<juce::File> record(int scene, int track, int channels); // channels 1 = LINE IN L/R, 2 otherwise
    void stopRecord(int scene, int track);
    bool recording() const noexcept { return recorder_ && recorder_->busy(); }
    std::function<void(const TakeInfo&)> onTake; // message thread

    // ---- live sampling (SMP-020…027) ----
    void liveArm(int scene, int track);   // live.rec = 1 (live.mode must be on)
    void liveDisarm(int scene, int track);
    void setNudge(std::int32_t frames);
    void frame();
    // The live take buffer installed on a track (null = none yet) and its layout margin.
    const core::audio::SampleBuffer* liveBuffer() const noexcept { return live_.buffer; }
    std::int32_t liveMargin() const noexcept { return live_.margin; }
    std::uint32_t liveSeq() const noexcept { return live_.seq; }
    void saveLive(const juce::File& target, std::function<void(core::Result<juce::File>)> done);
    std::function<void(int scene, int track, const core::audio::SampleBuffer*)> onLiveTake; // message thread
    std::function<void(const std::string&)> log;

    std::size_t ownedBuffers() const noexcept { return owned_.size(); }

private:
    bool post(std::string_view id, std::uint16_t scope, float value);
    void buildLive(bool full);
    void collect();

    engine::EngineBridge& bridge_;
    library::Workers& workers_;
    SessionConfig config_;
    core::sampling::CaptureHub* hub_ = nullptr; // our reference (the engine holds its own)
    std::unique_ptr<Recorder> recorder_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    struct Live
    {
        std::uint32_t seq = 0;      // the take handled last (CaptureStatus::liveSeq)
        std::int64_t start = 0, length = 0;
        std::int32_t margin = 0, nudge = 0;
        std::int64_t postCaptured = 0;
        int scene = 0, track = 0, channels = 2;
        float bpm = 120, bars = 1;
        bool building = false, rebuildWanted = false;
        const core::audio::SampleBuffer* buffer = nullptr; // our reference to the installed take
    } live_;
    std::vector<core::audio::SampleBuffer*> owned_; // take buffers we allocated (freed when refs reach 0)
    std::vector<std::string> salvaged_;             // WavWriter::salvage's lines, logged by the first frame()
};
} // namespace services::sampling
