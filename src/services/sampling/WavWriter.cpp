#include "services/sampling/WavWriter.h"

#include "services/library/FileOps.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace services::sampling
{
namespace
{
void put16(unsigned char* p, std::uint32_t v)
{
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
}
void put32(unsigned char* p, std::uint32_t v)
{
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}
core::Error ioError(const char* what, const juce::File& f, int err)
{
    return {juce::String("Could not write the recording").toStdString(),
            std::string(what) + " " + f.getFullPathName().toStdString() + ": " + std::strerror(err)};
}
} // namespace

juce::File WavWriter::partFor(const juce::File& target)
{
    return target.getSiblingFile("." + target.getFileName() + ".part");
}

std::int32_t WavWriter::toS24(float x) noexcept
{
    const float s = x * 8388608.0f;
    if (! (s > -8388608.0f)) // also NaN → the floor (never happens on a valid ring)
        return -8388608;
    if (s >= 8388607.0f)
        return 8388607;
    return static_cast<std::int32_t>(s < 0.0f ? s - 0.5f : s + 0.5f);
}

core::Result<void> WavWriter::open(const juce::File& target, double sampleRate, int channels)
{
    abandon();
    target_ = target;
    part_ = partFor(target);
    rate_ = sampleRate > 0 ? sampleRate : 44100.0;
    channels_ = channels == 1 ? 1 : 2;
    frames_ = 0;
    used_ = 0;
    if (! target.getParentDirectory().createDirectory())
        return ioError("mkdir", target.getParentDirectory(), errno);
    file_ = std::fopen(part_.getFullPathName().toRawUTF8(), "wb");
    if (file_ == nullptr)
        return ioError("open", part_, errno);
    chunk_.assign(kChunkBytes, 0);
    if (! writeHeader(0))
    {
        const int err = errno;
        abandon();
        return ioError("header", part_, err);
    }
    return {};
}

bool WavWriter::writeHeader(std::uint32_t dataBytes)
{
    unsigned char h[44];
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1); // PCM
    put16(h + 22, static_cast<std::uint32_t>(channels_));
    const auto rate = static_cast<std::uint32_t>(rate_ + 0.5);
    put32(h + 24, rate);
    put32(h + 28, rate * 3u * static_cast<std::uint32_t>(channels_));
    put16(h + 32, 3u * static_cast<std::uint32_t>(channels_));
    put16(h + 34, 24);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, dataBytes);
    return std::fseek(file_, 0, SEEK_SET) == 0 && std::fwrite(h, 1, sizeof h, file_) == sizeof h;
}

core::Result<void> WavWriter::write(const float* l, const float* r, std::int64_t frames)
{
    if (file_ == nullptr)
        return core::Result<void>::failure("Could not write the recording", "not open");
    if (r == nullptr)
        r = l;
    const std::size_t bpf = 3u * static_cast<std::size_t>(channels_);
    for (std::int64_t i = 0; i < frames; ++i)
    {
        if (used_ + bpf > chunk_.size())
            if (auto ok = flushChunk(); ! ok)
                return ok;
        unsigned char* p = chunk_.data() + used_;
        const std::int32_t a = toS24(l[i]);
        p[0] = static_cast<unsigned char>(a);
        p[1] = static_cast<unsigned char>(a >> 8);
        p[2] = static_cast<unsigned char>(a >> 16);
        if (channels_ == 2)
        {
            const std::int32_t b = toS24(r[i]);
            p[3] = static_cast<unsigned char>(b);
            p[4] = static_cast<unsigned char>(b >> 8);
            p[5] = static_cast<unsigned char>(b >> 16);
        }
        used_ += bpf;
    }
    frames_ += frames;
    return {};
}

core::Result<void> WavWriter::flushChunk()
{
    if (used_ == 0)
        return {};
    if (std::fwrite(chunk_.data(), 1, used_, file_) != used_)
        return ioError("write", part_, errno);
    used_ = 0;
    return {};
}

