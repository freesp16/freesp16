#include "services/library/Decoder.h"

#include "services/library/Hash.h"
#include "services/library/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

namespace services::library
{
namespace
{
using core::Result;
constexpr std::size_t kChunkBytes = 256 * 1024;

struct HashSeed // the format the PCM bytes are read with, and the rate the buffer is made for
{
    std::uint32_t encoding, bigEndian, channels, pad;
    std::uint64_t sourceRateMilliHz, engineRateMilliHz, frames;
};

std::int64_t framesWanted(const AudioFileInfo& info, double maxSeconds) noexcept
{
    if (maxSeconds <= 0)
        return info.frames;
    return std::min(info.frames, static_cast<std::int64_t>(std::ceil(maxSeconds * info.sampleRate)));
}

bool sameRate(double a, double b) noexcept
{
    return std::fabs(a - b) < 1e-6;
}

std::int64_t outFramesFor(const AudioFileInfo& info, std::int64_t frames, double engineRate)
{
    if (sameRate(info.sampleRate, engineRate) || frames == 0)
        return frames;
    return Resampler(info.sampleRate, engineRate).outputFrames(frames);
}

std::string format(const char* fmt, double a, double b = 0)
{
    char buf[160];
    std::snprintf(buf, sizeof buf, fmt, a, b);
    return buf;
}
} // namespace

std::size_t Decoder::decodedBytes(const AudioFileInfo& info, double engineRate, double maxSeconds) noexcept
{
    const std::int64_t frames = framesWanted(info, maxSeconds);
    const double out = sameRate(info.sampleRate, engineRate)
                           ? static_cast<double>(frames)
                           : std::ceil(static_cast<double>(frames) * engineRate / info.sampleRate);
    return core::audio::sampleBufferBytesFor(static_cast<std::int64_t>(out), std::min(info.channels, 2));
}

Result<DecodedAudio> Decoder::decode(const juce::File& file, double engineRate, DecodeReport& report)
{
    return decode(file, engineRate, report, {});
}

Result<DecodedAudio> Decoder::decode(const juce::File& file, double engineRate, DecodeReport& report,
                                     const DecodeOptions& options)
{
    juce::FileInputStream in(file);
    if (! in.openedOk())
        return Result<DecodedAudio>::failure("File not found", file.getFullPathName().toStdString());
    return decode(in, engineRate, report, options);
}

Result<DecodedAudio> Decoder::decode(juce::InputStream& stream, double engineRate, DecodeReport& report,
                                     const DecodeOptions& options)
{
    auto probed = probeAudio(stream);
    if (! probed)
        return probed.error();
    const AudioFileInfo info = probed.value();
    report.source = info;
    if (! (engineRate >= 8000 && engineRate <= 384000))
        return Result<DecodedAudio>::failure("Cannot load the sample", "engine rate out of range");

    const std::int64_t frames = framesWanted(info, options.maxSeconds);
    const int srcChannels = info.channels;
    const int channels = std::min(srcChannels, 2);
    const bool resample = ! sameRate(info.sampleRate, engineRate) && frames > 0;
    const std::int64_t outFrames = outFramesFor(info, frames, engineRate);
    const std::size_t bytes = core::audio::sampleBufferBytesFor(outFrames, channels);
    const std::size_t scratch = resample ? static_cast<std::size_t>(frames) * sizeof(float) * channels : 0;
    if (bytes == 0 || (options.limitBytes > 0 && bytes + scratch > static_cast<std::size_t>(options.limitBytes)))
        return Result<DecodedAudio>::failure("Not enough memory for this sample",
                                             std::to_string(bytes + scratch) + " bytes needed");

    report.partial = frames < info.frames;
    report.truncated = info.truncated;
    report.mixedDown = srcChannels > 2;
    report.resampled = resample;
    report.longSample = static_cast<double>(frames) / info.sampleRate > kLongSampleSeconds;
    if (report.mixedDown)
        report.notes.push_back(std::to_string(srcChannels) + " channels mixed down to stereo");
    if (report.resampled)
        report.notes.push_back(format("Resampled from %.0f Hz to %.0f Hz", info.sampleRate, engineRate));
    if (report.truncated)
        report.notes.push_back("The file is shorter than its header says: the available audio was loaded");
    if (report.longSample)
        report.notes.push_back(format("Long sample (%.1f min): it uses %.0f MB of memory",
                                      static_cast<double>(frames) / info.sampleRate / 60.0,
                                      static_cast<double>(bytes) / (1024.0 * 1024.0)));

    SampleBufferPtr buffer(core::audio::allocateSampleBuffer(outFrames, channels, engineRate));
    if (buffer == nullptr)
        return Result<DecodedAudio>::failure("Not enough memory for this sample", "allocation failed");

    std::vector<float> native[2];
    float* dst[2] = {nullptr, nullptr};
    try
    {
        for (int c = 0; c < channels; ++c)
        {
            if (resample)
            {
                native[c].assign(static_cast<std::size_t>(frames), 0.0f);
                dst[c] = native[c].data();
            }
            else
                dst[c] = core::audio::writableChannel(buffer.get(), c);
        }
    }
    catch (const std::bad_alloc&)
    {
        return Result<DecodedAudio>::failure("Not enough memory for this sample", "scratch allocation failed");
    }

    const HashSeed seedFields{static_cast<std::uint32_t>(info.encoding),
                              info.bigEndian ? 1u : 0u,
                              static_cast<std::uint32_t>(srcChannels),
                              0,
                              static_cast<std::uint64_t>(std::llround(info.sampleRate * 1000)),
                              static_cast<std::uint64_t>(std::llround(engineRate * 1000)),
                              static_cast<std::uint64_t>(frames)};
    Xxh64 hash(Xxh64::of(&seedFields, sizeof seedFields));

    const int frameBytes = info.bytesPerFrame;
    const std::int64_t chunkFrames = std::max<std::int64_t>(1, static_cast<std::int64_t>(kChunkBytes) / frameBytes);
    std::vector<unsigned char> chunk(static_cast<std::size_t>(chunkFrames * frameBytes));
    if (! stream.setPosition(info.dataOffset))
        return Result<DecodedAudio>::failure("Damaged file", "cannot seek to the audio data");
    std::int64_t done = 0;
    while (done < frames)
    {
        if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed))
            return Result<DecodedAudio>::failure("Cancelled");
        const std::int64_t want = std::min(chunkFrames, frames - done);
        const int got = stream.read(chunk.data(), static_cast<int>(want * frameBytes));
        const std::int64_t gotFrames = got > 0 ? got / frameBytes : 0;
        if (gotFrames <= 0)
        {
            report.truncated = true; // the rest stays silent (the buffer is zero-filled)
            break;
        }
        hash.update(chunk.data(), static_cast<std::size_t>(gotFrames * frameBytes));
        float* out[2] = {dst[0] + done, channels > 1 ? dst[1] + done : nullptr};
        convertPcm(chunk.data(), info.encoding, info.bigEndian, srcChannels, gotFrames, out, channels);
        done += gotFrames;
        if (options.progress != nullptr)
            options.progress->store(static_cast<float>(done) / static_cast<float>(frames) * (resample ? 0.5f : 1.0f),
                                    std::memory_order_relaxed);
    }

    if (resample)
    {
        const Resampler rs(info.sampleRate, engineRate);
        for (int c = 0; c < channels; ++c)
        {
            if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed))
                return Result<DecodedAudio>::failure("Cancelled");
            rs.process(native[c].data(), frames, core::audio::writableChannel(buffer.get(), c), outFrames);
        }
    }
    if (options.progress != nullptr)
        options.progress->store(1.0f, std::memory_order_relaxed);

    buffer->contentId = hash.digest();
    DecodedAudio result;
    const double scale = engineRate / info.sampleRate;
    if (info.loopStart && info.loopEnd && *info.loopEnd <= frames)
    {
        result.loopStart = std::llround(static_cast<double>(*info.loopStart) * scale);
        result.loopEnd = std::min<std::int64_t>(outFrames, std::llround(static_cast<double>(*info.loopEnd) * scale));
    }
    result.buffer = std::move(buffer);
    return result;
}
} // namespace services::library
