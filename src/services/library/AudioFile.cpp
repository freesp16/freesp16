#include "services/library/AudioFile.h"

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace services::library
{
namespace
{
using core::Result;

constexpr int kMaxChannels = 64; // more is refused as damaged
constexpr int kMaxChunks = 4096; // a walk never visits more chunks than this
constexpr double kMinRate = 1000, kMaxRate = 768000;

std::uint32_t le32(const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t le16(const unsigned char* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}
std::uint32_t be32(const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t>(p[3]) | static_cast<std::uint32_t>(p[2]) << 8 |
           static_cast<std::uint32_t>(p[1]) << 16 | static_cast<std::uint32_t>(p[0]) << 24;
}
std::uint16_t be16(const unsigned char* p) noexcept
{
    return static_cast<std::uint16_t>(p[1] | p[0] << 8);
}
bool tagIs(const unsigned char* p, const char* tag) noexcept
{
    return std::memcmp(p, tag, 4) == 0;
}

Result<AudioFileInfo> damaged(const char* detail)
{
    return Result<AudioFileInfo>::failure("Damaged file", detail);
}
Result<AudioFileInfo> unsupported(const char* detail)
{
    return Result<AudioFileInfo>::failure("Unsupported format (or copy-protected)", detail);
}

bool readExact(juce::InputStream& s, void* dst, int n)
{
    return s.read(dst, n) == n;
}

// IEEE 754 80-bit extended (AIFF COMM sample rate), big-endian.
double extended80(const unsigned char* p) noexcept
{
    const int exponent = ((p[0] & 0x7F) << 8) | p[1];
    std::uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
        mantissa = mantissa << 8 | p[2 + i];
    if (exponent == 0 && mantissa == 0)
        return 0;
    if (exponent == 0x7FFF)
        return 0; // inf / NaN: refused by the rate check
    const double v = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return (p[0] & 0x80) != 0 ? -v : v;
}

Result<AudioFileInfo> finish(AudioFileInfo info, std::int64_t declaredFrames)
{
    if (info.channels < 1 || info.channels > kMaxChannels)
        return damaged("channel count");
    if (! (info.sampleRate >= kMinRate && info.sampleRate <= kMaxRate))
        return damaged("sample rate");
    if (info.bytesPerFrame <= 0)
        return damaged("frame size");
    const std::int64_t present = info.dataBytes / info.bytesPerFrame;
    info.frames = declaredFrames >= 0 && declaredFrames < present ? declaredFrames : present;
    info.truncated = declaredFrames > present;
    if (info.loopStart && info.loopEnd &&
        ! (*info.loopStart >= 0 && *info.loopStart < *info.loopEnd && *info.loopEnd <= info.frames))
        info.loopStart.reset(), info.loopEnd.reset();
    return info;
}

Result<AudioFileInfo> probeWav(juce::InputStream& s, std::int64_t fileSize)
{
    AudioFileInfo info;
    info.container = AudioContainer::Wav;
    bool haveFmt = false, haveData = false;
    std::int64_t pos = 12;
    for (int chunk = 0; chunk < kMaxChunks && pos + 8 <= fileSize; ++chunk)
    {
        unsigned char h[8];
        if (! s.setPosition(pos) || ! readExact(s, h, 8))
            break;
        const std::int64_t size = le32(h + 4);
        const std::int64_t body = pos + 8;
        const std::int64_t avail = std::min<std::int64_t>(size, fileSize - body);
        if (tagIs(h, "fmt "))
        {
            unsigned char f[40]{};
            if (avail < 16)
                return damaged("fmt chunk too short");
            if (! readExact(s, f, static_cast<int>(std::min<std::int64_t>(avail, 40))))
                return damaged("fmt chunk");
            int tag = le16(f);
            const int channels = le16(f + 2);
            const std::uint32_t rate = le32(f + 4);
            const int blockAlign = le16(f + 12);
            const int bits = le16(f + 14);
            if (tag == 0xFFFE)
            {
                if (avail < 40)
                    return damaged("extensible fmt too short");
                tag = le16(f + 24); // SubFormat GUID, first two bytes
            }
            if (channels < 1 || channels > kMaxChannels || blockAlign <= 0 || blockAlign % channels != 0)
                return damaged("fmt layout");
            const int bytes = blockAlign / channels;
            if (tag == 1)
            {
                if (bytes < 1 || bytes > 4 || bits < 1 || bits > bytes * 8)
                    return unsupported("PCM sample size");
                info.encoding = bytes == 1   ? PcmEncoding::U8
                                : bytes == 2 ? PcmEncoding::S16
                                : bytes == 3 ? PcmEncoding::S24
                                             : PcmEncoding::S32;
            }
            else if (tag == 3)
            {
                if (bytes != 4 && bytes != 8)
                    return unsupported("float sample size");
                info.encoding = bytes == 4 ? PcmEncoding::F32 : PcmEncoding::F64;
            }
            else
                return unsupported("WAV format tag");
            info.channels = channels;
            info.sampleRate = rate;
            info.bitsPerSample = bytes * 8;
            info.bytesPerFrame = blockAlign;
            haveFmt = true;
        }
        else if (tagIs(h, "data"))
        {
            if (! haveData)
            {
                info.dataOffset = body;
                // A streamed WAV may carry 0 or 0xFFFFFFFF; a short file a size beyond its end: the data runs to EOF.
                const bool open = size == 0 || size == 0xFFFFFFFFLL;
                info.dataBytes = open ? fileSize - body : avail;
                info.truncated = ! open && size > avail;
                haveData = true;
            }
        }
        else if (tagIs(h, "smpl") && avail >= 36 + 24)
        {
            unsigned char m[36 + 24];
            if (readExact(s, m, static_cast<int>(sizeof m)) && le32(m + 28) > 0)
            {
                info.loopStart = le32(m + 36 + 8);
                info.loopEnd = static_cast<std::int64_t>(le32(m + 36 + 12)) + 1; // smpl end is inclusive
            }
        }
        if (size == 0xFFFFFFFFLL || (tagIs(h, "data") && size == 0))
            break; // open-ended data: nothing follows
        pos = body + size + (size & 1);
    }
    if (! haveFmt)
        return damaged("no fmt chunk");
    if (! haveData)
        return damaged("no data chunk");
    const bool truncated = info.truncated;
    auto r = finish(info, -1);
    if (r.ok())
        r.value().truncated = r.value().truncated || truncated;
    return r;
}

Result<AudioFileInfo> probeAiff(juce::InputStream& s, std::int64_t fileSize, bool aifc)
{
    AudioFileInfo info;
    info.container = AudioContainer::Aiff;
    info.bigEndian = true;
    bool haveComm = false, haveData = false;
    std::int64_t declaredFrames = -1;
    struct Marker
    {
        std::uint16_t id;
        std::uint32_t pos;
    };
    Marker markers[64];
    int markerCount = 0;
    int sustainBegin = -1, sustainEnd = -1;
    std::int64_t pos = 12;
    for (int chunk = 0; chunk < kMaxChunks && pos + 8 <= fileSize; ++chunk)
    {
        unsigned char h[8];
        if (! s.setPosition(pos) || ! readExact(s, h, 8))
            break;
        const std::int64_t size = be32(h + 4);
        const std::int64_t body = pos + 8;
        const std::int64_t avail = std::min<std::int64_t>(size, fileSize - body);
        if (tagIs(h, "COMM"))
        {
            unsigned char c[22]{};
            if (avail < 18 || ! readExact(s, c, static_cast<int>(std::min<std::int64_t>(avail, 22))))
                return damaged("COMM chunk");
            const int channels = be16(c);
            declaredFrames = be32(c + 2);
            const int bits = be16(c + 6);
            const double rate = extended80(c + 8);
            if (channels < 1 || channels > kMaxChannels || bits < 1 || bits > 64)
                return damaged("COMM layout");
            int bytes = (bits + 7) / 8;
            info.encoding = bytes == 1   ? PcmEncoding::S8
                            : bytes == 2 ? PcmEncoding::S16
                            : bytes == 3 ? PcmEncoding::S24
                                         : PcmEncoding::S32;
            if (bytes > 4 && ! aifc)
                return unsupported("AIFF sample size");
            if (aifc)
            {
                if (avail < 22)
                    return damaged("AIFC COMM too short");
                const unsigned char* t = c + 18;
                const bool integer = tagIs(t, "NONE") || tagIs(t, "twos") || tagIs(t, "sowt");
                if (integer && bytes > 4)
                    return unsupported("AIFC sample size");
                if (integer)
                    info.bigEndian = ! tagIs(t, "sowt");
                else if (tagIs(t, "fl32") || tagIs(t, "FL32"))
                    info.encoding = PcmEncoding::F32, bytes = 4;
                else if (tagIs(t, "fl64") || tagIs(t, "FL64"))
                    info.encoding = PcmEncoding::F64, bytes = 8;
                else if (tagIs(t, "in24"))
                    info.encoding = PcmEncoding::S24, bytes = 3;
                else if (tagIs(t, "in32"))
                    info.encoding = PcmEncoding::S32, bytes = 4;
                else if (tagIs(t, "raw "))
                    info.encoding = PcmEncoding::U8, bytes = 1;
                else
                    return unsupported("AIFC compression");
            }
            info.channels = channels;
            info.sampleRate = rate;
            info.bitsPerSample = bytes * 8;
            info.bytesPerFrame = bytes * channels;
            haveComm = true;
        }
        else if (tagIs(h, "SSND") && ! haveData)
        {
            unsigned char o[8];
            if (avail < 8 || ! readExact(s, o, 8))
                return damaged("SSND chunk");
            const std::int64_t offset = be32(o);
            if (offset > avail - 8)
                return damaged("SSND offset");
            info.dataOffset = body + 8 + offset;
            info.dataBytes = avail - 8 - offset;
            info.truncated = size > avail;
            haveData = true;
        }
        else if (tagIs(h, "MARK") && avail >= 2)
        {
            unsigned char n[2];
            if (readExact(s, n, 2))
            {
                const int count = be16(n);
                std::int64_t left = avail - 2;
                for (int i = 0; i < count && markerCount < 64 && left >= 7; ++i)
                {
                    unsigned char m[7];
                    if (! readExact(s, m, 7))
                        break;
                    const int nameLen = m[6];
                    const int skip = (1 + nameLen + 1) & ~1; // pstring padded to even, its length byte read
                    markers[markerCount++] = {be16(m), be32(m + 2)};
                    left -= 6 + skip;
                    if (skip > 1)
                        s.skipNextBytes(skip - 1);
                }
            }
        }
        else if (tagIs(h, "INST") && avail >= 20)
        {
            unsigned char in[20];
            if (readExact(s, in, 20) && be16(in + 8) != 0) // sustainLoop.playMode != NoLooping
            {
                sustainBegin = be16(in + 10);
                sustainEnd = be16(in + 12);
            }
        }
        pos = body + size + (size & 1);
    }
    if (! haveComm)
        return damaged("no COMM chunk");
    if (! haveData)
    {
        if (declaredFrames == 0)
            info.dataOffset = 0, info.dataBytes = 0, haveData = true;
        else
            return damaged("no SSND chunk");
    }
    for (int i = 0; i < markerCount; ++i)
    {
        if (markers[i].id == sustainBegin)
            info.loopStart = markers[i].pos;
        if (markers[i].id == sustainEnd)
            info.loopEnd = markers[i].pos;
    }
    const bool truncated = info.truncated;
    auto r = finish(info, declaredFrames);
    if (r.ok())
        r.value().truncated = r.value().truncated || truncated;
    return r;
}

template <typename Load>
void convertWith(const unsigned char* src, int stride, int srcChannels, std::int64_t frames, float* const* out,
                 int outChannels, Load load) noexcept
{
    const int bps = stride;
    const int frameBytes = bps * srcChannels;
    if (srcChannels <= 2)
    {
        for (int c = 0; c < outChannels; ++c)
        {
            float* dst = out[c];
            const unsigned char* p = src + c * bps;
            for (std::int64_t i = 0; i < frames; ++i, p += frameBytes)
                dst[i] = load(p);
        }
        return;
    }
    const int evens = (srcChannels + 1) / 2, odds = srcChannels / 2;
    const float ge = 1.0f / static_cast<float>(evens), go = 1.0f / static_cast<float>(odds);
    for (std::int64_t i = 0; i < frames; ++i)
    {
        const unsigned char* p = src + i * frameBytes;
        float l = 0, r = 0;
        for (int c = 0; c < srcChannels; ++c, p += bps)
            (c % 2 == 0 ? l : r) += load(p);
        out[0][i] = l * ge;
        if (outChannels > 1)
            out[1][i] = r * go;
    }
}

// Fast paths the compiler vectorises (the common 16/24-bit little-endian cases).
void s16le(const unsigned char* src, int srcChannels, std::int64_t frames, float* const* out) noexcept
{
    constexpr float k = 1.0f / 32768.0f;
    if (srcChannels == 1)
    {
        float* d = out[0];
        for (std::int64_t i = 0; i < frames; ++i)
        {
            std::int16_t v;
            std::memcpy(&v, src + 2 * i, 2);
            d[i] = static_cast<float>(v) * k;
        }
        return;
    }
    float* l = out[0];
    float* r = out[1];
    for (std::int64_t i = 0; i < frames; ++i)
    {
        std::int16_t v[2];
        std::memcpy(v, src + 4 * i, 4);
        l[i] = static_cast<float>(v[0]) * k;
        r[i] = static_cast<float>(v[1]) * k;
    }
}
} // namespace

int bytesPerSample(PcmEncoding e) noexcept
{
    switch (e)
    {
    case PcmEncoding::U8:
    case PcmEncoding::S8:
        return 1;
    case PcmEncoding::S16:
        return 2;
    case PcmEncoding::S24:
        return 3;
    case PcmEncoding::S32:
    case PcmEncoding::F32:
        return 4;
    case PcmEncoding::F64:
        return 8;
    }
    return 0;
}

Result<AudioFileInfo> probeAudio(juce::InputStream& s)
{
    const std::int64_t fileSize = s.getTotalLength();
    unsigned char h[12];
    if (fileSize < 12 || ! s.setPosition(0) || ! readExact(s, h, 12))
        return damaged("shorter than a header");
    if (tagIs(h, "RIFF") && tagIs(h + 8, "WAVE"))
        return probeWav(s, fileSize);
    if (tagIs(h, "FORM") && (tagIs(h + 8, "AIFF") || tagIs(h + 8, "AIFC")))
        return probeAiff(s, fileSize, tagIs(h + 8, "AIFC"));
    if (tagIs(h, "RIFX") || tagIs(h, "RF64"))
        return unsupported("RIFX/RF64");
    return unsupported("not a WAV or AIFF file");
}

namespace
{
// The first 4 KB of a file in one read (the headers of almost every WAV/AIFF), the rest from a FileInputStream opened
// only when a chunk walk goes further: a listing of 10,000 files costs one open + read each (LIB-010 budget).
class HeadStream final : public juce::InputStream
{
public:
    explicit HeadStream(const juce::File& f) : file_(f)
    {
        const int fd = ::open(f.getFullPathName().toRawUTF8(), O_RDONLY);
        if (fd < 0)
            return;
        struct stat st{};
        if (::fstat(fd, &st) == 0)
        {
            size_ = static_cast<std::int64_t>(st.st_size);
            const ssize_t n = ::read(fd, head_, sizeof head_);
            headLen_ = n > 0 ? static_cast<int>(n) : 0;
            ok_ = true;
        }
        ::close(fd);
    }
    bool ok() const noexcept { return ok_; }
    juce::int64 getTotalLength() override { return size_; }
    bool isExhausted() override { return pos_ >= size_; }
    juce::int64 getPosition() override { return pos_; }
    bool setPosition(juce::int64 p) override
    {
        pos_ = std::clamp<juce::int64>(p, 0, size_);
        return true;
    }
    int read(void* dest, int n) override
    {
        if (n <= 0)
            return 0;
        if (pos_ + n <= headLen_)
        {
            std::memcpy(dest, head_ + pos_, static_cast<std::size_t>(n));
            pos_ += n;
            return n;
        }
        if (rest_ == nullptr)
        {
            rest_ = std::make_unique<juce::FileInputStream>(file_);
            if (! rest_->openedOk())
                return 0;
        }
        if (! rest_->setPosition(pos_))
            return 0;
        const int got = rest_->read(dest, n);
        pos_ += got > 0 ? got : 0;
        return got;
    }

private:
    juce::File file_;
    unsigned char head_[4096];
    int headLen_ = 0;
    juce::int64 size_ = 0, pos_ = 0;
    bool ok_ = false;
    std::unique_ptr<juce::FileInputStream> rest_;
};
} // namespace

Result<AudioFileInfo> probeAudioFile(const juce::File& file)
{
    HeadStream in(file);
    if (! in.ok())
        return Result<AudioFileInfo>::failure("File not found", file.getFullPathName().toStdString());
    return probeAudio(in);
}

bool isAudioFileName(const juce::String& name)
{
    if (name.startsWithChar('.'))
        return false;
    const auto ext = juce::File::createFileWithoutCheckingPath(name).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff" || ext == ".aifc";
}

void convertPcm(const unsigned char* src, PcmEncoding enc, bool be, int srcChannels, std::int64_t frames,
                float* const* out, int outChannels) noexcept
{
    if (frames <= 0)
        return;
    const int bps = bytesPerSample(enc);
    if (enc == PcmEncoding::S16 && ! be && srcChannels <= 2)
        return s16le(src, srcChannels, frames, out);
    switch (enc)
    {
    case PcmEncoding::U8:
        return convertWith(src, bps, srcChannels, frames, out, outChannels, [](const unsigned char* p)
                           { return (static_cast<float>(p[0]) - 128.0f) * (1.0f / 128.0f); });
    case PcmEncoding::S8:
        return convertWith(src, bps, srcChannels, frames, out, outChannels, [](const unsigned char* p)
                           { return static_cast<float>(static_cast<std::int8_t>(p[0])) * (1.0f / 128.0f); });
    case PcmEncoding::S16:
        return convertWith(src, bps, srcChannels, frames, out, outChannels,
                           [be](const unsigned char* p)
                           {
                               const auto v = static_cast<std::int16_t>(be ? be16(p) : le16(p));
                               return static_cast<float>(v) * (1.0f / 32768.0f);
                           });
    case PcmEncoding::S24:
        return convertWith(
            src, bps, srcChannels, frames, out, outChannels,
            [be](const unsigned char* p)
            {
                const std::uint32_t u =
                    be ? (std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8)
                       : (std::uint32_t(p[2]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[0]) << 8);
                return static_cast<float>(static_cast<std::int32_t>(u)) * (1.0f / 2147483648.0f);
            });
    case PcmEncoding::S32:
        return convertWith(src, bps, srcChannels, frames, out, outChannels,
                           [be](const unsigned char* p)
                           {
                               const std::uint32_t u = be ? be32(p) : le32(p);
                               return static_cast<float>(static_cast<double>(static_cast<std::int32_t>(u)) *
                                                         (1.0 / 2147483648.0));
                           });
    case PcmEncoding::F32:
        return convertWith(src, bps, srcChannels, frames, out, outChannels,
                           [be](const unsigned char* p)
                           {
                               const std::uint32_t u = be ? be32(p) : le32(p);
                               float f;
                               std::memcpy(&f, &u, 4);
                               return std::isfinite(f) ? f
                                                       : 0.0f; // a NaN/inf in a damaged file never reaches the engine
                           });
    case PcmEncoding::F64:
        return convertWith(src, bps, srcChannels, frames, out, outChannels,
                           [be](const unsigned char* p)
                           {
                               std::uint64_t u = 0;
                               for (int i = 0; i < 8; ++i)
                                   u |= static_cast<std::uint64_t>(p[be ? i : 7 - i]) << (56 - 8 * i);
                               double d;
                               std::memcpy(&d, &u, 8);
                               return std::isfinite(d) ? static_cast<float>(d) : 0.0f;
                           });
    }
}
} // namespace services::library
