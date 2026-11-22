// Render benchmarks (budget "UI redraw": an idle HOME screen ≤ 1 ms CPU per frame, meters only; "start-up": atlas
// from the disk cache ≤ 100 ms). Host numbers rank and guard; the device numbers come from the emulator's FRAME run
// lines (relative only) and from hardware (open). Run: ctest --preset host-release -L bench -R render
//   idle HOME      the root (ui::createRoot) builds its frame, the soft renderer diffs it and redraws the meters only
//   full HOME      the same frames with damage tracking off (the skeleton's cost)
//   kernels        NEON vs scalar blend kernels (bit-exact, tests/unit/render/test_blend.cpp)
//   atlas          build from the embedded fonts vs load from the cache
#include "core/params/ParamRegistry.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/unit/render/RenderFixtures.h"
#include "ui/render/GlyphAtlas.h"
#include "ui/render/OffscreenRenderer.h"
#include "ui/render/simd/Blend.h"
#include "ui/shell/Root.h"
#include "tests/bench/harness/Bench.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Result
{
    double avg = 0, p95 = 0, raster = 0, damagedShare = 0;
};

// `frames` HOME frames at 60 Hz time steps, the MASTER meter moving like a test tone with a slow tremolo.
Result homeFrames(ui::render::OffscreenRenderer& off, int frames, bool movingMeters)
{
    fakes::FakeEngineBridge bridge;
    bridge.meterFeed.masterPeak = {0.2512f, 0.2512f};
    auto root = ui::createRoot({core::builtinParams(), off.measure(), &bridge, {}});
    ui::DrawList dl;
    dl.reserve(4096, 32768);
    std::vector<double> totals;
    double raster = 0, damaged = 0;
    for (int i = 0; i < frames + 30; ++i)
    {
        const double t = 1000.0 + i * (1000.0 / 60.0);
        if (movingMeters)
        {
            const float p = 0.25f + 0.05f * std::sin(static_cast<float>(i) * 0.05f);
            bridge.meterFeed.masterPeak = {p, p * 0.98f};
        }
        const double a = nowMs();
        dl.clear();
        root->frame(dl, t);
        ui::FrameTiming timing;
        REQUIRE(off.renderer().render(dl, timing));
        const double total = nowMs() - a;
        if (i >= 30) // warm-up: first frame is a full redraw, caches settle
        {
            totals.push_back(total);
            raster += timing.rasterMs;
            damaged += static_cast<double>(std::max(0L, timing.damagedPixels));
        }
    }
    Result r;
    for (double v : totals)
        r.avg += v;
    r.avg /= static_cast<double>(totals.size());
    std::sort(totals.begin(), totals.end());
    r.p95 = totals[totals.size() * 95 / 100];
    r.raster = raster / frames;
    r.damagedShare = damaged / frames / (off.renderer().width() * static_cast<double>(off.renderer().height()));
    return r;
}
} // namespace

TEST_CASE("bench render: idle HOME with damage tracking vs full redraw (soft, 800x480)", "[bench][render]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::render::OffscreenRenderer off;
    std::string error;
    REQUIRE(off.open(800, 480, error));
    const auto still = homeFrames(off, 600, false);
    const auto meters = homeFrames(off, 600, true);
    off.setDamageTracking(false);
    const auto full = homeFrames(off, 300, true);
    std::printf("BENCH render idle HOME (static): total avg %.4f ms p95 %.4f ms, raster %.4f ms, damaged %.2f %%\n",
                still.avg, still.p95, still.raster, still.damagedShare * 100);
    std::printf(
        "BENCH render idle HOME (meters moving): total avg %.4f ms p95 %.4f ms, raster %.4f ms, damaged %.2f %%\n",
        meters.avg, meters.p95, meters.raster, meters.damagedShare * 100);
    std::printf("BENCH render full HOME redraw: total avg %.4f ms p95 %.4f ms, raster %.4f ms (%.1fx the idle frame)\n",
                full.avg, full.p95, full.raster, full.avg / meters.avg);
    bench::record("render_idle_home_static", bench::msToCycles(1.0), bench::msToCycles(still.avg),
                  bench::msToCycles(still.p95));
    bench::record("render_idle_home_meters", bench::msToCycles(1.0), bench::msToCycles(meters.avg),
                  bench::msToCycles(meters.p95));
    bench::record("render_full_home", bench::msToCycles(16.7), bench::msToCycles(full.avg), bench::msToCycles(full.p95));
    CHECK(meters.avg <= 1.0);               // redraw budget (the host is faster than the A9: this only guards)
    CHECK(meters.damagedShare < 0.05);      // meters only
    CHECK(meters.raster * 4 < full.raster); // damage pays
}

