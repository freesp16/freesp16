// Decoder, formats, resampler and hash (LIB-020/021/022): every
// format/bit depth decodes within 1 LSB of the source, > 2 channels mix down with a note, a 48 kHz file resamples with
// ≤ −90 dB aliasing in 0–18 kHz, buffers are padded and 16-byte aligned, damaged files are refused with a message.
#include "LibTestAudio.h"

#include "services/library/Decoder.h"
#include "services/library/Hash.h"
#include "services/library/Resampler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <cmath>
#include <complex>

using namespace services::library;
using libtest::Container;
using libtest::Spec;

namespace
{
core::Result<DecodedAudio> decodeBytes(const std::vector<unsigned char>& bytes, double rate, DecodeReport& report,
                                       const DecodeOptions& options = {})
{
    juce::MemoryInputStream in(bytes.data(), bytes.size(), false);
    return Decoder::decode(in, rate, report, options);
}

const char* name(Container c)
{
    switch (c)
    {
    case Container::Wav:
        return "WAV";
    case Container::WavExtensible:
        return "WAV-EXT";
    case Container::Aiff:
        return "AIFF";
    case Container::Aifc:
        return "AIFC";
    }
    return "?";
}

// Largest |out − quantised(src)| in LSBs of the encoding.
double maxErrorLsb(const core::audio::SampleBuffer& b, const std::vector<std::vector<float>>& src, PcmEncoding e)
{
    const double lsb = e == PcmEncoding::F32 || e == PcmEncoding::F64 ? 1.0 / 8388608.0 : 1.0 / libtest::sampleMax(e);
    double worst = 0;
    for (std::size_t c = 0; c < src.size(); ++c)
        for (std::size_t i = 0; i < src[c].size(); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(b.ch[c][i]) -
                                              libtest::quantised(static_cast<double>(src[c][i]), e)) /
                                        lsb);
    return worst;
}

// Residual of `x` after removing the best-fitting sine at `hz` (least squares on sin/cos + DC), as dB re the sine.
double residualDb(const float* x, std::int64_t from, std::int64_t to, double hz, double rate)
{
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
    for (std::int64_t i = from; i < to; ++i)
    {
        const double w = 2 * 3.14159265358979323846 * hz * static_cast<double>(i) / rate;
        const double s = std::sin(w), c = std::cos(w);
        const auto v = static_cast<double>(x[i]);
        ss += s * s, sc += s * c, cc += c * c, xs += v * s, xc += v * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double sig = 0, res = 0;
    for (std::int64_t i = from; i < to; ++i)
    {
        const double w = 2 * 3.14159265358979323846 * hz * static_cast<double>(i) / rate;
        const double fit = a * std::sin(w) + b * std::cos(w);
        sig += fit * fit;
        res += (static_cast<double>(x[i]) - fit) * (static_cast<double>(x[i]) - fit);
    }
    return 10 * std::log10(res / sig);
}
} // namespace

TEST_CASE("lib.hash: XXH64 matches the reference vectors and streams in any split", "[library][hash]")
{
    CHECK(Xxh64::of("", 0) == 0xEF46DB3751D8E999ULL);
    CHECK(Xxh64::of("abc", 3) == 0x44BC2CF5AD770999ULL);
    const char* spam = "Nobody inspects the spammish repetition";
    CHECK(Xxh64::of(spam, std::strlen(spam)) == 0xFBCEA83C8A378BF1ULL);

    const auto data = libtest::noise(1000, 7);
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    for (std::size_t len : {0u, 1u, 7u, 31u, 32u, 33u, 63u, 64u, 100u, 1000u, 4000u})
    {
        const std::uint64_t whole = Xxh64::of(bytes, len, 99);
        for (std::size_t split : {1u, 3u, 17u, 32u, 61u})
        {
            Xxh64 h(99);
            for (std::size_t at = 0; at < len; at += split)
                h.update(bytes + at, std::min(split, len - at));
            CHECK(h.digest() == whole);
        }
    }
}

