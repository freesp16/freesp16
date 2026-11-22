#include "services/render/AudioWriter.h"

#include "services/library/FileOps.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace services::render
{
namespace
{
int bytesOf(SampleFormat f) noexcept
{
    return f == SampleFormat::Pcm16 ? 2 : (f == SampleFormat::Pcm24 ? 3 : 4);
}

// 80-bit IEEE 754 extended (AIFF COMM sampleRate), for positive integral rates.
void extended80(double v, unsigned char out[10]) noexcept
{
    std::memset(out, 0, 10);
    if (v <= 0)
        return;
    int exponent = 0;
    const double m = std::frexp(v, &exponent); // v = m · 2^exponent, 0.5 ≤ m < 1
    const int e = exponent - 1 + 16383;
    const auto mant = static_cast<std::uint64_t>(std::ldexp(m, 64)); // the leading 1 at bit 63
    out[0] = static_cast<unsigned char>((e >> 8) & 0x7F);
    out[1] = static_cast<unsigned char>(e & 0xFF);
    for (int i = 0; i < 8; ++i)
        out[2 + i] = static_cast<unsigned char>((mant >> (56 - 8 * i)) & 0xFF);
}

void le16(std::vector<unsigned char>& b, std::uint32_t v)
{
    b.push_back(static_cast<unsigned char>(v));
    b.push_back(static_cast<unsigned char>(v >> 8));
}
void le32(std::vector<unsigned char>& b, std::uint32_t v)
{
    le16(b, v & 0xFFFF);
    le16(b, v >> 16);
}
void be16(std::vector<unsigned char>& b, std::uint32_t v)
{
    b.push_back(static_cast<unsigned char>(v >> 8));
    b.push_back(static_cast<unsigned char>(v));
}
void be32(std::vector<unsigned char>& b, std::uint32_t v)
{
    be16(b, v >> 16);
    be16(b, v & 0xFFFF);
}
void tag(std::vector<unsigned char>& b, const char* t)
{
    b.insert(b.end(), t, t + 4);
}
} // namespace

SampleFormat formatForBitDepth(int bits) noexcept
{
    return bits == 16 ? SampleFormat::Pcm16 : (bits == 32 ? SampleFormat::Float32 : SampleFormat::Pcm24);
}

bool isAiffName(const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".aif" || ext == ".aiff" || ext == ".aifc";
}

AudioFileWriter::AudioFileWriter(const juce::File& target, const AudioWriterSpec& spec)
    : target_(target), part_(target.getSiblingFile(target.getFileName() + ".part")), spec_(spec)
{
}

AudioFileWriter::~AudioFileWriter()
{
    if (! finished_)
    {
        out_.reset();
        part_.deleteFile();
    }
}

core::Result<std::unique_ptr<AudioFileWriter>> AudioFileWriter::create(const juce::File& target,
                                                                       const AudioWriterSpec& spec)
{
    if (spec.channels < 1 || spec.channels > 2 || spec.sampleRate < 1000 || spec.sampleRate > 384000)
        return core::Error {"Cannot write this audio format", "channels/rate out of range"};
    std::unique_ptr<AudioFileWriter> w(new AudioFileWriter(target, spec));
    if (! target.getParentDirectory().createDirectory())
        return core::Error {"Cannot write the file", target.getParentDirectory().getFullPathName().toStdString()};
    w->part_.deleteFile();
    w->out_ = std::make_unique<juce::FileOutputStream>(w->part_);
    if (! w->out_->openedOk() || ! w->writeHeader())
        return core::Error {"Cannot write the file", w->part_.getFullPathName().toStdString()};
    return w;
}

bool AudioFileWriter::writeHeader()
{
    // Placeholder sizes (0); finish() rewrites the header with the real ones (same length).
    std::vector<unsigned char> h;
    const int bytes = bytesOf(spec_.format), ch = spec_.channels;
    const auto rate = static_cast<std::uint32_t>(std::lround(spec_.sampleRate));
    const auto frames = static_cast<std::uint32_t>(frames_);
    const auto data = static_cast<std::uint32_t>(frames_ * bytes * ch);
    const bool isFloat = spec_.format == SampleFormat::Float32;
    if (! spec_.aiff)
    {
        tag(h, "RIFF");
        le32(h, (isFloat ? 4 + 26 + 12 : 4 + 24) + 8 + data);
        tag(h, "WAVE");
        tag(h, "fmt ");
        le32(h, isFloat ? 18 : 16);
        le16(h, isFloat ? 3 : 1);
        le16(h, static_cast<std::uint32_t>(ch));
        le32(h, rate);
        le32(h, rate * static_cast<std::uint32_t>(bytes * ch));
        le16(h, static_cast<std::uint32_t>(bytes * ch));
        le16(h, static_cast<std::uint32_t>(bytes * 8));
        if (isFloat)
        {
            le16(h, 0); // cbSize
            tag(h, "fact");
            le32(h, 4);
            le32(h, frames);
        }
        tag(h, "data");
        le32(h, data);
    }
    else
    {
        unsigned char ext[10];
        extended80(spec_.sampleRate, ext);
        const std::uint32_t comm = isFloat ? 18 + 4 + 6 : 18; // AIFC: + compression type + its 6-byte name
        tag(h, "FORM");
        be32(h, 4 + (isFloat ? 12 : 0) + 8 + comm + 8 + 8 + data + (data & 1u));
        tag(h, isFloat ? "AIFC" : "AIFF");
        if (isFloat)
        {
            tag(h, "FVER");
            be32(h, 4);
            be32(h, 0xA2805140u); // AIFC version 1
        }
        tag(h, "COMM");
        be32(h, comm);
        be16(h, static_cast<std::uint32_t>(ch));
        be32(h, frames);
        be16(h, static_cast<std::uint32_t>(bytes * 8));
        h.insert(h.end(), ext, ext + 10);
        if (isFloat)
        {
            tag(h, "fl32");
            // compressionName: a Pascal string, padded to an even length: "\x04" "fl32" "\0" = 6 bytes.
            h.push_back(4);
            tag(h, "fl32");
            h.push_back(0);
        }
        tag(h, "SSND");
        be32(h, 8 + data);
        be32(h, 0); // offset
        be32(h, 0); // block size
    }
    return put(h.data(), h.size());
}

bool AudioFileWriter::put(const void* data, std::size_t bytes)
{
    if (! ok_)
        return false;
    ok_ = out_->write(data, bytes);
    return ok_;
}

bool AudioFileWriter::writeCounts(const std::int32_t* x, int frames)
{
    if (spec_.format == SampleFormat::Float32 || frames <= 0)
        return frames == 0 && ok_;
    const int bytes = bytesOf(spec_.format);
    const std::size_t n = static_cast<std::size_t>(frames) * static_cast<std::size_t>(spec_.channels);
    scratch_.resize(n * static_cast<std::size_t>(bytes));
    unsigned char* p = scratch_.data();
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto v = static_cast<std::uint32_t>(x[i]);
        if (! spec_.aiff)
            for (int k = 0; k < bytes; ++k)
                *p++ = static_cast<unsigned char>(v >> (8 * k));
        else
            for (int k = bytes - 1; k >= 0; --k)
                *p++ = static_cast<unsigned char>(v >> (8 * k));
    }
    frames_ += frames;
    return put(scratch_.data(), scratch_.size());
}