TEST_CASE("bench render: NEON vs scalar blend kernels", "[bench][render]")
{
    std::vector<std::uint32_t> px(800 * 480, 0x203040u);
    std::vector<std::uint8_t> cov(800 * 480);
    for (std::size_t i = 0; i < cov.size(); ++i)
        cov[i] = static_cast<std::uint8_t>((i * 37) % 7 == 0 ? 0 : (i * 13) & 255);
    auto time = [&](auto&& fn)
    {
        double best = 1e9;
        for (int r = 0; r < 15; ++r)
        {
            const double a = nowMs();
            fn();
            best = std::min(best, nowMs() - a);
        }
        return best;
    };
    const int n = static_cast<int>(px.size());
    const double spanScalar = time([&] { ui::simd::blendSpanScalar(px.data(), n, 0xFFEEDDu, 77); });
    const double maskScalar = time([&] { ui::simd::maskBlitScalar(px.data(), cov.data(), n, 0xFFEEDDu, 200); });
#ifdef APP_RENDER_NEON
    const double spanNeon = time([&] { ui::simd::blendSpanNeon(px.data(), n, 0xFFEEDDu, 77); });
    const double maskNeon = time([&] { ui::simd::maskBlitNeon(px.data(), cov.data(), n, 0xFFEEDDu, 200); });
    std::printf(
        "BENCH render kernels (384000 px): blendSpan scalar %.3f ms NEON %.3f ms (%.1fx); maskBlit scalar %.3f ms "
        "NEON %.3f ms (%.1fx)\n",
        spanScalar, spanNeon, spanScalar / spanNeon, maskScalar, maskNeon, maskScalar / maskNeon);
    bench::record("render_blend_span_neon", bench::msToCycles(spanScalar), bench::msToCycles(spanNeon));
    bench::record("render_mask_blit_neon", bench::msToCycles(maskScalar), bench::msToCycles(maskNeon));
    CHECK(maskNeon < maskScalar);
#else
    std::printf("BENCH render kernels (384000 px, no NEON): blendSpan %.3f ms, maskBlit %.3f ms\n", spanScalar,
                maskScalar);
#endif
}

TEST_CASE("bench render: glyph atlas build vs load from the disk cache", "[bench][render]")
{
    const auto dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("bench-render-atlas-" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    std::string error;
    ui::GlyphAtlas a, b;
    double t0 = nowMs();
    REQUIRE(a.loadOrBuild(0.625f, dir.getFullPathName().toStdString(), error));
    const double built = nowMs() - t0;
    t0 = nowMs();
    REQUIRE(b.loadOrBuild(0.625f, dir.getFullPathName().toStdString(), error));
    const double loaded = nowMs() - t0;
    std::printf("BENCH render atlas %s: build %.1f ms, from cache %.2f ms\n", b.describe().c_str(), built, loaded);
    CHECK(b.source() == ui::GlyphAtlas::Source::Cache);
    bench::record("render_atlas_from_cache", bench::msToCycles(100.0), bench::msToCycles(loaded));
    CHECK(loaded <= 100.0);
    dir.deleteRecursively();
}