TEST_CASE("lib.decoder: every format and bit depth decodes within 1 LSB of the source", "[library][decoder]")
{
    struct Case
    {
        Container c;
        PcmEncoding e;
        bool sowt;
    };
    const Case cases[] = {
        {Container::Wav, PcmEncoding::U8, false},
        {Container::Wav, PcmEncoding::S16, false},
        {Container::Wav, PcmEncoding::S24, false},
        {Container::Wav, PcmEncoding::S32, false},
        {Container::Wav, PcmEncoding::F32, false},
        {Container::Wav, PcmEncoding::F64, false},
        {Container::WavExtensible, PcmEncoding::S24, false},
        {Container::WavExtensible, PcmEncoding::F32, false},
        {Container::Aiff, PcmEncoding::S8, false},
        {Container::Aiff, PcmEncoding::S16, false},
        {Container::Aiff, PcmEncoding::S24, false},
        {Container::Aiff, PcmEncoding::S32, false},
        {Container::Aifc, PcmEncoding::S16, false},
        {Container::Aifc, PcmEncoding::S16, true},
        {Container::Aifc, PcmEncoding::F32, false},
        {Container::Aifc, PcmEncoding::F64, false},
    };
    for (const auto& k : cases)
        for (int channels : {1, 2})
        {
            INFO(name(k.c) << " encoding " << static_cast<int>(k.e) << " sowt " << k.sowt << " ch " << channels);
            Spec s;
            s.container = k.c, s.encoding = k.e, s.littleEndianAifc = k.sowt, s.channels = channels, s.rate = 44100;
            std::vector<std::vector<float>> src{libtest::noise(1001, 1)};
            if (channels == 2)
                src.push_back(libtest::sine(1001, 440, 44100, 0.99));
            src[0][0] = 1.0f, src[0][1] = -1.0f; // full scale both ways (clipped to max − 1 LSB by the encoder)
            DecodeReport report;
            auto r = decodeBytes(libtest::encode(s, src), 44100, report);
            REQUIRE(r.ok());
            const auto& b = *r.value().buffer;
            CHECK(b.frames == 1001);
            CHECK(b.channels == channels);
            CHECK(b.sampleRate == 44100);
            CHECK_FALSE(report.resampled);
            CHECK(maxErrorLsb(b, src, k.e) <= 1.0);
        }
}

TEST_CASE("lib.decoder: buffers are padded with zeros and 16-byte aligned; mono reads ch[1] = ch[0]",
          "[library][decoder]")
{
    for (int channels : {1, 2})
        for (std::int64_t frames : {0, 1, 5, 64, 1027})
        {
            Spec s;
            s.channels = channels;
            std::vector<std::vector<float>> src(static_cast<std::size_t>(channels),
                                                std::vector<float>(static_cast<std::size_t>(frames), 0.5f));
            DecodeReport report;
            auto r = decodeBytes(libtest::encode(s, src), 44100, report);
            REQUIRE(r.ok());
            const auto& b = *r.value().buffer;
            REQUIRE(b.frames == frames);
            for (int c = 0; c < 2; ++c)
            {
                CHECK(reinterpret_cast<std::uintptr_t>(b.ch[c]) % 16 == 0);
                for (int i = 1; i <= core::audio::SampleBuffer::pad; ++i)
                {
                    CHECK(b.ch[c][-i] == 0.0f);
                    CHECK(b.ch[c][frames + i - 1] == 0.0f);
                }
            }
            if (channels == 1)
                CHECK(b.ch[1] == b.ch[0]);
            CHECK(b.refs.load() == 0);
        }
}

TEST_CASE("lib.decoder: more than two channels mix down to stereo with a note", "[library][decoder]")
{
    Spec s;
    s.channels = 6;
    s.encoding = PcmEncoding::F32;
    std::vector<std::vector<float>> src;
    for (int c = 0; c < 6; ++c)
        src.push_back(std::vector<float>(100, 0.1f * static_cast<float>(c + 1)));
    DecodeReport report;
    auto r = decodeBytes(libtest::encode(s, src), 44100, report);
    REQUIRE(r.ok());
    const auto& b = *r.value().buffer;
    CHECK(b.channels == 2);
    CHECK(report.mixedDown);
    REQUIRE_FALSE(report.notes.empty());
    CHECK(report.notes[0].find("mixed down") != std::string::npos);
    CHECK(std::fabs(b.ch[0][50] - (0.1f + 0.3f + 0.5f) / 3) < 1e-6f); // channels 1, 3, 5 (even indices)
    CHECK(std::fabs(b.ch[1][50] - (0.2f + 0.4f + 0.6f) / 3) < 1e-6f);
}

