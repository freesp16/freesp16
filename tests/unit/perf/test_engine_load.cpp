// The engine's LoadMeter wiring (around renderBlock() in Engine::process): one sample per 64-frame
// block rendered, whatever the device period; no allocation on the audio thread (process() marks it real-time).
#include "core/util/Realtime.h"
#include "engine/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

TEST_CASE("engine load meter: one sample per rendered block, cycles within the period", "[perf][engine]")
{
    auto e = std::make_unique<engine::Engine>();
    e->prepare(44100.0, 256);
    CHECK(e->loadMeter().snapshot().blocks == 0);
    std::vector<float> l(100), r(100);
    float* out[2] = {l.data(), r.data()};
    for (int i = 0; i < 64; ++i) // 6400 frames in odd 100-frame periods: exercises the block adapter
        e->process(out, 2, 100);
    const auto s = e->loadMeter().snapshot();
    CHECK(s.blocks == e->blocksRendered());
    CHECK(s.blocks == (6400 + 63) / 64);
    CHECK(s.avgCycles > 0);
    CHECK(s.maxCycles >= s.p95Cycles);
    CHECK(s.loadPercent < 100.0);
#if APP_RT_CHECKS
    CHECK(core::rt::violationCount() == 0);
#endif
}
