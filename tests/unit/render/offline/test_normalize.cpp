// Exact peak normalize (REN-010…013, REN-016). Every fixture is generated here (libtest::encode writes
// WAV / AIFF / AIFC from exact integer counts); the oracle is independent of services/normalize: it decodes the output
// with its own byte reader and checks every sample with integer arithmetic (PCM: |2·(y·P − x·C)| ≤ P, and a rational
// tie |2·(y·P − x·C)| = P must land on an even y) or long double (float: y is the float nearest to x·T/P).
#include "services/normalize/Normalizer.h"
#include "tests/unit/library/LibTestAudio.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace
{
using services::library::PcmEncoding;
namespace nz = services::normalize;

double fullScale(PcmEncoding e)
{
    return e == PcmEncoding::S16 ? 32768.0 : 8388608.0;
}

// The oracle's own decoder: interleaved samples of the data chunk as doubles in counts (PCM) or floats.
std::vector<double> decodeAll(const juce::File& f, services::library::AudioFileInfo& info)
{
    auto probed = services::library::probeAudioFile(f);
    REQUIRE(probed);
    info = probed.value();
    juce::MemoryBlock mb;
    REQUIRE(f.loadFileAsData(mb));
    const auto* p = static_cast<const unsigned char*>(mb.getData()) + info.dataOffset;
    const std::size_t n = static_cast<std::size_t>(info.frames * info.channels);
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (info.encoding == PcmEncoding::S16)
        {
            const unsigned char* q = p + 2 * i;
            const int v = info.bigEndian ? (q[0] << 8 | q[1]) : (q[1] << 8 | q[0]);
            out[i] = static_cast<double>(v >= 32768 ? v - 65536 : v);
        }
        else if (info.encoding == PcmEncoding::S24)
        {
            const unsigned char* q = p + 3 * i;
            const long v = info.bigEndian ? (long(q[0]) << 16 | long(q[1]) << 8 | q[2])
                                          : (long(q[2]) << 16 | long(q[1]) << 8 | q[0]);
            out[i] = static_cast<double>(v >= 8388608 ? v - 16777216 : v);
        }
        else
        {
            const unsigned char* q = p + 4 * i;
            const std::uint32_t u = info.bigEndian ? (std::uint32_t(q[0]) << 24 | std::uint32_t(q[1]) << 16 |
                                                      std::uint32_t(q[2]) << 8 | q[3])
                                                   : (std::uint32_t(q[3]) << 24 | std::uint32_t(q[2]) << 16 |
                                                      std::uint32_t(q[1]) << 8 | q[0]);
            float v;
            std::memcpy(&v, &u, 4);
            out[i] = v;
        }
    }
    return out;
}

// Integer counts with peak `peak` (both signs present), ties of x·C/P = k + ½ included where they exist.
std::vector<std::int64_t> countsWithPeak(std::int64_t peak, std::int64_t ceiling, std::size_t n, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::int64_t> d(-peak, peak);
    std::vector<std::int64_t> v(n);
    for (auto& x : v)
        x = d(rng);
    v[0] = peak;
    v[1] = -peak;
    // Ties: 2·x·C ≡ P (mod 2P).
    std::size_t at = 2;
    for (std::int64_t x = 1; x <= peak && at < n / 2; ++x)
        if ((2 * x * ceiling) % (2 * peak) == peak)
        {
            v[at++] = x;
            v[at++] = -x;
        }
    return v;
}

struct Fixture
{
    libtest::Spec spec;
    double target;
};

// Writes counts (PCM) or floats as planar float input of libtest::encode (k / 2^(bits−1) is exact in float).
juce::File writeFixture(const juce::File& f, const libtest::Spec& s, const std::vector<double>& interleaved)
{
    const auto frames = interleaved.size() / static_cast<std::size_t>(s.channels);
    std::vector<std::vector<float>> planar(static_cast<std::size_t>(s.channels), std::vector<float>(frames));
    const double m = s.encoding == PcmEncoding::F32 ? 1.0 : fullScale(s.encoding);
    for (std::size_t i = 0; i < frames; ++i)
        for (int c = 0; c < s.channels; ++c)
            planar[static_cast<std::size_t>(c)][i] =
                static_cast<float>(interleaved[i * static_cast<std::size_t>(s.channels) + static_cast<std::size_t>(c)] / m);
    REQUIRE(libtest::writeFile(f, libtest::encode(s, planar)));
    return f;
}