TEST_CASE("lib.resampler: 48 kHz → 44.1 kHz with <= -90 dB aliasing in 0-18 kHz", "[library][resampler]")
{
    const double tones[] = {997, 5003, 11025, 15013, 17989};
    for (double hz : tones)
    {
        INFO("tone " << hz << " Hz");
        Spec s;
        s.encoding = PcmEncoding::F32;
        s.rate = 48000;
        std::vector<std::vector<float>> src{libtest::sine(48000, hz, 48000, 0.9)};
        DecodeReport report;
        auto r = decodeBytes(libtest::encode(s, src), 44100, report);
        REQUIRE(r.ok());
        const auto& b = *r.value().buffer;
        CHECK(report.resampled);
        CHECK(b.frames == 44100);
        CHECK(b.sampleRate == 44100);
        const double db = residualDb(b.ch[0], 2000, b.frames - 2000, hz, 44100);
        INFO("residual " << db << " dB");
        CHECK(db <= -90.0);
    }
}

TEST_CASE("lib.resampler: NEON matches the scalar reference; any rate pair keeps length and level",
          "[library][resampler][neon]")
{
    const auto in = libtest::noise(9000, 3, 0.5);
    const std::pair<double, double> pairs[] = {{48000, 44100}, {44100, 48000},   {22050, 44100}, {96000, 44100},
                                               {32000, 48000}, {44100.5, 44100}, {11025, 48000}};
    for (auto [from, to] : pairs)
    {
        INFO(from << " -> " << to);
        const Resampler rs(from, to);
        const std::int64_t n = rs.outputFrames(9000);
        CHECK(std::llabs(n - static_cast<std::int64_t>(std::ceil(9000 * to / from))) <= 1);
        std::vector<float> a(static_cast<std::size_t>(n)), b(static_cast<std::size_t>(n));
        rs.process(in.data(), 9000, a.data(), n, Resampler::Kernel::Scalar);
        rs.process(in.data(), 9000, b.data(), n, Resampler::Kernel::Neon);
        float worst = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::fabs(a[i] - b[i]));
        CHECK(worst <= 1e-6f);
        // DC passes at unity gain (every phase normalised).
        std::vector<float> dc(4000, 0.25f), out(static_cast<std::size_t>(rs.outputFrames(4000)));
        rs.process(dc.data(), 4000, out.data(), static_cast<std::int64_t>(out.size()));
        CHECK(std::fabs(out[out.size() / 2] - 0.25f) < 1e-6f);
    }
}

TEST_CASE("lib.decoder: contentId follows the PCM data, not the file around it", "[library][decoder][hash]")
{
    Spec s;
    s.channels = 2;
    std::vector<std::vector<float>> src{libtest::noise(500, 1), libtest::noise(500, 2)};
    auto bytes = libtest::encode(s, src);
    DecodeReport r1, r2, r3, r4;
    auto a = decodeBytes(bytes, 44100, r1);
    Spec looped = s;
    looped.loopStart = 10, looped.loopEnd = 400; // an extra smpl chunk: same PCM
    auto b = decodeBytes(libtest::encode(looped, src), 44100, r2);
    src[1][250] += 0.01f;
    auto c = decodeBytes(libtest::encode(s, src), 44100, r3);
    auto d = decodeBytes(bytes, 48000, r4); // the same file for another engine rate is another buffer
    REQUIRE((a.ok() && b.ok() && c.ok() && d.ok()));
    CHECK(a.value().buffer->contentId == b.value().buffer->contentId);
    CHECK(a.value().buffer->contentId != c.value().buffer->contentId);
    CHECK(a.value().buffer->contentId != d.value().buffer->contentId);
    CHECK(b.value().loopStart == 10);
    CHECK(b.value().loopEnd == 400);
}

