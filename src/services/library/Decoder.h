// Sample decoder (LIB-020/021/022): WAV/AIFF (AudioFile.h) → a SampleBuffer at the
// engine rate — mixed down to ≤ 2 channels, resampled (Resampler.h) when the file's rate differs, padded and 16-byte
// aligned (SampleBufferStorage.h), its contentId the XXH64 of the PCM bytes (seeded with the format and the engine
// rate) computed while reading. Damaged or unsupported files are refused with a message, never a crash (fuzzed).
// Above 10 minutes the load proceeds with a warning; above DecodeOptions::limitBytes it is refused.
//
// Worker threads only (never the audio thread, never the message thread for anything but tiny files in tests).
// Budget on one A9 core: 16-bit stereo 44.1 kHz ≥ 100× real time (60 s in ≤ 0.6 s), resampling ≥ 15× real time
// (bench_decode).
#pragma once

#include "core/audio/SampleBufferStorage.h"
#include "core/util/Result.h"
#include "services/library/AudioFile.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace services::library
{
using SampleBufferPtr = std::unique_ptr<core::audio::SampleBuffer, core::audio::SampleBufferDeleter>;

struct DecodeOptions
{
    double maxSeconds = 0;                     // > 0: decode at most this much from the start (preview: 30 s)
    std::int64_t limitBytes = 0;               // > 0: refuse when the decoded buffer would be larger
    const std::atomic<bool>* cancel = nullptr; // set → the decode stops with "Cancelled"
    std::atomic<float>* progress = nullptr;    // 0…1 while decoding
};

struct DecodeReport
{
    AudioFileInfo source{};
    std::vector<std::string> notes; // user-facing notes: mixdown, resampling, truncation, length warning
    bool mixedDown = false, resampled = false, truncated = false, longSample = false, partial = false;
};

struct DecodedAudio
{
    SampleBufferPtr buffer;                         // refs = 0; the pool publishes it
    std::optional<std::int64_t> loopStart, loopEnd; // the file's loop, in buffer (engine-rate) frames
};

class Decoder
{
public:
    static constexpr double kLongSampleSeconds = 600; // LIB-022 warning threshold

    static core::Result<DecodedAudio> decode(const juce::File&, double engineRate, DecodeReport&);
    static core::Result<DecodedAudio> decode(const juce::File&, double engineRate, DecodeReport&, const DecodeOptions&);
    static core::Result<DecodedAudio> decode(juce::InputStream&, double engineRate, DecodeReport&,
                                             const DecodeOptions&);

    // Bytes the decoded buffer of `info` takes at `engineRate` (pool limit checks before decoding).
    static std::size_t decodedBytes(const AudioFileInfo& info, double engineRate, double maxSeconds = 0) noexcept;
};
} // namespace services::library
