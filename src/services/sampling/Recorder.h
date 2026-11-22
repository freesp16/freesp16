// services::sampling::Recorder — the disk recorder thread (spec 02-features/sampling.md SMP-005/007,
// 01-architecture/threading.md "Disk recorder: high priority, drains the record ring buffer to disk, never blocks the
// audio thread"). One ring, two readers.
//
//   message thread  start(file, rate, channels)  arms the writer for the next take the engine starts (the session
//                                                posts sampling.rec = 1 right after): the take whose takeSeq is newer
//                                                than the one at start()
//   recorder thread every 5 ms: reads CaptureStatus {takeSeq, state, takeStart, takeEnd} and the ring head, copies
//                   [pos, min(head, end)) out of the ring in 64 KB chunks of 24-bit WAV (WavWriter), until the engine
//                   has published the end and the file reached it; then finish (fsync, rename into place) + sidecar
//                   (SidecarStore: bpm = the BPM at the start, bars = REC LENGTH when synced, SMP-007)
//   any thread      stop(done)   "tell me when the take is complete": done(TakeInfo) runs on the recorder thread when
//                                the take finishes (or at once, on the caller's thread, when it already has). The engine
//                                decides the end (REC LENGTH, or sampling.rec = 0 — the session posts it); stop() does
//                                not end the take by itself.
//   cancel()        abandons the current take (the part file is deleted); a take the engine cancelled (REC left in
//                   standby, RecState::Cancelled) ends the same way, done(TakeInfo{cancelled}).
//
// The ring must hold more than the recorder's worst stall (threading.md: ≥ 10 s; the session allocates ≥ 30 s). If the
// writer falls behind by more than the ring (the head overtook it), the take fails with "overrun" instead of writing
// garbage. Free space below 1 GB at start() sets TakeInfo::lowDisk (the UI warns, SMP-005); the length is otherwise
// limited only by the disk (a full disk fails the take with the OS error).
//
// Memory: the thread, the conversion chunk and the two read buffers are allocated once (constructor / open); a
// recording of any length keeps RAM flat. CPU budget: ≤ 3 % of one A9 core at 44.1 kHz stereo (88k samples/s
// conversion + 265 KB/s writes).
#pragma once

#include "core/util/Result.h"
#include "core/util/sampling/CaptureHub.h"
#include "services/sampling/WavWriter.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace services::sampling
{
struct TakeInfo
{
    bool ok = false, cancelled = false, lowDisk = false;
    std::string error;          // ok == false && ! cancelled: why (user message + detail)
    juce::File file;            // the finished WAV (in place, with its sidecar)
    std::int64_t frames = 0;
    std::int64_t ringStart = -1; // where the take started in the ring (TakeBuilder / SaveSelection read from there)
    int channels = 2;
    double sampleRate = 44100.0;
    float bpm = 0.0f;           // the sidecar's bpm (the sequencer BPM at the start)
    int bars = 0;               // REC LENGTH (0 = OFF: not synced)
    int scene = 0, track = 0;
    std::uint32_t takeSeq = 0;
    float peak = 0.0f;          // max |x| of the take (linear)
};

class Recorder
{
public:
    static constexpr std::int64_t kLowDiskBytes = 1ll << 30; // SMP-005: warn below 1 GB free
    static constexpr int kPollMs = 5;

    explicit Recorder(core::sampling::CaptureHub& hub); // retains the hub; starts the (idle) recorder thread
    ~Recorder();                                        // abandons an unfinished take, joins the thread
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    core::Result<void> start(const juce::File& file, double sampleRate, int channels);
    void stop(std::function<void(TakeInfo)> done);
    void cancel();

    bool busy() const noexcept { return armed_.load(std::memory_order_acquire); }
    std::int64_t framesWritten() const noexcept { return written_.load(std::memory_order_relaxed); }
    std::uint64_t drains() const noexcept { return drains_.load(std::memory_order_relaxed); } // passes with data

private:
    void run();
    bool drain(); // one pass; true = the take finished (finish_ filled)
    void complete(TakeInfo info);

    core::sampling::CaptureHub& hub_;
    WavWriter writer_;
    std::vector<float> l_, r_; // one chunk of frames
    std::int64_t chunkFrames_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool quit_ = false;
    std::atomic<bool> armed_ {false};
    std::atomic<bool> cancel_ {false};
    std::function<void(TakeInfo)> done_;
    bool finished_ = false;   // the take completed before stop() gave a callback
    TakeInfo result_ {};

    // The take being written (recorder thread while armed).
    std::uint32_t armSeq_ = 0, seq_ = 0;
    bool haveTake_ = false;
    std::int64_t pos_ = -1;
    TakeInfo info_ {};
    std::atomic<std::int64_t> written_ {0};
    std::atomic<std::uint64_t> drains_ {0};
};
} // namespace services::sampling
