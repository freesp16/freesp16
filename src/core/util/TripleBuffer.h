// Triple-buffered snapshot from one writer thread to one reader thread (threading.md "Audio → UI: triple-buffered
// snapshots": playhead, meters, voice activity). Wait-free on both sides, never allocates, never blocks the writer:
// the writer fills its private buffer and publishes it; the reader takes the newest published buffer (intermediate
// snapshots are skipped, which is what a 60 Hz display wants). NFR-020/021: RT-safe on the writer side.
//
// Writer (audio thread):  T& s = buffer.write(); fill *every* field of s; buffer.publish();
//   write() returns a buffer whose previous content is unspecified (an older snapshot): write the whole snapshot.
// Reader (UI thread):     const T& s = buffer.read();   // valid until the next read() on this thread
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace core
{
template <typename T>
class TripleBuffer
{
    static_assert(std::is_trivially_copyable_v<T>, "snapshots are plain data");

public:
    TripleBuffer() noexcept = default;
    explicit TripleBuffer(const T& initial) noexcept { buffers_.fill(initial); }

    // Writer thread only.
    T& write() noexcept { return buffers_[back_]; }
    void publish() noexcept
    {
        const auto previous = middle_.exchange(static_cast<std::uint8_t>(back_ | kFresh), std::memory_order_acq_rel);
        back_ = static_cast<std::uint8_t>(previous & kIndex);
    }

    // Reader thread only. Returns the newest published snapshot (or the last one read when nothing new arrived).
    const T& read() const noexcept
    {
        if ((middle_.load(std::memory_order_relaxed) & kFresh) != 0)
        {
            const auto previous = middle_.exchange(front_, std::memory_order_acq_rel);
            front_ = static_cast<std::uint8_t>(previous & kIndex);
        }
        return buffers_[front_];
    }
    // Reader thread only: true when a snapshot was published since the last read().
    bool fresh() const noexcept { return (middle_.load(std::memory_order_acquire) & kFresh) != 0; }

private:
    static constexpr std::uint8_t kIndex = 3, kFresh = 4;
    std::array<T, 3> buffers_ {};
    std::uint8_t back_ = 0;                        // writer's buffer
    mutable std::atomic<std::uint8_t> middle_ {1}; // exchanged buffer index | kFresh
    mutable std::uint8_t front_ = 2;               // reader's buffer
};
} // namespace core
