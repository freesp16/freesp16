// WAV and AIFF/AIFC headers and PCM conversion (LIB-020/021): RIFF WAVE (PCM 8/16/24/32, IEEE float 32/64,
// WAVE_FORMAT_EXTENSIBLE), AIFF (PCM 8–32, big-endian) and AIFC (NONE/twos, sowt, fl32, fl64, in24, in32, raw).
//
// An own parser instead of juce_audio_formats: the app's JUCE library (cmake/Juce.cmake) does not build
// juce_audio_formats (nor juce_audio_basics on the host), the device binary is near its 8 MB budget, and the
// header-only listing, the bounded parse (fuzzed: no read past what a chunk declares, no allocation from an untrusted
// size before it is checked against the file) and the streaming hash all need the chunk walk anyway. Every format
// JUCE's WavAudioFormat/AiffAudioFormat read for LIB-020 is covered; compressed/unknown codings (ADPCM, µ-law, DRM
// wrappers) are refused with a message.
//
// Worker threads only.
#pragma once

#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <optional>

namespace services::library
{
enum class AudioContainer : std::uint8_t
{
    Wav,
    Aiff
};
enum class PcmEncoding : std::uint8_t
{
    U8,
    S8,
    S16,
    S24,
    S32,
    F32,
    F64
};

struct AudioFileInfo
{
    AudioContainer container = AudioContainer::Wav;
    PcmEncoding encoding = PcmEncoding::S16;
    bool bigEndian = false;
    int channels = 0;
    double sampleRate = 0;
    int bitsPerSample = 0; // as stored (container bits)
    int bytesPerFrame = 0;
    std::int64_t frames = 0; // frames actually present in the file
    std::int64_t dataOffset = 0, dataBytes = 0;
    bool truncated = false;                         // the header promises more frames than the file holds
    std::optional<std::int64_t> loopStart, loopEnd; // first sustain/sample loop, source frames (WAV smpl, AIFF INST)

    double seconds() const noexcept { return sampleRate > 0 ? static_cast<double>(frames) / sampleRate : 0.0; }
};

// Header only (no sample data is read): `stream` is positioned anywhere; the parse seeks from 0.
core::Result<AudioFileInfo> probeAudio(juce::InputStream& stream);
core::Result<AudioFileInfo> probeAudioFile(const juce::File& file);

// The library's audio file names (LIB-010): .wav .wave .aif .aiff .aifc, any case.
bool isAudioFileName(const juce::String& name);

int bytesPerSample(PcmEncoding) noexcept;

// Converts `frames` interleaved frames of `srcChannels` channels at `src` into planar float (−1…1) at out[0..outCh):
// srcChannels ≤ 2 → copied channel by channel (outCh == srcChannels); more → mixed down to stereo, out[0] = mean of
// the even-indexed channels, out[1] = mean of the odd-indexed ones (our LIB-021 rule, noted in the DecodeReport).
void convertPcm(const unsigned char* src, PcmEncoding, bool bigEndian, int srcChannels, std::int64_t frames,
                float* const* out, int outChannels) noexcept;
} // namespace services::library
