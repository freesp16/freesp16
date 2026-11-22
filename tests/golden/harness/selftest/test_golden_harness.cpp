// Self-tests of the golden harness: the comparator and its report, the gain-table cross-check, GainTest's EffectBase
// contract, and that every committed case loads, names a known subject and passes.
#include "tests/golden/harness/GainTest.h"
#include "tests/golden/harness/GoldenCase.h"
#include "tests/golden/harness/Subject.h"

#include "core/util/GainLaw.h"
#include "core/util/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cfloat>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using golden::Channels;
using golden::GoldenCase;

namespace
{
GoldenCase synthetic(int blocks = 4)
{
    GoldenCase c;
    c.name = "selftest";
    c.id = "synthetic";
    c.subject = "none";
    c.blocks = blocks;
    c.frames = blocks * 64;
    c.windowEnd = c.frames;
    c.outputNames = {"L", "R"};
    c.expected.assign(2, std::vector<float>(static_cast<std::size_t>(c.frames)));
    std::uint32_t x = 1;
    for (auto& ch : c.expected)
        for (auto& v : ch)
        {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            v = static_cast<float>(x / 4294967296.0 - 0.5);
        }
    return c;
}

std::vector<fs::path> committedCases()
{
    std::vector<fs::path> out;
    for (const auto& d : fs::directory_iterator(fs::path(APP_SOURCE_DIR) / "tests" / "golden"))
    {
        if (! d.is_directory() || ! fs::exists(d.path() / "cases.json"))
            continue;
        for (const auto& f : fs::directory_iterator(d.path()))
            if (f.path().extension() == ".json" && f.path().filename() != "cases.json")
                out.push_back(f.path());
    }
    return out;
}
} // namespace

TEST_CASE("harness.compare: identical output passes in both modes", "[golden]")
{
    const auto c = synthetic();
    for (bool parity : {false, true})
    {
        const auto r = golden::compare(c, c.expected, parity);
        CHECK(r.pass);
        CHECK(r.report.rfind("GOLDEN PASS selftest/synthetic", 0) == 0);
    }
}

TEST_CASE("harness.compare: a gain error of 2^-10 fails with a readable report", "[golden]")
{
    const auto c = synthetic();
    Channels a = c.expected;
    for (auto& ch : a)
        for (auto& v : ch)
            v *= golden::kFaultGainScale;
    const auto r = golden::compare(c, a, false, "fault: gain");
    CHECK_FALSE(r.pass);
    INFO(r.report);
    CHECK(r.report.rfind("GOLDEN FAIL selftest/synthetic [fault: gain]", 0) == 0);
    CHECK(r.report.find("max |err|") != std::string::npos);
    CHECK(r.report.find("dBFS") != std::string::npos);
    CHECK(r.report.find("(block ") != std::string::npos);
    CHECK(r.report.find("expected ") != std::string::npos);
}

TEST_CASE("harness.compare: errors below the tolerance pass, parity catches one ulp", "[golden]")
{
    const auto c = synthetic();
    Channels a = c.expected;
    a[0][100] = std::nextafter(a[0][100], 2.0f); // 1 ulp: ~6e-8 relative, far below 1e-5 and -100 dBFS RMS
    CHECK(golden::compare(c, a, false).pass);
    const auto r = golden::compare(c, a, true);
    CHECK_FALSE(r.pass);
    CHECK(r.report.find("parity: 1 of 256 samples not bit-exact, first at frame 100 (block 1, offset 36)") !=
          std::string::npos);
}

TEST_CASE("harness.compare: denormal-range tails count as equal (FTZ); NaN always fails", "[golden]")
{
    auto c = synthetic();
    c.expected[1][200] = 1e-39f; // the reference keeps denormals, the app flushes them
    Channels a = c.expected;
    a[1][200] = 0.0f;
    CHECK(golden::compare(c, a, false).pass);
    CHECK(golden::compare(c, a, true).pass);
    a[1][10] = NAN;
    const auto r = golden::compare(c, a, false);
    CHECK_FALSE(r.pass);
    CHECK(r.report.find("non-finite") != std::string::npos);
}

