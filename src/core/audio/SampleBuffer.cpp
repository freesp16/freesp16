// core::audio::retain / release and the buffer storage (SampleBuffer.h, SampleBufferStorage.h).
#include "core/audio/SampleBuffer.h"
#include "core/audio/SampleBufferStorage.h"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>

namespace core::audio
{
namespace
{
constexpr std::size_t kAlign = 16;
constexpr std::size_t kFloatsPerAlign = kAlign / sizeof(float);
static_assert(SampleBuffer::pad % kFloatsPerAlign == 0, "pad keeps ch[c] 16-byte aligned");

// The allocation: this header, then channel 0 [pad | frames | pad, rounded to 16 bytes], then channel 1.
struct alignas(kAlign) Block
{
    SampleBuffer buffer;
    std::size_t bytes;
    std::size_t stride; // floats per channel, a multiple of kFloatsPerAlign
};
static_assert(offsetof(Block, buffer) == 0, "a SampleBuffer* is its Block*");
constexpr std::size_t kHeader = (sizeof(Block) + kAlign - 1) / kAlign * kAlign;

std::atomic<std::uint64_t> zeroReleases{0};

std::size_t strideFor(std::int64_t frames) noexcept
{
    const auto f = static_cast<std::size_t>(frames) + 2 * SampleBuffer::pad;
    return (f + kFloatsPerAlign - 1) / kFloatsPerAlign * kFloatsPerAlign;
}

Block* blockOf(const SampleBuffer* b) noexcept
{
    return static_cast<Block*>(static_cast<void*>(const_cast<SampleBuffer*>(b))); // offset 0, Block-aligned
}
} // namespace

void retain(const SampleBuffer* b) noexcept
{
    if (b != nullptr)
        b->refs.fetch_add(1, std::memory_order_relaxed);
}

void release(const SampleBuffer* b) noexcept
{
    if (b == nullptr)
        return;
    const std::uint32_t before = b->refs.fetch_sub(1, std::memory_order_acq_rel);
    assert(before > 0 && "release without a matching retain");
    if (before == 1)
        zeroReleases.fetch_add(1, std::memory_order_release); // the owner's collector frees it (never this thread)
}

std::uint64_t zeroReleaseCount() noexcept
{
    return zeroReleases.load(std::memory_order_acquire);
}

std::size_t sampleBufferBytesFor(std::int64_t frames, int channels) noexcept
{
    if (frames < 0 || channels < 1 || channels > 2)
        return 0;
    constexpr auto maxFrames = static_cast<std::int64_t>(std::numeric_limits<std::size_t>::max() / 16);
    if (frames > maxFrames)
        return 0;
    const std::size_t stride = strideFor(frames);
    return kHeader + stride * sizeof(float) * static_cast<std::size_t>(channels);
}

SampleBuffer* allocateSampleBuffer(std::int64_t frames, int channels, double sampleRate) noexcept
{
    const std::size_t bytes = sampleBufferBytesFor(frames, channels);
    if (bytes == 0)
        return nullptr;
    void* memory = ::operator new(bytes, std::align_val_t{kAlign}, std::nothrow);
    if (memory == nullptr)
        return nullptr;
    auto* block = new (memory) Block{};
    block->bytes = bytes;
    block->stride = strideFor(frames);
    auto* data = static_cast<float*>(static_cast<void*>(static_cast<unsigned char*>(memory) + kHeader)); // 16-aligned
    std::memset(data, 0, block->stride * sizeof(float) * static_cast<std::size_t>(channels));
    SampleBuffer& b = block->buffer;
    b.frames = frames;
    b.channels = static_cast<std::uint8_t>(channels);
    b.sampleRate = sampleRate;
    b.ch[0] = data + SampleBuffer::pad;
    b.ch[1] = channels == 2 ? data + block->stride + SampleBuffer::pad : b.ch[0];
    b.contentId = 0;
    b.refs.store(0, std::memory_order_relaxed);
    return &b;
}

float* writableChannel(SampleBuffer* b, int c) noexcept
{
    return b != nullptr && c >= 0 && c < b->channels ? const_cast<float*>(b->ch[c]) : nullptr;
}

void freeSampleBuffer(const SampleBuffer* b) noexcept
{
    if (b == nullptr)
        return;
    assert(b->refs.load(std::memory_order_acquire) == 0 && "freeing a buffer that is still referenced");
    Block* block = blockOf(b);
    block->~Block();
    ::operator delete(static_cast<void*>(block), std::align_val_t{kAlign});
}

std::size_t sampleBufferBytes(const SampleBuffer* b) noexcept
{
    return b != nullptr ? blockOf(b)->bytes : 0;
}
} // namespace core::audio
