// Fuzz smoke test: 500 mutated WAV/AIFF/AIFC/.wav.dat files never crash the
// decoder, the header probe or the sidecar parser (run under ASan/UBSan in the host preset). Inputs are generated:
// valid files from the test encoder, then bytes flipped, sizes rewritten, truncated or extended (deterministic seeds).
#include "LibTestAudio.h"

#include "services/library/Decoder.h"
#include "services/library/SidecarStore.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_data_structures/juce_data_structures.h>

using namespace services::library;

namespace
{
struct Rng
{
    std::uint32_t x;
    std::uint32_t next()
    {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        return x;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : next() % n; }
};

std::vector<unsigned char> mutate(std::vector<unsigned char> b, Rng& r)
{
    const int kind = static_cast<int>(r.below(5));
    const int edits = 1 + static_cast<int>(r.below(8));
    for (int i = 0; i < edits && ! b.empty(); ++i)
    {
        const std::size_t at = r.below(std::min<std::size_t>(b.size(), kind == 0 ? b.size() : 96)); // headers mostly
        switch (kind)
        {
        case 0:
        case 1:
            b[at] = static_cast<unsigned char>(r.next());
            break;
        case 2: // a 32-bit field set to an extreme
        {
            const std::uint32_t v = r.below(2) ? 0xFFFFFFFFu : static_cast<std::uint32_t>(r.below(3));
            for (int k = 0; k < 4 && at + static_cast<std::size_t>(k) < b.size(); ++k)
                b[at + static_cast<std::size_t>(k)] = static_cast<unsigned char>(v >> (8 * k));
            break;
        }
        case 3:
            b.resize(r.below(b.size()));
            break;
        case 4:
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), static_cast<unsigned char>(r.next()));
            break;
        }
    }
    return b;
}
} // namespace

TEST_CASE("lib.fuzz: 500 mutated audio and sidecar files never crash", "[library][fuzz]")
{
    using libtest::Container;
    std::vector<std::vector<unsigned char>> seeds;
    for (auto c : {Container::Wav, Container::WavExtensible, Container::Aiff, Container::Aifc})
        for (auto e : {PcmEncoding::S16, PcmEncoding::S24, PcmEncoding::F32})
        {
            libtest::Spec s;
            s.container = c, s.encoding = e, s.channels = 1 + static_cast<int>(seeds.size() % 3);
            s.rate = seeds.size() % 2 ? 48000 : 44100;
            s.loopStart = 5, s.loopEnd = 50;
            std::vector<std::vector<float>> src(static_cast<std::size_t>(s.channels), libtest::noise(64, 9));
            seeds.push_back(libtest::encode(s, src));
        }
    std::vector<juce::MemoryBlock> dats;
    for (int i = 0; i < 3; ++i)
    {
        juce::ValueTree t{"SampleAdjunctData"};
        t.setProperty("bar", 0.25 * i, nullptr);
        t.setProperty("bpm", 120.0, nullptr);
        t.setProperty("sampleNum", i, nullptr);
        t.setProperty("list", juce::Array<juce::var>{1, 2.5, "x"}, nullptr);
        juce::MemoryOutputStream out;
        t.writeToStream(out);
        dats.push_back(out.getMemoryBlock());
    }

    Rng rng{12345};
    int decoded = 0, refused = 0, sidecars = 0;
    for (int i = 0; i < 400; ++i)
    {
        const auto bytes = mutate(seeds[static_cast<std::size_t>(i) % seeds.size()], rng);
        juce::MemoryInputStream in(bytes.data(), bytes.size(), false);
        (void)probeAudio(in);
        DecodeReport report;
        DecodeOptions options;
        options.limitBytes = 64 << 20; // a mutated size never makes the test allocate gigabytes
        auto r = Decoder::decode(in, i % 2 ? 44100.0 : 48000.0, report, options);
        if (r.ok())
        {
            ++decoded;
            const auto& b = *r.value().buffer;
            for (int c = 0; c < b.channels; ++c)
                for (std::int64_t k = 0; k < b.frames; ++k)
                    REQUIRE(std::isfinite(b.ch[c][k]));
        }
        else
        {
            ++refused;
            CHECK_FALSE(r.error().message.empty());
        }
    }
    for (int i = 0; i < 100; ++i)
    {
        const auto& d = dats[static_cast<std::size_t>(i) % dats.size()];
        std::vector<unsigned char> bytes(static_cast<const unsigned char*>(d.getData()),
                                         static_cast<const unsigned char*>(d.getData()) + d.getSize());
        bytes = mutate(bytes, rng);
        if (SidecarStore::parseReference(bytes.data(), bytes.size()))
            ++sidecars;
        (void)SidecarStore::parseJson(
            juce::String::fromUTF8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())));
    }
    INFO("decoded " << decoded << ", refused " << refused << ", sidecars parsed " << sidecars);
    CHECK(decoded + refused == 400);
    CHECK(decoded > 0);
    CHECK(refused > 0);
}