TEST_CASE("harness.compare: the window limits the comparison", "[golden]")
{
    auto c = synthetic();
    Channels a = c.expected;
    a[0][250] += 0.5f;
    CHECK_FALSE(golden::compare(c, a, false).pass);
    c.windowEnd = 192;
    CHECK(golden::compare(c, a, false).pass);
}

TEST_CASE("harness.gainLaw: a golden made with another gain table is reported", "[golden]")
{
    GoldenCase c = synthetic();
    for (int i = 0; i <= core::GainLaw::maxIndex; ++i)
        c.gainLawBits.push_back(std::bit_cast<std::uint32_t>(core::GainLaw::gain(i)));
    CHECK(golden::checkGainLaw(c).empty());
    c.gainLawBits[5] = std::bit_cast<std::uint32_t>(0.005f);
    const auto r = golden::checkGainLaw(c);
    CHECK(r.find("index   5") != std::string::npos);
    CHECK(r.find("regenerate") != std::string::npos);
}

TEST_CASE("harness.GainTest: EffectBase contract (no allocation in process, clear state, no NaN, latency 0)",
          "[golden]")
{
    golden::GainTest fresh, fx;
    fresh.prepare(44100.0, 64);
    fx.prepare(44100.0, 64);
    engine::fx::ProcessContext ctx{44100.0, 120.0, false, 0u, {}, 0};
    float l[64], r[64], l2[64], r2[64];
    std::uint32_t x = 7;
    auto noise = [&](float* d)
    {
        for (int i = 0; i < 64; ++i)
        {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            d[i] = (x & 1u) ? 1.0f : -1.0f;
        }
    };
    {
        core::rt::ScopedRealtimeThread rt;
        for (float v : {0.0f, 1.0f, 0.37f, 0.787f})
        {
            fx.setParam(0, v);
            noise(l);
            noise(r);
            fx.process(l, r, ctx);
            for (int i = 0; i < 64; ++i)
                REQUIRE((std::isfinite(l[i]) && std::isfinite(r[i])));
        }
        fx.setOption(engine::fx::kClearState, 1);
        fx.reset();
    }
    noise(l);
    noise(r);
    std::copy(l, l + 64, l2);
    std::copy(r, r + 64, r2);
    fx.process(l, r, ctx);
    fresh.process(l2, r2, ctx);
    for (int i = 0; i < 64; ++i)
        REQUIRE((std::bit_cast<std::uint32_t>(l[i]) == std::bit_cast<std::uint32_t>(l2[i]) &&
                 std::bit_cast<std::uint32_t>(r[i]) == std::bit_cast<std::uint32_t>(r2[i])));
    CHECK(fx.latencySamples() == 0);
    // Default index 100 is unity: bit-transparent.
    noise(l);
    std::copy(l, l + 64, l2);
    fresh.process(l, r, ctx);
    CHECK(std::equal(l, l + 64, l2));
}

TEST_CASE("harness.cases: every committed golden loads, names a subject and passes (tolerance and parity)", "[golden]")
{
    const auto cases = committedCases();
    REQUIRE(cases.size() >= 2);
    for (const auto& json : cases)
    {
        GoldenCase c;
        const auto e = golden::loadCase(json, c);
        INFO(json.string() << ": " << e);
        REQUIRE(e.empty());
        REQUIRE(golden::checkGainLaw(c).empty());
        auto s = golden::makeSubject(c.subject);
        REQUIRE(s != nullptr);
        Channels actual;
        REQUIRE(golden::runSubject(c, *s, actual).empty());
        const auto r = golden::compare(c, actual, c.parityExpected);
        INFO(r.report);
        CHECK(r.pass);
    }
}
