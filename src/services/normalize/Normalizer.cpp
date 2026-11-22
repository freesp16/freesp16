#include "services/normalize/Normalizer.h"

#include "services/library/AudioFile.h"
#include "services/render/AudioWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace services::normalize
{
namespace lib = services::library;
namespace rnd = services::render;

std::int64_t pcmCeiling(int bits, double dbfs) noexcept
{
    const double full = std::ldexp(1.0, bits - 1);
    const double c = std::floor(full * std::pow(10.0, dbfs / 20.0));
    return static_cast<std::int64_t>(std::clamp(c, 0.0, full - 1.0));
}

std::int32_t scalePcm(std::int32_t x, double gain, std::int64_t ceiling, std::int64_t peak) noexcept
{
    const double v = static_cast<double>(x) * gain;
    const double fl = std::floor(v);
    if (std::fabs(v - fl - 0.5) >= 1e-6)
        return static_cast<std::int32_t>(std::nearbyint(v)); // far from a half: the double product decides
    // Near a half: exact integer rounding of |x| · ceiling / peak, ties to even, sign restored.
    const std::int64_t num = (x < 0 ? -static_cast<std::int64_t>(x) : x) * ceiling;
    std::int64_t q = num / peak;
    const std::int64_t twice = 2 * (num - q * peak);
    if (twice > peak || (twice == peak && (q & 1) != 0))
        ++q;
    return static_cast<std::int32_t>(x < 0 ? -q : q);
}

void scalePcmBlock(std::span<std::int32_t> x, std::int64_t ceiling, std::int64_t peak) noexcept
{
    const double gain = static_cast<double>(ceiling) / static_cast<double>(peak);
    for (auto& v : x)
        v = scalePcm(v, gain, ceiling, peak);
}

std::int64_t peakOf(std::span<const std::int32_t> x) noexcept
{
    std::int64_t p = 0;
    for (const auto v : x)
        p = std::max<std::int64_t>(p, v < 0 ? -static_cast<std::int64_t>(v) : v);
    return p;
}

float peakOf(std::span<const float> x) noexcept
{
    float p = 0;
    for (const auto v : x)
        p = std::max(p, std::fabs(v));
    return p;
}

juce::File normalizedFileFor(const juce::File& in)
{
    return in.getSiblingFile(in.getFileNameWithoutExtension() + "_norm.wav");
}

namespace
{
constexpr int kChunkFrames = 65536;

// Raw interleaved samples → integer counts / floats.
void toCounts(const unsigned char* p, std::size_t n, lib::PcmEncoding e, bool big, std::int32_t* out) noexcept
{
    if (e == lib::PcmEncoding::S16)
        for (std::size_t i = 0; i < n; ++i, p += 2)
            out[i] = static_cast<std::int16_t>(big ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]));
    else
        for (std::size_t i = 0; i < n; ++i, p += 3)
        {
            const std::uint32_t u = big ? (std::uint32_t(p[0]) << 16 | std::uint32_t(p[1]) << 8 | p[2])
                                        : (std::uint32_t(p[2]) << 16 | std::uint32_t(p[1]) << 8 | p[0]);
            out[i] = static_cast<std::int32_t>(u << 8) >> 8; // sign-extend 24 bits
        }
}

void toFloats(const unsigned char* p, std::size_t n, bool big, float* out) noexcept
{
    for (std::size_t i = 0; i < n; ++i, p += 4)
    {
        const std::uint32_t u = big ? (std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 |
                                       std::uint32_t(p[2]) << 8 | p[3])
                                    : (std::uint32_t(p[3]) << 24 | std::uint32_t(p[2]) << 16 |
                                       std::uint32_t(p[1]) << 8 | p[0]);
        std::memcpy(&out[i], &u, 4);
    }
}

// Streams the data chunk in chunks of kChunkFrames: fn(raw, frames) per chunk; false on a read error.
template <typename Fn> bool forEachChunk(const juce::File& file, const lib::AudioFileInfo& info, Fn&& fn)
{
    juce::FileInputStream in(file);
    if (! in.openedOk() || ! in.setPosition(info.dataOffset))
        return false;
    std::vector<unsigned char> raw(static_cast<std::size_t>(kChunkFrames * info.bytesPerFrame));
    for (std::int64_t done = 0; done < info.frames;)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(kChunkFrames, info.frames - done));
        const int bytes = n * info.bytesPerFrame;
        if (in.read(raw.data(), bytes) != bytes)
            return false;
        fn(raw.data(), n);
        done += n;
    }
    return true;
}
} // namespace

