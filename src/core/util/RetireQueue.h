// Deferred destruction of objects the audio thread stops using (threading.md "Object swap protocol", step 3–4):
// the audio thread installs a new object at a block start and retires the old one here; a non-RT thread (a worker, or
// the message thread via EngineBridge::retirePending) frees it after a grace period of >= 2 engine blocks, so no
// object is ever freed — or its memory touched by the allocator — on the audio thread (NFR-020/021).
//
// Audio thread:  retire(ptr, &destroyFn, blockIndex) or retireObject(ptr, blockIndex)   — wait-free, no allocation
// Collector:     collect(currentBlockIndex)                                            — frees what is old enough
// Exactly one producer (the audio thread) and one collector thread per queue.
#pragma once

#include "core/util/SpscQueue.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace core
{
struct RetiredObject
{
    const void* object = nullptr;
    void (*destroy)(const void*) noexcept = nullptr;
    std::uint64_t retiredAtBlock = 0;
};

template <std::size_t Capacity = 1024>
class RetireQueue
{
public:
    static constexpr std::uint64_t kGraceBlocks = 2; // threading.md: ">= 2 blocks"

    // Audio thread. False when the queue is full: the caller keeps the object installed and retries next block.
    bool retire(const void* object, void (*destroy)(const void*) noexcept, std::uint64_t blockIndex) noexcept
    {
        return object == nullptr || queue_.push(RetiredObject {object, destroy, blockIndex});
    }

    template <typename T>
    bool retireObject(const T* object, std::uint64_t blockIndex) noexcept
    {
        return retire(object, [](const void* p) noexcept { delete static_cast<const T*>(p); }, blockIndex);
    }

    // Collector thread (never the audio thread): destroys every object retired at least kGraceBlocks blocks before
    // `currentBlock`; younger ones stay pending. Returns the number destroyed.
    std::size_t collect(std::uint64_t currentBlock, std::uint64_t graceBlocks = kGraceBlocks)
    {
        RetiredObject r;
        while (queue_.pop(r))
            pending_.push_back(r);
        std::size_t freed = 0, kept = 0;
        for (auto& p : pending_)
        {
            if (currentBlock >= p.retiredAtBlock + graceBlocks)
            {
                if (p.destroy != nullptr)
                    p.destroy(p.object);
                ++freed;
            }
            else
                pending_[kept++] = p;
        }
        pending_.resize(kept);
        return freed;
    }

    // Collector thread: number of objects popped but not yet old enough.
    std::size_t pending() const noexcept { return pending_.size(); }

private:
    SpscQueue<RetiredObject, Capacity> queue_;
    std::vector<RetiredObject> pending_; // collector side only
};
} // namespace core