TEST_CASE("lib.decoder: AIFF sustain loops, truncated data, partial and cancelled decodes", "[library][decoder]")
{
    Spec s;
    s.container = Container::Aiff;
    s.loopStart = 100, s.loopEnd = 900;
    std::vector<std::vector<float>> src{libtest::sine(1000, 100, 44100)};
    auto bytes = libtest::encode(s, src);
    {
        DecodeReport report;
        auto r = decodeBytes(bytes, 44100, report);
        REQUIRE(r.ok());
        CHECK(r.value().loopStart == 100);
        CHECK(r.value().loopEnd == 900);
    }
    {
        auto cut = bytes;
        cut.resize(cut.size() - 500); // 250 frames missing
        DecodeReport report;
        auto r = decodeBytes(cut, 44100, report);
        REQUIRE(r.ok());
        CHECK(report.truncated);
        CHECK(r.value().buffer->frames == 750);
        CHECK_FALSE(report.notes.empty());
    }
    {
        DecodeOptions o;
        o.maxSeconds = 0.01; // 441 frames
        DecodeReport report;
        auto r = decodeBytes(bytes, 44100, report, o);
        REQUIRE(r.ok());
        CHECK(report.partial);
        CHECK(r.value().buffer->frames == 441);
    }
    {
        std::atomic<bool> cancel{true};
        DecodeOptions o;
        o.cancel = &cancel;
        DecodeReport report;
        auto r = decodeBytes(bytes, 44100, report, o);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message == "Cancelled");
    }
}

TEST_CASE("lib.decoder: damaged and unsupported files are refused with a message", "[library][decoder]")
{
    Spec s;
    std::vector<std::vector<float>> src{libtest::sine(100, 100, 44100)};
    const auto good = libtest::encode(s, src);
    auto adpcm = good;
    adpcm[20] = 2; // format tag 2 = MS ADPCM
    auto noFmt = good;
    std::memcpy(noFmt.data() + 12, "junk", 4);
    std::vector<unsigned char> text = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', '!'};
    auto zeroChannels = good;
    zeroChannels[22] = 0;
    auto badRate = good;
    badRate[24] = badRate[25] = badRate[26] = badRate[27] = 0;
    for (const std::vector<unsigned char>* bytes : {&adpcm, &noFmt, &text, &zeroChannels, &badRate})
    {
        DecodeReport report;
        auto r = decodeBytes(*bytes, 44100, report);
        REQUIRE_FALSE(r.ok());
        CHECK_FALSE(r.error().message.empty());
    }
    DecodeReport report;
    CHECK(decodeBytes(adpcm, 44100, report).error().message.find("Unsupported") != std::string::npos);
    CHECK(Decoder::decode(juce::File("/nonexistent/x.wav"), 44100, report).error().message == "File not found");
}

TEST_CASE("lib.decoder: over the memory limit is refused; above 10 minutes loads with a warning", "[library][decoder]")
{
    Spec s;
    s.encoding = PcmEncoding::U8;
    s.rate = 8000;
    std::vector<std::vector<float>> src{std::vector<float>(8000 * 601, 0.25f)}; // 10 min 1 s
    const auto bytes = libtest::encode(s, src);
    {
        DecodeOptions o;
        o.limitBytes = 1 << 20;
        DecodeReport report;
        auto r = decodeBytes(bytes, 8000, report, o);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().message.find("memory") != std::string::npos);
    }
    DecodeReport report;
    auto r = decodeBytes(bytes, 8000, report);
    REQUIRE(r.ok());
    CHECK(report.longSample);
    CHECK(r.value().buffer->frames == 8000 * 601);
    CHECK(std::any_of(report.notes.begin(), report.notes.end(),
                      [](const std::string& n) { return n.find("Long sample") != std::string::npos; }));
}

TEST_CASE("lib.decoder: the header probe reads length, channels and rate without the data", "[library][decoder]")
{
    Spec s;
    s.channels = 2;
    s.rate = 48000;
    s.encoding = PcmEncoding::S24;
    std::vector<std::vector<float>> src{libtest::noise(4800, 1), libtest::noise(4800, 2)};
    libtest::TempDir tmp;
    const auto file = tmp / "a.wav";
    REQUIRE(libtest::writeFile(file, libtest::encode(s, src)));
    auto info = probeAudioFile(file);
    REQUIRE(info.ok());
    CHECK(info.value().frames == 4800);
    CHECK(info.value().channels == 2);
    CHECK(info.value().sampleRate == 48000);
    CHECK(info.value().bitsPerSample == 24);
    CHECK(info.value().seconds() == 0.1);
    CHECK(isAudioFileName("Kick.WAV"));
    CHECK(isAudioFileName("x.aif"));
    CHECK(isAudioFileName("x.aiff"));
    CHECK_FALSE(isAudioFileName(".hidden.wav"));
    CHECK_FALSE(isAudioFileName("notes.txt"));
    CHECK_FALSE(isAudioFileName("kick.wav.json"));
}
