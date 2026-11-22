// Sample analysis (REN-020 peaks, REN-021 bars ⇄ BPM, REN-024 loop / one-shot). The classification is
// checked against an independent brute-force reading of the rule over a generated corpus of lengths.
#include "core/audio/SampleBufferStorage.h"
#include "services/analysis/Analysis.h"
#include "tests/unit/library/LibTestAudio.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

namespace an = services::analysis;

namespace
{
struct BufferDeleter
{
    void operator()(core::audio::SampleBuffer* b) const noexcept { core::audio::freeSampleBuffer(b); }
};
using Buffer = std::unique_ptr<core::audio::SampleBuffer, BufferDeleter>;

Buffer makeBuffer(std::int64_t frames, int channels, double rate, std::uint64_t id = 7)
{
    Buffer b(core::audio::allocateSampleBuffer(frames, channels, rate));
    b->contentId = id;
    for (int c = 0; c < channels; ++c)
    {
        auto* x = core::audio::writableChannel(b.get(), c);
        const auto n = libtest::noise(frames, static_cast<std::uint32_t>(c + 3), 0.8);
        for (std::int64_t i = 0; i < frames; ++i)
            x[i] = n[static_cast<std::size_t>(i)];
    }
    return b;
}

// The rule as written (REN-024): some N ∈ {1, 2, 4, 8, 16} and T ∈ [70, 180] with |L − 240N/T| ≤ 2 % of 240N/T,
// searched on a fine tempo grid.
bool bruteLoop(double seconds)
{
    for (const int n : {1, 2, 4, 8, 16})
        for (double t = 70.0; t <= 180.0 + 1e-9; t += 0.005)
        {
            const double l = 240.0 * n / t;
            if (std::fabs(seconds - l) <= 0.02 * l)
                return true;
        }
    return false;
}
} // namespace

TEST_CASE("offline render: peaks are min / max over all channels per 256-frame bucket (REN-020)")
{
    const auto b = makeBuffer(1000, 2, 44100);
    const auto o = an::peaks(*b);
    REQUIRE(o.bucket == 256);
    REQUIRE(o.min.size() == 4); // 3 full buckets + 1 partial
    for (std::size_t k = 0; k < o.min.size(); ++k)
    {
        float mn = 1e9f, mx = -1e9f;
        for (int c = 0; c < 2; ++c)
            for (std::int64_t i = static_cast<std::int64_t>(k) * 256; i < std::min<std::int64_t>(1000, (k + 1) * 256); ++i)
            {
                mn = std::min(mn, b->ch[c][i]);
                mx = std::max(mx, b->ch[c][i]);
            }
        CHECK(o.min[k] == mn);
        CHECK(o.max[k] == mx);
    }
    CHECK(an::peaks(*makeBuffer(0, 1, 44100)).min.empty());
}

TEST_CASE("offline render: the peak cache computes once and round-trips its file (REN-020)")
{
    libtest::TempDir tmp("peaks");
    const auto b = makeBuffer(5000, 1, 48000, 0xABCDEF);
    an::PeakCache cache(tmp.dir());
    const auto& a = cache.get(*b);
    CHECK(cache.size() == 1);
    CHECK(&cache.get(*b) == &a);
    CHECK(tmp.dir().getNumberOfChildFiles(juce::File::findFiles) == 1);
    an::PeakCache second(tmp.dir()); // a new session reads the file
    CHECK(second.get(*b) == a);
    const auto f = tmp.dir().getChildFile("bad.peaks");
    f.replaceWithText("PKS1 nonsense");
    CHECK_FALSE(an::PeakCache::read(f).has_value());
}

TEST_CASE("offline render: bars and BPM recompute each other from the length (REN-021)")
{
    CHECK(an::bpmFromBars(2, 4.0) == Catch::Approx(120.0));
    CHECK(an::barsFromBpm(120, 4.0) == Catch::Approx(2.0));
    CHECK(an::bpmFromBars(1, 0) == 0);
    CHECK(an::barsFromBpm(0, 3) == 0);
    for (const double bpm : {70.0, 93.5, 120.0, 174.0})
        CHECK(an::bpmFromBars(an::barsFromBpm(bpm, 7.3), 7.3) == Catch::Approx(bpm));
}

TEST_CASE("offline render: loop / one-shot classification matches the rule on a generated corpus (REN-024)")
{
    int loops = 0, shots = 0;
    for (double s = 0.05; s < 70.0; s *= 1.013)
    {
        const bool expect = bruteLoop(s);
        INFO("seconds " << s);
        CHECK(an::loopLength(s) == expect);
        CHECK((an::classify(static_cast<std::int64_t>(std::llround(s * 44100.0)), 44100.0, {}) == an::SampleClass::Loop) ==
              an::loopLength(std::llround(s * 44100.0) / 44100.0));
        (expect ? loops : shots)++;
    }
    CHECK(loops > 0);
    CHECK(shots > 0);
    // Edges: 1 bar at 180 BPM is 1.333 s (−2 % → 1.3067 s); 16 bars at 70 BPM 54.857 s (+2 % → 55.954 s).
    CHECK(an::loopLength(1.31));
    CHECK_FALSE(an::loopLength(1.30));
    CHECK(an::loopLength(55.9));
    CHECK_FALSE(an::loopLength(56.0));
    // A sidecar with bars > 0 is a loop whatever its length; the user's override wins (stored as sidecar extras).
    an::Sidecar m;
    m.bars = 1;
    CHECK(an::classify(1000, 44100, m) == an::SampleClass::Loop);
    an::setClassOverride(m, an::SampleClass::OneShot);
    CHECK(an::classify(1000, 44100, m) == an::SampleClass::OneShot);
    an::setClassOverride(m, std::nullopt);
    CHECK_FALSE(an::classOverride(m).has_value());
    an::Sidecar shot;
    an::setClassOverride(shot, an::SampleClass::Loop);
    CHECK(an::classify(100, 44100, shot) == an::SampleClass::Loop);
}
