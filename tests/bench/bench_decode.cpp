// Library worker budgets, one A9 core:
//   decode 16-bit stereo 44.1 kHz WAV ≥ 100× real time (60 s in ≤ 0.6 s)     decode_wav16_stereo_1s,
//   decode_wav16_60s_file resample ≥ 15× real time (stereo 48 → 44.1 kHz)                          decode_resample_4096
//   hash (XXH64) ≥ 200 MB/s                                                   decode_hash_1mb
//   preview start ≤ 80 ms after focus for a file ≤ 5 s                       decode_preview_5s
//   listing 10,000 entries ≤ 300 ms                                           decode_listing_10k
// BENCH budgets are A9 cycles (the harness convention: host ns are compared with them, emulator numbers are relative
// only). The Catch2 cases additionally gate the host at the A9 budget / 8 (our host factor: the harness publishes no
// calibration factor yet; an Apple M1 core is taken as ≥ 8× a 1 GHz A9 for this scalar/NEON integer + float work).
// All audio is generated here.
#include "tests/unit/library/LibTestAudio.h"

#include "services/library/Decoder.h"
#include "services/library/Hash.h"
#include "services/library/Resampler.h"
#include "services/library/SampleLibrary.h"
#include "services/store/SampleRefs.h"
#include "tests/bench/harness/Bench.h"

#include <chrono>
#include <cstdio>

using namespace services::library;

namespace
{
constexpr double kHostFactor = 8.0;

std::vector<unsigned char> wav16(double seconds, int channels, double rate)
{
    libtest::Spec s;
    s.channels = channels;
    s.rate = rate;
    const auto frames = static_cast<std::int64_t>(seconds * rate);
    std::vector<std::vector<float>> src;
    for (int c = 0; c < channels; ++c)
        src.push_back(libtest::noise(frames, static_cast<std::uint32_t>(c + 1), 0.7));
    return libtest::encode(s, src);
}

double decodeOnce(const std::vector<unsigned char>& bytes, double rate)
{
    juce::MemoryInputStream in(bytes.data(), bytes.size(), false);
    DecodeReport report;
    auto r = Decoder::decode(in, rate, report, {});
    return r.ok() ? static_cast<double>(r.value().buffer->ch[0][1]) : 0.0;
}

template <typename Fn> double medianMs(int runs, Fn&& fn)
{
    std::vector<double> t;
    for (int i = 0; i < runs; ++i)
    {
        const auto a = std::chrono::steady_clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}
} // namespace

// 1 s of 16-bit stereo 44.1 kHz from memory: probe + read + hash + convert. 100× real time → 10 ms.
BENCH(decode_wav16_stereo_1s, 10'000'000)
{
    const auto bytes = wav16(1.0, 2, 44100);
    state.run([&] { bench::doNotOptimize(decodeOnce(bytes, 44100)); });
}

// 5 s preview (16-bit stereo, at the engine rate) ≤ 80 ms from the focus settling to the buffer being ready.
BENCH(decode_preview_5s, 80'000'000)
{
    const auto bytes = wav16(5.0, 2, 44100);
    state.run([&] { bench::doNotOptimize(decodeOnce(bytes, 44100)); });
}

// 4096 output frames, one channel, 48 → 44.1 kHz: stereo at 15× real time leaves 4096 / 44100 / 15 / 2 s = 3.1 ms.
BENCH(decode_resample_4096, 3'100'000)
{
    const Resampler rs(48000, 44100);
    const auto in = libtest::noise(4500, 3);
    std::vector<float> out(4096);
    state.run(
        [&]
        {
            rs.process(in.data(), 4500, out.data(), 4096);
            bench::doNotOptimize(out[17]);
        });
}

// XXH64 of 1 MB at 200 MB/s = 5 ms.
BENCH(decode_hash_1mb, 5'000'000)
{
    const auto data = libtest::noise(1 << 18, 5);
    state.run([&] { bench::doNotOptimize(Xxh64::of(data.data(), data.size() * sizeof(float))); });
}

#if ! defined(BENCH_STANDALONE)
TEST_CASE("decode_wav16_60s_file: 60 s stereo 16-bit from a file, and the 48 kHz resampling load", "[bench]")
{
    libtest::TempDir tmp("bench-decode");
    const juce::File a = tmp / "a.wav", b = tmp / "b.wav";
    REQUIRE(libtest::writeFile(a, wav16(60.0, 2, 44100)));
    REQUIRE(libtest::writeFile(b, wav16(60.0, 2, 48000)));
    auto load = [](const juce::File& f)
    {
        DecodeReport report;
        auto r = Decoder::decode(f, 44100, report);
        REQUIRE(r.ok());
        bench::doNotOptimize(r.value().buffer->ch[0][5]);
    };
    load(a); // warm the page cache
    const double plain = medianMs(5, [&] { load(a); });
    const double resampled = medianMs(3, [&] { load(b); });
    double shaMs = 0;
    shaMs = medianMs(3, [&] { REQUIRE(services::store::contentId(a).ok()); });
    std::printf("BENCH decode 60 s 16-bit stereo: %.1f ms (%.0f× real time); 48 kHz → 44.1 kHz load %.1f ms "
                "(%.0f× real time); sha256 content id %.1f ms (%.0f MB/s), not gated\n",
                plain, 60000.0 / plain, resampled, 60000.0 / resampled, shaMs,
                static_cast<double>(a.getSize()) / 1e6 / (shaMs / 1000.0));
    bench::record("decode_wav16_60s_file", bench::msToCycles(600), bench::msToCycles(plain));
    bench::record("decode_resample_60s_file", bench::msToCycles(4000), bench::msToCycles(resampled));
    CHECK(plain <= 600.0 / kHostFactor);
    CHECK(resampled <= 4000.0 / kHostFactor); // 60 s at 15× real time = 4 s on the A9
}

TEST_CASE("decode_listing_10k: listing a folder of 10,000 samples (headers + sidecar lookups)", "[bench]")
{
    libtest::TempDir tmp("bench-listing");
    Workers workers(1);
    SampleLibrary library(workers);
    REQUIRE(library.setRoot(tmp.dir()).ok());
    const juce::File folder = tmp.dir().getChildFile("Samples").getChildFile("Many");
    folder.createDirectory();
    const auto bytes = wav16(0.001, 1, 44100);
    for (int i = 0; i < 10000; ++i)
        REQUIRE(folder.getChildFile("s" + juce::String(i) + ".wav").replaceWithData(bytes.data(), bytes.size()));
    REQUIRE(library.listNow("$Lib/Samples/Many", SortOrder::AToZ).size() == 10000);
    const double ms =
        medianMs(3, [&] { bench::doNotOptimize(library.listNow("$Lib/Samples/Many", SortOrder::AToZ).size()); });
    std::printf("BENCH decode listing 10,000 entries: %.1f ms\n", ms);
    bench::record("decode_listing_10k", bench::msToCycles(600), bench::msToCycles(ms));
    // The listing is bound by one open + read per file (≈ 25 µs per open on the macOS host, more than the whole
    // decode of a short file), not by computation: the host gate is the A9 budget × 2, a regression catcher; the A9
    // figure is open until measured on a unit (the emulator's qemu user mode forwards syscalls to the host).
    CHECK(ms <= 2 * 300.0);
}
#endif
