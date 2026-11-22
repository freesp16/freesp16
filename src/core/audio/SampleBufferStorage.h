// Storage of core::audio::SampleBuffer (LIB-030/031): one aligned allocation per buffer holding the header and
// the planar channels, each with SampleBuffer::pad zero frames before 0 and after the end, every ch[c] 16-byte aligned
// (NEON loads). A mono buffer sets ch[1] = ch[0], so a stereo reader needs no branch.
//
// Life cycle (threading.md "Object swap protocol"):
//   worker          allocateSampleBuffer → fill writableChannel(…) → set contentId → hand to the pool (refs = 0)
//   any thread      retain / release (SampleBuffer.h; wait-free, RT-safe; the audio thread only decrements)
//   owner (pool)    freeSampleBuffer once refs == 0 — never on the audio thread. A release that reaches 0 bumps
//                   zeroReleaseCount(), so the owner's collector knows a sweep is worth it (the retire path: the
//                   audio thread never frees, never touches the allocator).
#pragma once

#include "core/audio/SampleBuffer.h"

#include <cstddef>
#include <cstdint>

namespace core::audio
{
// A zero-filled buffer of `frames` × `channels` (1 or 2) at `sampleRate`, refs = 0, contentId = 0; nullptr when
// out of memory or the size overflows. Non-real-time threads only.
SampleBuffer* allocateSampleBuffer(std::int64_t frames, int channels, double sampleRate) noexcept;
// The writable channel c of a buffer this module allocated (before it is published).
float* writableChannel(SampleBuffer* buffer, int c) noexcept;
// Frees a buffer from allocateSampleBuffer (nullptr is ignored). Precondition: refs == 0, no reader left.
void freeSampleBuffer(const SampleBuffer* buffer) noexcept;
// Bytes the allocation holds (the memory meter counts these).
std::size_t sampleBufferBytes(const SampleBuffer* buffer) noexcept;
std::size_t sampleBufferBytesFor(std::int64_t frames, int channels) noexcept;

// Number of releases (process-wide) that brought a count to 0. Monotonic; collectors compare it with the value of
// their last sweep.
std::uint64_t zeroReleaseCount() noexcept;

// Deleter for std::unique_ptr<SampleBuffer, SampleBufferDeleter> (a buffer not yet published).
struct SampleBufferDeleter
{
    void operator()(const SampleBuffer* b) const noexcept { freeSampleBuffer(b); }
};
} // namespace core::audio
