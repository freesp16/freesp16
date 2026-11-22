// Bounded multi-producer / single-consumer lock-free queue (D. Vyukov's bounded queue, one consumer). Used where
// several non-RT producer threads feed the audio thread, e.g. the TriggerSink every pad/MIDI reader thread pushes
// TriggerNow into (UI-118). Producers never block (push fails when full); the consumer (audio thread) is wait-free.
// Never allocates after construction; trivially-copyable payloads only; capacity is a power of two. NFR-020/021.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace core
{
template <typename T, std::size_t Capacity>
class MpscQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0 && Capacity >= 2, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "payloads cross threads by copy");

public:
    MpscQueue() noexcept
    {
        for (std::size_t i = 0; i < Capacity; ++i)
            cells_[i].sequence.store(i, std::memory_order_relaxed);
    }

    // Any producer thread. False when full.
    bool push(const T& value) noexcept
    {
        auto pos = head_.load(std::memory_order_relaxed);
        for (;;)
        {
            auto& cell = cells_[pos & (Capacity - 1)];
            const auto seq = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0)
            {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.value = value;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
                return false; // full
            else
                pos = head_.load(std::memory_order_relaxed);
        }
    }

    // The single consumer thread.
    bool pop(T& out) noexcept
    {
        auto& cell = cells_[tail_ & (Capacity - 1)];
        const auto seq = cell.sequence.load(std::memory_order_acquire);
        if (static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(tail_ + 1) < 0)
            return false; // empty
        out = cell.value;
        cell.sequence.store(tail_ + Capacity, std::memory_order_release);
        ++tail_;
        return true;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct Cell
    {
        std::atomic<std::size_t> sequence {0};
        T value {};
    };
    alignas(64) std::array<Cell, Capacity> cells_ {};
    alignas(64) std::atomic<std::size_t> head_ {0};
    alignas(64) std::size_t tail_ = 0;
};
} // namespace core
