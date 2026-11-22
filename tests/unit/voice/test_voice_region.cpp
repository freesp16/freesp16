// Region, loop and reverse playback (PLY-001, PLY-010, PLY-011, PLY-012): TrackVoice against an independent scalar
// reference player, frame by frame — forward, reverse, loop and loop-in-reverse, unpitched (exact) and pitched
// (Hermite, double-precision positions).
#include "VoiceHarness.h"

#include "engine/voice/InterpKernels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <random>

using namespace voicetest;
using engine::voice::Region;
using engine::voice::TrackVoice;

namespace
{
std::vector<float> noise(int n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> v(static_cast<std::size_t>(n));
    for (auto& x : v)
        x = d(rng);
    return v;
}

double hermiteAt(const TestBuffer& b, int c, double pos)
{
    const double fl = std::floor(pos);
    const auto i = static_cast<std::int64_t>(fl);
    const double x = pos - fl;
    const double ym = static_cast<double>(b.at(c, i - 1)), y0 = static_cast<double>(b.at(c, i)),
                 y1 = static_cast<double>(b.at(c, i + 1)), y2 = static_cast<double>(b.at(c, i + 2));
    const double c1 = 0.5 * (y1 - ym), c2 = ym - 2.5 * y0 + 2.0 * y1 - 0.5 * y2, c3 = 0.5 * (y2 - ym) + 1.5 * (y0 - y1);
    return ((c3 * x + c2) * x + c1) * x + y0;
}

// The reference player (PLY-010/011/012), written from the rules, not from the voice: plays the clamped region from
// its start (forward) or from end − 1 (reverse) at `rate`; loops jump by the loop length; the last X = min(64, L/2)
// source frames before the loop end crossfade linearly into the audio one loop length earlier (or later, reversed),
// or fade to silence when the sample has no X frames there.
std::vector<double> reference(const TestBuffer& b, int c, Region r, double rate, int n)
{
    r = r.clamp(b.frames());
    const double start = static_cast<double>(r.start), end = static_cast<double>(r.end());
    const double loopStart = static_cast<double>(r.loopStart), length = end - loopStart;
    const double x = std::min(64.0, std::floor(length / 2.0));
    const double preroll = r.reverse ? static_cast<double>(b.frames()) - end : loopStart;
    const bool silent = preroll < x;
    std::vector<double> out(static_cast<std::size_t>(n), 0.0);
    double pos = r.reverse ? end - 1.0 : start;
    for (int k = 0; k < n; ++k)
    {
        // Distance travelled toward the boundary, in the direction of play.
        const double toEnd = r.reverse ? pos - (loopStart - 1.0) : end - pos; // loop boundary distance
        if (! r.loop)
        {
            const bool past = r.reverse ? pos <= start - 1.0 : pos >= end;
            if (past)
                break;
            out[static_cast<std::size_t>(k)] = hermiteAt(b, c, pos);
        }
        else
        {
            const double a = hermiteAt(b, c, pos);
            if (x > 0 && toEnd <= x)
            {
                const double w = (x - toEnd) / x;
                const double other = silent ? 0.0 : hermiteAt(b, c, r.reverse ? pos + length : pos - length);
                out[static_cast<std::size_t>(k)] = silent ? a * (1.0 - w) : a + w * (other - a);
            }
            else
                out[static_cast<std::size_t>(k)] = a;
        }
        pos += r.reverse ? -rate : rate;
        if (r.loop)
        {
            if (! r.reverse && pos >= end)
                pos -= length;
            if (r.reverse && pos <= loopStart - 1.0)
                pos += length;
        }
    }
    return out;
}

void compare(const TestBuffer& b, const engine::voice::PlaybackParams& p, double rate, int blocks, double tolerance)
{
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    // The voice's own rate: cents in float, one exp2f, Q32.32 step — the reference plays at exactly that rate.
    const float cents = static_cast<float>(1200.0 * std::log2(rate));
    namespace k = engine::voice::kernels;
    const double actual = k::fromFixed(k::toFixed(static_cast<double>(std::exp2(cents * (1.0f / 1200.0f)))));
    const auto out = run(v, blocks, [&](int block) {
        if (block == 0)
            v.start(p, cents - p.pitchCents, 0, 1.0f);
    });
    for (int c = 0; c < b.get()->channels; ++c)
    {
        const auto ref = reference(b, c, p.region, actual, blocks * kBlockFrames);
        double worst = 0;
        std::size_t at = 0;
        for (std::size_t i = 0; i < ref.size(); ++i)
        {
            const double e = std::fabs(static_cast<double>((c == 0 ? out.mainL : out.mainR)[i]) - ref[i]);
            if (e > worst)
            {
                worst = e;
                at = i;
            }
        }
        INFO("channel " << c << " worst error " << worst << " at frame " << at);
        CHECK(worst <= tolerance);
    }
    for (std::size_t i = 0; i < out.size(); ++i)
        REQUIRE(out.tailL[i] == 0.0f); // nothing was retriggered
}
} // namespace