bool AudioFileWriter::writeFloats(const float* x, int frames)
{
    if (spec_.format != SampleFormat::Float32 || frames <= 0)
        return frames == 0 && ok_;
    const std::size_t n = static_cast<std::size_t>(frames) * static_cast<std::size_t>(spec_.channels);
    scratch_.resize(n * 4);
    unsigned char* p = scratch_.data();
    for (std::size_t i = 0; i < n; ++i)
    {
        std::uint32_t v;
        std::memcpy(&v, &x[i], 4);
        if (! spec_.aiff)
            for (int k = 0; k < 4; ++k)
                *p++ = static_cast<unsigned char>(v >> (8 * k));
        else
            for (int k = 3; k >= 0; --k)
                *p++ = static_cast<unsigned char>(v >> (8 * k));
    }
    frames_ += frames;
    return put(scratch_.data(), scratch_.size());
}

bool AudioFileWriter::writePlanar(const float* const* ch, int frames)
{
    const int c = spec_.channels;
    if (spec_.format == SampleFormat::Float32)
    {
        std::vector<float> inter(static_cast<std::size_t>(frames * c));
        for (int i = 0; i < frames; ++i)
            for (int k = 0; k < c; ++k)
                inter[static_cast<std::size_t>(i * c + k)] = ch[k][i];
        return writeFloats(inter.data(), frames);
    }
    const double scale = spec_.format == SampleFormat::Pcm16 ? 32768.0 : 8388608.0;
    const double lo = -scale, hi = scale - 1.0;
    std::vector<std::int32_t> inter(static_cast<std::size_t>(frames * c));
    for (int i = 0; i < frames; ++i)
        for (int k = 0; k < c; ++k)
            inter[static_cast<std::size_t>(i * c + k)] =
                static_cast<std::int32_t>(std::clamp(std::nearbyint(static_cast<double>(ch[k][i]) * scale), lo, hi));
    return writeCounts(inter.data(), frames);
}

core::Result<void> AudioFileWriter::finish()
{
    if (finished_)
        return {};
    const int bytes = bytesOf(spec_.format);
    const std::int64_t data = frames_ * bytes * spec_.channels;
    if (data > 0xFFFFFFF0ll - 128)
        return core::Error {"The render is too long for one file", "> 4 GB"};
    if (spec_.aiff && (data & 1) != 0)
    {
        const unsigned char pad = 0;
        put(&pad, 1); // chunks are even-sized
    }
    if (ok_)
    {
        ok_ = out_->setPosition(0);
        if (ok_)
            ok_ = writeHeader();
    }
    if (ok_)
    {
        out_->flush();
        ok_ = out_->getStatus().wasOk();
    }
    out_.reset();
    if (! ok_)
    {
        part_.deleteFile();
        finished_ = true;
        return core::Error {"Cannot write the file", target_.getFullPathName().toStdString()};
    }
    // NFR-010: flush() above fsync'ed the part file (JUCE FileOutputStream); rename() replaces an
    // existing render atomically (the old file stays complete until the new one carries its name — deleting it first
    // left a window with neither), then the folder is fsync'ed so the new name survives a power cut.
    finished_ = true;
    if (std::rename(part_.getFullPathName().toRawUTF8(), target_.getFullPathName().toRawUTF8()) != 0)
    {
        const int err = errno;
        part_.deleteFile();
        return core::Error {"Cannot write the file",
                            target_.getFullPathName().toStdString() + ": " + std::strerror(err)};
    }
    library::fsyncPath(target_.getParentDirectory());
    return {};
}
} // namespace services::render
