// Streaming WAV / AIFF writer of the render and normalize services (REN-002 bit depths 16 / 24 / 32f; REN-013).
//
// Own writer instead of JUCE's WavAudioFormat: the app's JUCE library does not build
// juce_audio_formats (services/library/AudioFile.h, binary budget), and the reader side is already the
// library's own parser. WAV: RIFF/WAVE, `fmt ` PCM (tag 1) for 16/24 bits, IEEE float (tag 3, + `fact`) for 32f.
// AIFF: FORM/AIFF COMM + SSND big-endian PCM for 16/24 bits; AIFC `fl32` for float. Sizes are patched in finish().
// The file is written to `<target>.part` and renamed onto the target by finish() (fsync of the part, an atomic rename
// over an existing target, fsync of the folder: a crash or power cut leaves the old or the new file); a
// writer destroyed before finish() deletes the part file, so no partial file ever carries the target's name.
//
// Float → PCM (render output): y = clamp(nearbyint(x · 2^(bits−1)), −2^(bits−1), 2^(bits−1) − 1), no dither.
// Worker threads (any thread but audio).
#pragma once

#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace services::render
{
enum class SampleFormat : std::uint8_t { Pcm16, Pcm24, Float32 };

struct AudioWriterSpec
{
    SampleFormat format = SampleFormat::Pcm24;
    int channels = 2;          // 1 or 2
    double sampleRate = 44100;
    bool aiff = false;         // false = WAV
};

// 16 → Pcm16, 24 → Pcm24, 32 → Float32 (REN-002 "bit depth 16/24/32f"); anything else → Pcm24.
SampleFormat formatForBitDepth(int bits) noexcept;
// .aif / .aiff / .aifc (any case) = AIFF.
bool isAiffName(const juce::File&);

class AudioFileWriter
{
public:
    static core::Result<std::unique_ptr<AudioFileWriter>> create(const juce::File& target, const AudioWriterSpec&);
    ~AudioFileWriter(); // without finish(): the part file is deleted
    AudioFileWriter(const AudioFileWriter&) = delete;
    AudioFileWriter& operator= (const AudioFileWriter&) = delete;

    // Interleaved integer counts (PCM formats; not left-justified: −32768…32767 for PCM16).
    bool writeCounts(const std::int32_t* interleaved, int frames);
    // Interleaved floats as stored (Float32 format only).
    bool writeFloats(const float* interleaved, int frames);
    // Planar floats −1…1, converted to the format (PCM: the rounding of the header). ch[1] ignored for mono.
    bool writePlanar(const float* const* ch, int frames);

    core::Result<void> finish(); // patches the sizes, flushes, renames onto the target
    std::int64_t framesWritten() const noexcept { return frames_; }
    const AudioWriterSpec& spec() const noexcept { return spec_; }

private:
    AudioFileWriter(const juce::File& target, const AudioWriterSpec&);
    bool writeHeader();
    bool put(const void* data, std::size_t bytes);

    juce::File target_, part_;
    AudioWriterSpec spec_;
    std::unique_ptr<juce::FileOutputStream> out_;
    std::vector<unsigned char> scratch_;
    std::int64_t frames_ = 0;
    bool ok_ = true, finished_ = false;
};
} // namespace services::render