TEST_CASE("voice.region: clamp enforces PLY-001 for any input", "[voice][region]")
{
    std::mt19937 rng(5);
    std::uniform_int_distribution<std::int64_t> any(-1000, 3000);
    for (int i = 0; i < 2000; ++i)
    {
        const std::int64_t frames = 1 + i % 1500;
        const Region r = Region {any(rng), any(rng), any(rng), true, false}.clamp(frames);
        INFO(r.start << " " << r.loopStart << " " << r.length << " / " << frames);
        REQUIRE(r.start >= 0);
        REQUIRE(r.start < r.end());
        REQUIRE(r.end() <= frames);
        REQUIRE(r.loopStart >= r.start);
        REQUIRE(r.loopStart < r.end());
    }
    const Region whole = Region {}.clamp(1000);
    CHECK(whole.start == 0);
    CHECK(whole.loopStart == 0);
    CHECK(whole.length == 1000); // the default LENGTH is the whole sample
    CHECK(Region {}.clamp(0).length == 0);
}

TEST_CASE("voice.region: one-shot, reverse, loop and loop-in-reverse are frame-exact (unpitched)", "[voice][region]")
{
    const auto l = noise(3000, 1), r = noise(3000, 2);
    TestBuffer b(l, r);
    struct Case
    {
        const char* name;
        Region region;
    };
    const Case cases[] = {
        {"whole sample", {0, 0, engine::voice::kWholeSample, false, false}},
        {"sub-region", {500, 500, 700, false, false}},
        {"reverse", {500, 500, 700, false, true}},
        {"loop", {200, 900, 1100, true, false}},           // loop [900, 1300), 600 frames
        {"loop in reverse", {200, 900, 1100, true, true}}, // plays 1299 ↓ 900, then loops
        {"short loop", {100, 140, 90, true, false}},       // loop of 50 frames: 25-frame seam
        {"loop from 0 (seam fades out)", {0, 0, 1000, true, false}},
        {"reverse loop to the end (seam fades out)", {2000, 2500, 1000, true, true}},
    };
    for (const auto& c : cases)
    {
        INFO(c.name);
        compare(b, {c.region, 0.0f, false}, 1.0, 60, 1e-6);
    }
}

TEST_CASE("voice.region: pitched playback follows the reference player", "[voice][region]")
{
    const auto l = voicetest::sine(4000, 220.0), r = voicetest::sine(4000, 330.0);
    TestBuffer b(l, r);
    const double rate = GENERATE(1.0594630943592953, 0.5, 1.5, 2.0);
    const bool reverse = GENERATE(false, true);
    const bool loop = GENERATE(false, true);
    INFO("rate " << rate << " reverse " << reverse << " loop " << loop);
    compare(b, {{300, 1200, 2000, loop, reverse}, 0.0f, false}, rate, 80, 2e-5);
}

TEST_CASE("voice.region: a one-shot ends, publishes its position and goes idle", "[voice][region]")
{
    TestBuffer b(std::vector<float>(1000, 0.5f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    CHECK(v.position() == -1);
    CHECK(v.playPosition() < 0.0f);
    std::int64_t posAfterFirst = -2;
    float normAfterFirst = -2;
    const auto out = run(
        v, 20, [&](int block) {
            if (block == 0)
                v.start(params(100, 300), 0.0f, 0, 1.0f);
        },
        [&](int block) {
            if (block == 1)
            {
                posAfterFirst = v.position();
                normAfterFirst = v.playPosition();
            }
        });
    CHECK(posAfterFirst == 164);
    CHECK_THAT(static_cast<double>(normAfterFirst), Catch::Matchers::WithinAbs(0.164, 1e-6));
    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK(out.mainL[i] == (i < 300 ? 0.5f : 0.0f));
    CHECK(v.ended());
    CHECK_FALSE(v.active());
    CHECK(v.position() == -1);
    CHECK(b.refs() == 1); // only the track's own reference remains
}

TEST_CASE("voice.region: a looping note keeps playing and does not end", "[voice][region]")
{
    TestBuffer b(std::vector<float>(1000, 0.25f));
    TrackVoice v;
    v.prepare(kRate, 64);
    v.setSample(b.get());
    const auto out = run(v, 100, [&](int block) {
        if (block == 0)
            v.start(params(0, 200, true, 100), 0.0f, 0, 1.0f);
    });
    CHECK_FALSE(v.ended());
    CHECK(v.mainActive());
    const auto pos = v.position();
    CHECK(pos >= 100);
    CHECK(pos < 200);
    CHECK(out.mainL.back() != 0.0f);
}
