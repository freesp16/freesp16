// Test audio for the library tests and bench_decode: WAV/AIFF/AIFC files generated in memory from float samples
// (never device data), temp folders, and small signal helpers.
#pragma once

#include "services/library/AudioFile.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace libtest
{
using services::library::PcmEncoding;

enum class Container
{
    Wav,
    WavExtensible,
    Aiff,
    Aifc
};

struct Spec
{
    Container container = Container::Wav;
    PcmEncoding encoding = PcmEncoding::S16;
    bool littleEndianAifc = false; // AIFC 'sowt' (16-bit only)
    int channels = 1;
    double rate = 44100;
    std::int64_t loopStart = -1, loopEnd = -1; // WAV smpl / AIFF MARK+INST
};

inline double sampleMax(PcmEncoding e)
{
    switch (e)
    {
    case PcmEncoding::U8:
    case PcmEncoding::S8:
        return 128.0;
    case PcmEncoding::S16:
        return 32768.0;
    case PcmEncoding::S24:
        return 8388608.0;
    case PcmEncoding::S32:
        return 2147483648.0;
    default:
        return 1.0;
    }
}

// Quantises v (−1…1) the way the encoder stores it; the decoder must reproduce `quantised(v)` within 1 LSB.
inline double quantised(double v, PcmEncoding e)
{
    if (e == PcmEncoding::F32)
        return static_cast<double>(static_cast<float>(v));
    if (e == PcmEncoding::F64)
        return v;
    const double m = sampleMax(e);
    const double q = std::floor(v * m + 0.5);
    return std::clamp(q, -m, m - 1) / m;
}

class Bytes
{
public:
    std::vector<unsigned char> b;
    void tag(const char* t) { b.insert(b.end(), t, t + 4); }
    void le(std::uint64_t v, int n)
    {
        for (int i = 0; i < n; ++i)
            b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
    void be(std::uint64_t v, int n)
    {
        for (int i = n - 1; i >= 0; --i)
            b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
    void patchLe32(std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<unsigned char>(v >> (8 * i));
    }
    void patchBe32(std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<unsigned char>(v >> (8 * (3 - i)));
    }
};

inline void putSample(Bytes& out, double v, PcmEncoding e, bool bigEndian)
{
    auto put = [&](std::uint64_t u, int n) { bigEndian ? out.be(u, n) : out.le(u, n); };
    switch (e)
    {
    case PcmEncoding::U8:
        out.b.push_back(static_cast<unsigned char>(quantised(v, e) * 128.0 + 128.0));
        break;
    case PcmEncoding::S8:
        out.b.push_back(static_cast<unsigned char>(static_cast<std::int8_t>(quantised(v, e) * 128.0)));
        break;
    case PcmEncoding::S16:
        put(static_cast<std::uint16_t>(static_cast<std::int16_t>(quantised(v, e) * 32768.0)), 2);
        break;
    case PcmEncoding::S24:
        put(static_cast<std::uint32_t>(static_cast<std::int32_t>(quantised(v, e) * 8388608.0)) & 0xFFFFFF, 3);
        break;
    case PcmEncoding::S32:
        put(static_cast<std::uint32_t>(static_cast<std::int32_t>(quantised(v, e) * 2147483648.0)), 4);
        break;
    case PcmEncoding::F32:
    {
        const float f = static_cast<float>(v);
        std::uint32_t u;
        std::memcpy(&u, &f, 4);
        put(u, 4);
        break;
    }
    case PcmEncoding::F64:
    {
        std::uint64_t u;
        std::memcpy(&u, &v, 8);
        put(u, 8);
        break;
    }
    }
}

inline void extended80(Bytes& out, double rate)
{
    int exp = 0;
    const double m = std::frexp(rate, &exp); // rate = m × 2^exp, 0.5 ≤ m < 1
    const auto mant = static_cast<std::uint64_t>(std::ldexp(m, 64));
    out.be(static_cast<std::uint64_t>(16383 + exp - 1), 2);
    out.be(mant, 8);
}

// planar[c][i] in −1…1.
inline std::vector<unsigned char> encode(const Spec& s, const std::vector<std::vector<float>>& planar)
{
    const std::int64_t frames = planar.empty() ? 0 : static_cast<std::int64_t>(planar[0].size());
    const int bps = services::library::bytesPerSample(s.encoding);
    const bool isFloat = s.encoding == PcmEncoding::F32 || s.encoding == PcmEncoding::F64;
    Bytes o;
    if (s.container == Container::Wav || s.container == Container::WavExtensible)
    {
        const bool ext = s.container == Container::WavExtensible;
        o.tag("RIFF");
        o.le(0, 4);
        o.tag("WAVE");
        o.tag("fmt ");
        o.le(ext ? 40 : 16, 4);
        o.le(ext ? 0xFFFE : (isFloat ? 3 : 1), 2);
        o.le(static_cast<std::uint64_t>(s.channels), 2);
        o.le(static_cast<std::uint64_t>(s.rate), 4);
        o.le(static_cast<std::uint64_t>(s.rate) * static_cast<std::uint64_t>(bps * s.channels), 4);
        o.le(static_cast<std::uint64_t>(bps * s.channels), 2);
        o.le(static_cast<std::uint64_t>(bps * 8), 2);
        if (ext)
        {
            o.le(22, 2);
            o.le(static_cast<std::uint64_t>(bps * 8), 2);
            o.le(0, 4);
            o.le(isFloat ? 3 : 1, 2);
            const unsigned char guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
            o.b.insert(o.b.end(), guidTail, guidTail + 14);
        }
        o.tag("LIST"); // an unknown chunk before the data, odd-sized (padded)
        o.le(5, 4);
        o.tag("INFO");
        o.b.push_back('x');
        o.b.push_back(0);
        o.tag("data");
        o.le(static_cast<std::uint64_t>(frames * bps * s.channels), 4);
        for (std::int64_t i = 0; i < frames; ++i)
            for (int c = 0; c < s.channels; ++c)
                putSample(o, static_cast<double>(planar[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]),
                          s.encoding, false);
        if ((frames * bps * s.channels) & 1)
            o.b.push_back(0);
        if (s.loopStart >= 0)
        {
            o.tag("smpl");
            o.le(36 + 24, 4);
            for (int i = 0; i < 7; ++i)
                o.le(0, 4);
            o.le(1, 4); // loops
            o.le(0, 4); // sampler data
            o.le(0, 4); // cue id
            o.le(0, 4); // type forward
            o.le(static_cast<std::uint64_t>(s.loopStart), 4);
            o.le(static_cast<std::uint64_t>(s.loopEnd - 1), 4); // inclusive
            o.le(0, 4);
            o.le(0, 4);
        }
        o.patchLe32(4, static_cast<std::uint32_t>(o.b.size() - 8));
        return o.b;
    }
    const bool aifc = s.container == Container::Aifc;
    o.tag("FORM");
    o.be(0, 4);
    o.tag(aifc ? "AIFC" : "AIFF");
    o.tag("COMM");
    o.be(aifc ? 18 + 4 + 2 : 18, 4);
    o.be(static_cast<std::uint64_t>(s.channels), 2);
    o.be(static_cast<std::uint64_t>(frames), 4);
    o.be(static_cast<std::uint64_t>(bps * 8), 2);
    extended80(o, s.rate);
    bool big = true;
    if (aifc)
    {
        if (s.encoding == PcmEncoding::F32)
            o.tag("fl32");
        else if (s.encoding == PcmEncoding::F64)
            o.tag("fl64");
        else if (s.littleEndianAifc)
            o.tag("sowt"), big = false;
        else
            o.tag("NONE");
        o.b.push_back(0); // empty pstring + pad
        o.b.push_back(0);
    }
    if (s.loopStart >= 0)
    {
        o.tag("MARK");
        o.be(2 + 2 * 8, 4);
        o.be(2, 2);
        o.be(1, 2), o.be(static_cast<std::uint64_t>(s.loopStart), 4), o.b.push_back(0), o.b.push_back(0);
        o.be(2, 2), o.be(static_cast<std::uint64_t>(s.loopEnd), 4), o.b.push_back(0), o.b.push_back(0);
        o.tag("INST");
        o.be(20, 4);
        for (int i = 0; i < 8; ++i)
            o.b.push_back(0);
        o.be(1, 2), o.be(1, 2), o.be(2, 2); // sustain loop: forward, markers 1 → 2
        o.be(0, 2), o.be(0, 2), o.be(0, 2);
    }
    o.tag("SSND");
    o.be(static_cast<std::uint64_t>(8 + frames * bps * s.channels), 4);
    o.be(0, 4);
    o.be(0, 4);
    for (std::int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < s.channels; ++c)
            putSample(o, static_cast<double>(planar[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]),
                      s.encoding, big);
    if ((frames * bps * s.channels) & 1)
        o.b.push_back(0);
    o.patchBe32(4, static_cast<std::uint32_t>(o.b.size() - 8));
    return o.b;
}

inline std::vector<float> sine(std::int64_t frames, double hz, double rate, double amp = 0.5, double phase = 0)
{
    std::vector<float> v(static_cast<std::size_t>(frames));
    for (std::int64_t i = 0; i < frames; ++i)
        v[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2 * 3.14159265358979323846 * hz * static_cast<double>(i) / rate + phase));
    return v;
}

// Deterministic noise in −amp…amp (xorshift).
inline std::vector<float> noise(std::int64_t frames, std::uint32_t seed, double amp = 0.9)
{
    std::vector<float> v(static_cast<std::size_t>(frames));
    std::uint32_t x = seed * 2654435761u + 1;
    for (auto& s : v)
    {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        s = static_cast<float>(amp * (static_cast<double>(x) / 4294967295.0 * 2 - 1));
    }
    return v;
}

inline bool writeFile(const juce::File& f, const std::vector<unsigned char>& bytes)
{
    f.getParentDirectory().createDirectory();
    return f.replaceWithData(bytes.data(), bytes.size());
}

// A fresh folder under the system temp dir, removed on destruction.
class TempDir
{
public:
    explicit TempDir(const char* name = "lib")
        : dir_(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile(juce::String("sixteen-test-") + name + "-" +
                                 juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())))
    {
        dir_.createDirectory();
    }
    ~TempDir() { dir_.deleteRecursively(); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const juce::File& dir() const noexcept { return dir_; }
    juce::File operator/(const char* rel) const { return dir_.getChildFile(rel); }

private:
    juce::File dir_;
};
} // namespace libtest