core::Result<void> WavWriter::finish()
{
    if (file_ == nullptr)
        return core::Result<void>::failure("Could not write the recording", "not open");
    if (auto ok = flushChunk(); ! ok)
    {
        abandon();
        return ok;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(frames_) * 3u * static_cast<std::uint64_t>(channels_);
    if (bytes > 0xFFFFFFFFull - 36)
    {
        abandon();
        return core::Result<void>::failure("The recording is too long for a WAV file", "> 4 GB");
    }
    if (! writeHeader(static_cast<std::uint32_t>(bytes)) || std::fflush(file_) != 0 || ::fsync(::fileno(file_)) != 0)
    {
        const int err = errno;
        abandon();
        return ioError("finish", part_, err);
    }
    std::fclose(file_);
    file_ = nullptr;
    if (std::rename(part_.getFullPathName().toRawUTF8(), target_.getFullPathName().toRawUTF8()) != 0)
    {
        const int err = errno;
        part_.deleteFile();
        return ioError("rename", target_, err);
    }
    library::fsyncPath(target_.getParentDirectory());
    chunk_.clear();
    chunk_.shrink_to_fit();
    return {};
}

std::vector<std::string> WavWriter::salvage(const juce::File& folder)
{
    std::vector<std::string> lines;
    if (! folder.isDirectory())
        return lines;
    for (const auto& part : folder.findChildFiles(juce::File::findFiles, false, ".*.part"))
    {
        const juce::String name = part.getFileName();
        if (name.length() <= 6) // "." + at least one character + ".part"
            continue;
        const std::string path = part.getFullPathName().toStdString();
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        if (f == nullptr)
            continue;
        unsigned char h[44] = {};
        const std::size_t got = std::fread(h, 1, sizeof h, f);
        const auto u16 = [&h](int at) { return static_cast<std::uint32_t>(h[at] | h[at + 1] << 8); };
        const auto u32 = [&](int at) { return u16(at) | u16(at + 2) << 16; };
        const bool ours = got == sizeof h && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVEfmt ", 8) == 0 &&
                          u32(16) == 16 && u16(20) == 1 && (u16(22) == 1 || u16(22) == 2) && u16(34) == 24 &&
                          std::memcmp(h + 36, "data", 4) == 0;
        if (! ours)
        {
            std::fclose(f);
            if (got < sizeof h && part.getSize() == static_cast<juce::int64>(got)) // cut before the header was out
            {
                part.deleteFile();
                lines.push_back("SAMPLING SALVAGE " + name.toStdString() + " removed (no header)");
            }
            continue;
        }
        const std::uint64_t bpf = 3u * u16(22);
        std::uint64_t frames = (static_cast<std::uint64_t>(part.getSize()) - sizeof h) / bpf;
        frames = std::min<std::uint64_t>(frames, (0xFFFFFFFFull - 36) / bpf);
        if (frames == 0)
        {
            std::fclose(f);
            part.deleteFile();
            lines.push_back("SAMPLING SALVAGE " + name.toStdString() + " removed (no audio)");
            continue;
        }
        const auto bytes = static_cast<std::uint32_t>(frames * bpf);
        put32(h + 4, 36 + bytes);
        put32(h + 40, bytes);
        const bool ok = ::ftruncate(::fileno(f), static_cast<off_t>(sizeof h + bytes)) == 0 &&
                        std::fseek(f, 0, SEEK_SET) == 0 && std::fwrite(h, 1, sizeof h, f) == sizeof h &&
                        std::fflush(f) == 0 && ::fsync(::fileno(f)) == 0;
        std::fclose(f);
        if (! ok)
        {
            lines.push_back("SAMPLING SALVAGE " + name.toStdString() + " FAILED " + std::strerror(errno));
            continue;
        }
        const juce::String targetName = name.substring(1, name.length() - 5);
        juce::File target = folder.getChildFile(targetName);
        for (int k = 1; target.exists() && k < 1000; ++k)
            target = folder.getChildFile(juce::File(targetName).getFileNameWithoutExtension() + " recovered" +
                                         (k > 1 ? " " + juce::String(k) : juce::String()) + ".wav");
        if (target.exists() || std::rename(path.c_str(), target.getFullPathName().toRawUTF8()) != 0)
        {
            lines.push_back("SAMPLING SALVAGE " + name.toStdString() + " FAILED rename");
            continue;
        }
        library::fsyncPath(folder);
        lines.push_back("SAMPLING SALVAGE " + name.toStdString() + " -> " + target.getFileName().toStdString() +
                        " frames=" + std::to_string(frames) + " channels=" + std::to_string(u16(22)));
    }
    return lines;
}

void WavWriter::abandon()
{
    if (file_ != nullptr)
    {
        std::fclose(file_);
        file_ = nullptr;
        part_.deleteFile();
    }
}
} // namespace services::sampling