int checkFile(const libtest::Spec& s, double target, std::int64_t peak, std::size_t frames, std::uint32_t seed,
              const juce::File& dir, int& ties)
{
    const bool isFloat = s.encoding == PcmEncoding::F32;
    const std::int64_t ceiling = isFloat ? 0 : nz::pcmCeiling(s.encoding == PcmEncoding::S16 ? 16 : 24, target);
    std::vector<double> in;
    if (isFloat)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> d(-0.7f, 0.7f);
        in.resize(frames * static_cast<std::size_t>(s.channels));
        for (auto& x : in)
            x = d(rng);
        in[3] = 0.7f;
    }
    else
        for (const auto v : countsWithPeak(peak, ceiling, frames * static_cast<std::size_t>(s.channels), seed))
            in.push_back(static_cast<double>(v));
    const auto src = writeFixture(dir.getChildFile("in" + juce::String(seed) + (s.container == libtest::Container::Wav ? ".wav" : ".aif")), s, in);
    const auto dst = dir.getChildFile("out" + juce::String(seed) + ".wav");
    const auto r = nz::normalize(src, dst, target);
    REQUIRE(r);
    REQUIRE(r.value().changed);
    services::library::AudioFileInfo info;
    const auto out = decodeAll(dst, info);
    REQUIRE(info.encoding == s.encoding);
    REQUIRE(info.channels == s.channels);
    REQUIRE(info.sampleRate == s.rate);
    REQUIRE(out.size() == in.size());
    int checks = 0;
    if (! isFloat)
    {
        std::int64_t outPeak = 0;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            const auto x = static_cast<std::int64_t>(in[i]), y = static_cast<std::int64_t>(out[i]);
            const std::int64_t twice = 2 * (y * peak - x * ceiling); // exact: 2^24 · 2^23 fits in 64 bits
            REQUIRE(std::llabs(twice) <= peak); // within ½ LSB of the exact target (REN-016)
            if (std::llabs(twice) == peak)
            {
                REQUIRE(y % 2 == 0); // ties to even
                ++ties;
            }
            outPeak = std::max<std::int64_t>(outPeak, std::llabs(y));
            ++checks;
        }
        REQUIRE(outPeak == ceiling); // the peak hits the target exactly
    }
    else
    {
        const long double t = static_cast<float>(std::pow(10.0, target / 20.0));
        long double inPeak = 0;
        for (const auto v : in)
            inPeak = std::max(inPeak, std::fabs(static_cast<long double>(v)));
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            const long double exact = static_cast<long double>(in[i]) * t / inPeak;
            const auto y = static_cast<float>(out[i]);
            const long double ulp = std::nextafter(std::fabs(y), INFINITY) - std::fabs(y);
            REQUIRE(std::fabs(static_cast<long double>(y) - exact) <= 0.5L * ulp * (1 + 1e-6L));
            ++checks;
        }
    }
    return checks;
}
} // namespace

TEST_CASE("offline render: normalize ceilings at -1 dBFS are 29204 / 7476354 (REN-011)")
{
    CHECK(nz::pcmCeiling(16, -1.0) == 29204);
    CHECK(nz::pcmCeiling(24, -1.0) == 7476354);
    CHECK(nz::pcmCeiling(16, 0.0) == 32767);
    CHECK(nz::pcmCeiling(24, 0.0) == 8388607);
    CHECK(nz::pcmCeiling(16, -6.0) == 16422);
}

TEST_CASE("offline render: scalePcm rounds ties to even exactly (REN-010)")
{
    // C = 3, P = 2: x · 1.5 → 1.5 → 2, 4.5 → 4, -1.5 → -2, -4.5 → -4.
    CHECK(nz::scalePcm(1, 1.5, 3, 2) == 2);
    CHECK(nz::scalePcm(3, 1.5, 3, 2) == 4);
    CHECK(nz::scalePcm(-1, 1.5, 3, 2) == -2);
    CHECK(nz::scalePcm(-3, 1.5, 3, 2) == -4);
    CHECK(nz::scalePcm(5, 1.5, 3, 2) == 8); // 7.5 → 8
}

