// Streaming 24-bit PCM WAV writer (spec 02-features/sampling.md SMP-007 "24-bit WAV at the engine
// rate, streamed to disk while recording"; 64 KB chunks). Float → S24 little-endian (×2^23, rounded to the
// nearest, clamped to −2^23 … 2^23 − 1: services::library::convertPcm reads it back exactly), interleaved, 1 or 2
// channels, a plain 44-byte RIFF/WAVE header (PCM, format 1) whose sizes finish() patches.
//
// Crash safety (PRJ-013 style): the file is written under a hidden part name next to the target
// (".<name>.part": the library listing skips dot files, LIB-003) and finish() flushes, fsyncs, renames it to the target
// and fsyncs the folder, so the library never lists a half-written take. abandon() deletes the part file.
//
// Memory: one fixed 64 KB conversion buffer, allocated in open(); nothing grows with the recording (RAM stays flat).
// Not thread-safe; the Recorder thread or a worker owns one writer at a time.
#pragma once

#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace services::sampling
{
class WavWriter
{
public:
    static constexpr std::size_t kChunkBytes = 64 * 1024;

    WavWriter() = default;
    ~WavWriter() { abandon(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    core::Result<void> open(const juce::File& target, double sampleRate, int channels);
    // l / r planar (r ignored for 1 channel; r null = l). Writes full 64 KB chunks as they fill.
    core::Result<void> write(const float* l, const float* r, std::int64_t frames);
    core::Result<void> finish(); // flush, sizes, fsync, rename into place, fsync the folder
    void abandon();              // close and delete the part file (no-op when not open)

    bool isOpen() const noexcept { return file_ != nullptr; }
    std::int64_t frames() const noexcept { return frames_; }
    const juce::File& target() const noexcept { return target_; }
    const juce::File& partFile() const noexcept { return part_; }

    static juce::File partFor(const juce::File& target); // ".<name>.part"
    // NFR-010: a take whose recording was cut by a crash or a power loss is a ".<name>.part" with a
    // placeholder header (sizes 0). salvage() runs at start-up over a recordings folder: each such part file with this
    // writer's header (RIFF/WAVE, PCM, 24-bit, 1-2 channels) is cut to whole frames, its RIFF / data sizes patched,
    // fsync'ed and renamed to <name> (or "<stem> recovered[ N].wav" when <name> exists), the folder fsync'ed; a part
    // file without one whole frame is deleted; anything else is left alone. One line per file, for the log.
    static std::vector<std::string> salvage(const juce::File& folder);
    // One frame of S24 (tests): x → 3 bytes LE.
    static std::int32_t toS24(float x) noexcept;

private:
    core::Result<void> flushChunk();
    bool writeHeader(std::uint32_t dataBytes);

    std::FILE* file_ = nullptr;
    juce::File target_, part_;
    double rate_ = 44100.0;
    int channels_ = 2;
    std::int64_t frames_ = 0;
    std::vector<unsigned char> chunk_;
    std::size_t used_ = 0;
};
} // namespace services::sampling
