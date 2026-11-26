// The capture ring and the sampler's status, shared by the engine (one writer: the audio thread) and the sampling
// services (readers: the Recorder thread, the take builder's workers, the message thread) — spec
// 02-features/sampling.md SMP-001…010, SMP-020…027; 01-architecture/threading.md "Audio → Recorder: lock-free ring
// buffer (≥ 10 s)". One ring, two readers.
//
// Why core/util (as core/util/arrangement/ArrangementPlan.h): its natural home would be engine/sampling/CaptureRing.h,
// but tools/check-deps.py lets services and app include only engine/EngineApi.h of the engine, so the one type both
// halves share lives here; engine/sampling/CaptureRing.h re-exports it under that name. The hub is created
// by services::sampling::SamplingSession (message thread, it allocates the ring) and handed to the engine with
// ObjectSwap{SwapSlot::LiveTake, kHubScene, kHubIndex} — EngineApi.h has no other way for the app to reach engine
// memory. Lifetime: an intrusive count (the session holds one reference, the installed slot one); the last release
// deletes it, on a non-real-time thread (the engine's DestroyFn runs on the message thread).
//
//   CaptureRing  float planar stereo, `capacity` frames, written once per engine block (64 frames) by the audio
//                thread, read by absolute frame index ("frames written since the hub was installed"). head() is the
//                release-published write position; read(from, n) copies [from, from + n) and re-validates the head
//                afterwards (seqlock style): false when any of it was or may have been overwritten meanwhile (the
//                writer may be one unpublished write of kMaxWrite frames ahead), or is not written yet.
//   CaptureStatus the sampler's state for the services and the UI: relaxed / release atomics, 32- or 64-bit (lock-free
//                on the Cortex-A9, ldrexd/strexd; no libatomic). A take is published as {seq, start, end}: the audio
//                thread writes start/end before storing state with release; readers load state with acquire.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace core::sampling
{
inline constexpr std::uint8_t kHubScene = 15, kHubIndex = 15; // SwapSlot::LiveTake 15/15

// Sampler states (SMP-002): Standby = REC flashing (waiting for the threshold / PLAY / boundary); Recording = REC lit;
// Done = the take's end is known (the ring may still be filling up to it); Cancelled = REC left before the start, or
// the take dropped (sampling.drop: a pad hit again during the take) — the recorder deletes its part file.
enum class RecState : std::int32_t { Idle, Standby, Recording, Done, Cancelled };
// Live sampling (SMP-020…023): Armed = waits for the next pattern-quantize boundary; Recording; Done = the window is
// complete in the ring (end ≤ head); LoopPending = the take buffer is installed, waits for the next boundary; Looping.
enum class LiveState : std::int32_t { Idle, Armed, Recording, Done, LoopPending, Looping };

class CaptureRing
{
public:
    static constexpr int kMaxWrite = 4096; // the longest single write (frames): the validation guard of read()

    CaptureRing() = default;
    CaptureRing(const CaptureRing&) = delete;
    CaptureRing& operator= (const CaptureRing&) = delete;

    // Message thread, before the hub is handed to the engine. false = out of memory.
    bool allocate(std::int64_t frames, int /*channels: always stored as planar stereo*/ = 2)
    {
        if (frames <= kMaxWrite)
            frames = 2 * kMaxWrite;
        l_.reset(new (std::nothrow) float[static_cast<std::size_t>(frames)]); // rt-ok: message thread
        r_.reset(new (std::nothrow) float[static_cast<std::size_t>(frames)]); // rt-ok: message thread
        if (! l_ || ! r_)
        {
            l_.reset();
            r_.reset();
            capacity_ = 0;
            return false;
        }
        std::memset(l_.get(), 0, sizeof(float) * static_cast<std::size_t>(frames));
        std::memset(r_.get(), 0, sizeof(float) * static_cast<std::size_t>(frames));
        capacity_ = frames;
        head_.store(0, std::memory_order_relaxed);
        return true;
    }

    // Audio thread: appends `frames` (≤ kMaxWrite) frames; r null = mono (l on both sides).
    void write(const float* l, const float* r, int frames) noexcept
    {
        if (capacity_ == 0 || frames <= 0)
            return;
        const std::int64_t h = head_.load(std::memory_order_relaxed);
        const auto pos = static_cast<std::size_t>(h % capacity_);
        const std::size_t n = static_cast<std::size_t>(frames);
        const std::size_t first = std::min(n, static_cast<std::size_t>(capacity_) - pos);
        if (r == nullptr)
            r = l;
        copyIn(l_.get(), l, pos, first, n);
        copyIn(r_.get(), r, pos, first, n);
        head_.store(h + frames, std::memory_order_release);
    }

    // Frames written since allocate() (the next write's absolute index).
    std::int64_t head() const noexcept { return head_.load(std::memory_order_acquire); }
    std::int64_t capacity() const noexcept { return capacity_; }
    // The oldest frame read() still accepts (with the guard for an unpublished write).
    std::int64_t oldest() const noexcept
    {
        const std::int64_t o = head() + kMaxWrite - capacity_;
        return o > 0 ? o : 0;
    }

    // Any thread: copies [from, from + frames) into l / r (either may be null). false = not written yet, or overwritten.
    bool read(std::int64_t from, int frames, float* l, float* r) const noexcept
    {
        if (capacity_ == 0 || frames < 0 || from < 0)
            return false;
        if (frames == 0)
            return true;
        const std::int64_t h = head();
        if (from + frames > h || from < h + kMaxWrite - capacity_)
            return false;
        const auto pos = static_cast<std::size_t>(from % capacity_);
        const std::size_t n = static_cast<std::size_t>(frames);
        const std::size_t first = std::min(n, static_cast<std::size_t>(capacity_) - pos);
        if (l != nullptr)
            copyOut(l, l_.get(), pos, first, n);
        if (r != nullptr)
            copyOut(r, r_.get(), pos, first, n);
        std::atomic_thread_fence(std::memory_order_acquire);
        return from >= head() + kMaxWrite - capacity_; // still valid after the copy
    }

private:
    static void copyIn(float* dst, const float* src, std::size_t pos, std::size_t first, std::size_t n) noexcept
    {
        std::memcpy(dst + pos, src, sizeof(float) * first);
        if (first < n)
            std::memcpy(dst, src + first, sizeof(float) * (n - first));
    }
    static void copyOut(float* dst, const float* src, std::size_t pos, std::size_t first, std::size_t n) noexcept
    {
        std::memcpy(dst, src + pos, sizeof(float) * first);
        if (first < n)
            std::memcpy(dst + first, src, sizeof(float) * (n - first));
    }

    std::unique_ptr<float[]> l_, r_;
    std::int64_t capacity_ = 0;
    alignas(64) std::atomic<std::int64_t> head_ {0};
};

struct CaptureStatus
{
    // ---- sampling (SMP-001…005) ----
    std::atomic<std::uint32_t> takeSeq {0};          // +1 at every REC press that enters standby
    std::atomic<std::int32_t> state {0};             // RecState
    std::atomic<std::int64_t> takeStart {-1};        // ring frame of the take's first frame (−1 = not started)
    std::atomic<std::int64_t> takeEnd {-1};          // exclusive end (−1 = still open)
    std::atomic<std::int32_t> takeChannels {2};      // 1 = mono source (LINE IN L or R), 2 = stereo
    std::atomic<std::int32_t> takeTrack {0};         // the target track 0–15 (scope of sampling.rec)
    std::atomic<std::int32_t> takeScene {0};
    std::atomic<float> takeBpm {120.0f};             // the sequencer BPM at the start (sidecar bpm, SMP-007)
    std::atomic<std::int32_t> takeBars {0};          // REC LENGTH bars (0 = OFF: not synced)
    std::atomic<std::int32_t> takeQuantize {0};      // REC QUANTIZE of the take in beats (0 = OFF)
    std::atomic<float> startInBeats {-1.0f};         // standby with REC QUANTIZE: beats to the start (−1 = stopped)
    // ---- live sampling (SMP-020…027) ----
    std::atomic<std::uint32_t> liveReset{0};         // token of the last live-take reset applied by the engine
    std::atomic<std::uint32_t> liveSeq {0};          // +1 at every take that starts recording
    std::atomic<std::int32_t> liveState {0};         // LiveState
    std::atomic<std::int64_t> liveStart {-1};        // ring frame of the take window (latency compensated, no nudge)
    std::atomic<std::int64_t> liveLength {0};        // frames (exactly the record length)
    std::atomic<std::int32_t> liveTrack {0}, liveScene {0};
    std::atomic<std::int32_t> liveNudge {0};         // frames, clamped to ±liveMargin (SMP-024)
    std::atomic<std::int32_t> liveMargin {0};        // one beat at the take's tempo (frames)
    std::atomic<float> liveBpm {120.0f};
    std::atomic<float> liveBars {1.0f};              // the record length in bars (1/4 … 8)
    std::atomic<std::int64_t> loopStartFrame {-1};   // ring frame (transport) of the last loop-after-rec trigger
    // ---- input meter (SMP-003): post-limiter peak of the last block, clip = raw input ≥ full scale ----
    std::atomic<float> peakL {0.0f}, peakR {0.0f};
    std::atomic<std::uint32_t> clips {0};            // blocks with a clipped input since the hub was installed
    std::atomic<std::int32_t> latency {0};           // what the ring's source lags the transport (frames)
    std::atomic<std::int32_t> ringChannels {2};      // 1 = the ring holds a mono source (LINE IN L / R) on both sides
    std::atomic<std::uint64_t> blocks {0};           // engine blocks captured (liveness)
};
static_assert(std::atomic<std::int64_t>::is_always_lock_free && std::atomic<float>::is_always_lock_free,
              "the capture status must not need libatomic");

struct CaptureHub
{
    CaptureRing ring;
    CaptureStatus status;
    double sampleRate = 44100.0;
    mutable std::atomic<std::uint32_t> refs {1}; // the creator's reference

    void retain() const noexcept { refs.fetch_add(1, std::memory_order_relaxed); }
    // Non-real-time threads only: the last release deletes the hub.
    void release() const noexcept
    {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this; // rt-ok: never on the audio thread (the slot's DestroyFn runs on the message thread)
    }
};

// SwapTable DestroyFn of SwapSlot::LiveTake 15/15 (message thread): gives back the engine's reference.
inline void releaseCaptureHub(const void* hub) noexcept
{
    if (hub != nullptr)
        static_cast<const CaptureHub*>(hub)->release();
}
} // namespace core::sampling