TEST_CASE("offline render: normalize passes the half-LSB oracle on every generated fixture (REN-010, REN-013, REN-016)")
{
    libtest::TempDir tmp("norm");
    int files = 0, checks = 0, ties = 0;
    std::uint32_t seed = 1;
    for (const auto enc : {PcmEncoding::S16, PcmEncoding::S24, PcmEncoding::F32})
        for (const double rate : {44100.0, 48000.0, 96000.0})
            for (const int channels : {1, 2})
                for (const auto container : {libtest::Container::Wav, libtest::Container::Aiff})
                    for (const double target : {-1.0, -6.0, 0.0, -3.7})
                    {
                        libtest::Spec s;
                        s.container = enc == PcmEncoding::F32 && container == libtest::Container::Aiff
                                          ? libtest::Container::Aifc
                                          : container;
                        s.encoding = enc;
                        s.channels = channels;
                        s.rate = rate;
                        // Peaks below and above the ceiling (gain > 1 and < 1); an even peak gives ties.
                        const std::int64_t peak = enc == PcmEncoding::S16 ? (seed % 2 ? 12000 : 32766)
                                                                          : (seed % 2 ? 3000000 : 8388606);
                        checks += checkFile(s, target, peak, 1500, seed++, tmp.dir(), ties);
                        ++files;
                    }
    INFO(files << " files, " << checks << " samples, " << ties << " ties");
    CHECK(files == 144);
    CHECK(ties > 0);
}

TEST_CASE("offline render: normalize streams files longer than one chunk")
{
    libtest::TempDir tmp("norm-long");
    libtest::Spec s;
    s.encoding = PcmEncoding::S24;
    s.channels = 2;
    int ties = 0;
    CHECK(checkFile(s, -1.0, 4000000, 140000, 77, tmp.dir(), ties) == 280000);
}

TEST_CASE("offline render: silence and files already at the target are left unchanged (REN-012)")
{
    libtest::TempDir tmp("norm-same");
    libtest::Spec s;
    s.encoding = PcmEncoding::S16;
    s.channels = 2;
    const auto silent = writeFixture(tmp / "silent.wav", s, std::vector<double>(2000, 0.0));
    auto r = nz::normalize(silent, tmp / "silent_norm.wav");
    REQUIRE(r);
    CHECK(r.value().silence);
    CHECK_FALSE(r.value().changed);
    CHECK_FALSE((tmp / "silent_norm.wav").exists());

    std::vector<double> at(2000, 100.0);
    at[7] = -29204;
    const auto atTarget = writeFixture(tmp / "at.wav", s, at);
    r = nz::normalize(atTarget, tmp / "at_norm.wav", -1.0);
    REQUIRE(r);
    CHECK(r.value().atTarget);
    CHECK_FALSE(r.value().changed);
    CHECK_FALSE((tmp / "at_norm.wav").exists());

    s.encoding = PcmEncoding::F32;
    std::vector<double> f(500, 0.25);
    f[3] = static_cast<float>(std::pow(10.0, -1.0 / 20.0));
    const auto atFloat = writeFixture(tmp / "atf.wav", s, f);
    r = nz::normalize(atFloat, tmp / "atf_norm.wav", -1.0);
    REQUIRE(r);
    CHECK(r.value().atTarget);
    CHECK_FALSE((tmp / "atf_norm.wav").exists());
}

TEST_CASE("offline render: unsupported encodings are refused and write nothing (REN-013)")
{
    libtest::TempDir tmp("norm-refuse");
    struct Case
    {
        PcmEncoding enc;
        int channels;
        double rate;
    };
    const Case cases[] = {{PcmEncoding::U8, 1, 44100}, {PcmEncoding::S32, 2, 44100}, {PcmEncoding::F64, 1, 48000},
                          {PcmEncoding::S16, 3, 44100}, {PcmEncoding::S16, 1, 22050}, {PcmEncoding::S24, 2, 32000},
                          {PcmEncoding::F32, 2, 88200}};
    int n = 0;
    for (const auto& c : cases)
    {
        libtest::Spec s;
        s.encoding = c.enc;
        s.channels = c.channels;
        s.rate = c.rate;
        std::vector<std::vector<float>> planar(static_cast<std::size_t>(c.channels), libtest::sine(1000, 440, c.rate, 0.3));
        const auto in = tmp.dir().getChildFile("in" + juce::String(n) + ".wav");
        REQUIRE(libtest::writeFile(in, libtest::encode(s, planar)));
        const auto out = tmp.dir().getChildFile("out" + juce::String(n) + ".wav");
        const auto r = nz::normalize(in, out);
        CHECK_FALSE(r);
        CHECK_FALSE(out.exists());
        CHECK_FALSE(out.getSiblingFile(out.getFileName() + ".part").exists());
        ++n;
    }
    // Not an audio file at all.
    (tmp / "junk.wav").replaceWithText("not audio");
    CHECK_FALSE(nz::normalize(tmp / "junk.wav", tmp / "junk_norm.wav"));
    CHECK_FALSE((tmp / "junk_norm.wav").exists());
}

TEST_CASE("offline render: the Keep file name is <name>_norm.wav (REN-014)")
{
    CHECK(nz::normalizedFileFor(juce::File("/x/kick.aif")).getFullPathName() == "/x/kick_norm.wav");
}