core::Result<NormalizeReport> normalize(const juce::File& in, const juce::File& out, double targetDbfs)
{
    if (in == out)
        return core::Error {"Normalize needs a new file name", in.getFullPathName().toStdString()};
    auto probed = lib::probeAudioFile(in);
    if (! probed)
        return probed.error();
    const auto info = probed.value();
    NormalizeReport r;
    r.channels = info.channels;
    r.sampleRate = info.sampleRate;
    r.frames = info.frames;
    r.targetDbfs = std::clamp(targetDbfs, -6.0, 0.0);
    // REN-013: refused before anything is written.
    switch (info.encoding)
    {
        case lib::PcmEncoding::S16: r.encoding = Encoding::Pcm16; break;
        case lib::PcmEncoding::S24: r.encoding = Encoding::Pcm24; break;
        case lib::PcmEncoding::F32: r.encoding = Encoding::Float32; break;
        default: return core::Error {"Normalize supports PCM 16/24-bit and 32-bit float only", "encoding"};
    }
    if (info.channels < 1 || info.channels > 2)
        return core::Error {"Normalize supports mono and stereo files only", "channels"};
    const auto rate = std::lround(info.sampleRate);
    if (std::fabs(info.sampleRate - static_cast<double>(rate)) > 1e-6 || (rate != 44100 && rate != 48000 && rate != 96000))
        return core::Error {"Normalize supports 44.1, 48 and 96 kHz files only", "rate"};
    if (info.bytesPerFrame != info.channels * lib::bytesPerSample(info.encoding) || info.truncated)
        return core::Error {"This file cannot be normalized", info.truncated ? "truncated" : "frame layout"};

    const std::size_t maxSamples = static_cast<std::size_t>(kChunkFrames * info.channels);
    const bool isFloat = r.encoding == Encoding::Float32;
    std::vector<std::int32_t> counts(isFloat ? 0 : maxSamples);
    std::vector<float> floats(isFloat ? maxSamples : 0);

    // Pass 1: the peak.
    std::int64_t peakCounts = 0;
    float peakFloat = 0;
    const bool read1 = forEachChunk(in, info, [&](const unsigned char* raw, int frames) {
        const auto n = static_cast<std::size_t>(frames * info.channels);
        if (isFloat)
        {
            toFloats(raw, n, info.bigEndian, floats.data());
            peakFloat = std::max(peakFloat, peakOf(std::span<const float>(floats.data(), n)));
        }
        else
        {
            toCounts(raw, n, info.encoding, info.bigEndian, counts.data());
            peakCounts = std::max(peakCounts, peakOf(std::span<const std::int32_t>(counts.data(), n)));
        }
    });
    if (! read1)
        return core::Error {"Cannot read the file", in.getFullPathName().toStdString()};
    if (isFloat && ! std::isfinite(peakFloat))
        return core::Error {"This file cannot be normalized", "non-finite samples"};

    const float floatTarget = static_cast<float>(std::pow(10.0, r.targetDbfs / 20.0));
    if (isFloat)
    {
        r.peakBefore = peakFloat;
        r.silence = peakFloat == 0.0f;
        r.atTarget = peakFloat == floatTarget;
        r.gain = r.silence ? 1.0 : static_cast<double>(floatTarget) / static_cast<double>(peakFloat);
    }
    else
    {
        r.ceiling = pcmCeiling(r.encoding == Encoding::Pcm16 ? 16 : 24, r.targetDbfs);
        r.peakBefore = static_cast<double>(peakCounts);
        r.silence = peakCounts == 0;
        r.atTarget = peakCounts == r.ceiling;
        r.gain = r.silence ? 1.0 : static_cast<double>(r.ceiling) / static_cast<double>(peakCounts);
    }
    if (r.silence || r.atTarget) // REN-012: unchanged, nothing written
    {
        r.gain = 1.0;
        r.peakAfter = r.peakBefore;
        return r;
    }

    // Pass 2: one gain, written through a part file.
    rnd::AudioWriterSpec spec {isFloat ? rnd::SampleFormat::Float32
                                       : (r.encoding == Encoding::Pcm16 ? rnd::SampleFormat::Pcm16
                                                                        : rnd::SampleFormat::Pcm24),
                               info.channels, info.sampleRate, rnd::isAiffName(out)};
    auto writer = rnd::AudioFileWriter::create(out, spec);
    if (! writer)
        return writer.error();
    auto& w = *writer.value();
    double after = 0;
    bool wrote = true;
    const bool read2 = forEachChunk(in, info, [&](const unsigned char* raw, int frames) {
        const auto n = static_cast<std::size_t>(frames * info.channels);
        if (isFloat)
        {
            toFloats(raw, n, info.bigEndian, floats.data());
            for (std::size_t i = 0; i < n; ++i)
                floats[i] = static_cast<float>(static_cast<double>(floats[i]) * r.gain);
            after = std::max(after, static_cast<double>(peakOf(std::span<const float>(floats.data(), n))));
            wrote = w.writeFloats(floats.data(), frames) && wrote;
        }
        else
        {
            toCounts(raw, n, info.encoding, info.bigEndian, counts.data());
            scalePcmBlock(std::span<std::int32_t>(counts.data(), n), r.ceiling, peakCounts);
            after = std::max(after, static_cast<double>(peakOf(std::span<const std::int32_t>(counts.data(), n))));
            wrote = w.writeCounts(counts.data(), frames) && wrote;
        }
    });
    if (! read2 || ! wrote)
        return core::Error {read2 ? "Cannot write the file" : "Cannot read the file",
                            (read2 ? out : in).getFullPathName().toStdString()}; // the writer deletes its part file
    if (auto done = w.finish(); ! done)
        return done.error();
    r.peakAfter = after;
    r.changed = true;
    return r;
}
} // namespace services::normalize
