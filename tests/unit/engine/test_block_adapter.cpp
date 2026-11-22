// The device-buffer adapter (audio-engine.md "Internal block": fixed 64-frame engine blocks whatever the device buffer;
// threading.md "Timing": device sizes 32–512): output is sample-identical for every device period — multiples of 64
// or not, constant or changing from period to period — and the engine renders exactly ceil(frames / 64) blocks.
#include "tests/unit/engine/EngineTestUtil.h"

#include "core/util/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <random>

using enginetest::render;

TEST_CASE("block adapter: output is sample-identical for device sizes 32…512", "[engine][adapter]")
{
    constexpr int frames = 44100;
    auto reference = enginetest::make();
    const auto ref = render(*reference, frames, 64);
    for (const int period : {32, 48, 64, 65, 96, 100, 128, 191, 192, 256, 333, 500, 512, 1, 7, 1024})
    {
        INFO("device period " << period);
        auto e = enginetest::make();
        const auto out = render(*e, frames, period);
        CHECK(std::memcmp(out.l.data(), ref.l.data(), sizeof(float) * frames) == 0);
        CHECK(std::memcmp(out.r.data(), ref.r.data(), sizeof(float) * frames) == 0);
        CHECK(e->blocksRendered() == (frames + 63) / 64);
    }
}

TEST_CASE("block adapter: periods that change size every callback", "[engine][adapter]")
{
    constexpr int frames = 30000;
    auto reference = enginetest::make();
    const auto ref = render(*reference, frames, 64);
    auto e = enginetest::make();
    std::vector<float> l(frames), r(frames);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> size(32, 512);
    {
        core::rt::ScopedRealtimeThread realtime;
        for (int done = 0; done < frames;)
        {
            const int n = std::min(size(rng), frames - done);
            float* out[2] = {l.data() + done, r.data() + done};
            e->process(out, 2, n);
            done += n;
        }
    }
    CHECK(std::memcmp(l.data(), ref.l.data(), sizeof(float) * frames) == 0);
    CHECK(std::memcmp(r.data(), ref.r.data(), sizeof(float) * frames) == 0);
}

TEST_CASE("block adapter: a zero-frame callback renders nothing and still publishes", "[engine][adapter]")
{
    auto e = enginetest::make();
    float* out[2] = {nullptr, nullptr};
    e->process(out, 2, 0);
    CHECK(e->blocksRendered() == 0);
    render(*e, 64, 64);
    CHECK(e->blocksRendered() == 1);
    CHECK(e->bridge().playhead().blockIndex == 1);
}

TEST_CASE("block adapter: input arrives with a constant 64-frame latency", "[engine][adapter]")
{
    // The input meter of a block sees the input frames of the block before it: an impulse at stream frame k shows up in
    // the meters of the period that renders block floor(k / 64) + 1.
    auto e = enginetest::make(false);
    constexpr int period = 100;
    std::vector<float> in(period, 0.0f), l(period), r(period);
    const float* inputs[2] = {in.data(), in.data()};
    std::array<float*, 12> out {};
    out[0] = l.data();
    out[1] = r.data();
    auto run = [&](bool impulseAt30) {
        std::fill(in.begin(), in.end(), 0.0f);
        if (impulseAt30)
            in[30] = 0.5f;
        core::rt::ScopedRealtimeThread realtime;
        e->process({out.data(), 1, inputs, 2, period, 0});
        return e->bridge().meters().inputPeak[0];
    };
    // period 1 = stream frames 0–99, renders blocks 0 and 1 (frames 0–127). The impulse at frame 30 is in block 0's
    // input window, which block 1 renders → visible in period 1 already.
    CHECK(run(true) == 0.5f);
    // period 2 = frames 100–199, renders blocks 2 and 3 (input = frames 64–191): no impulse.
    CHECK(run(false) == 0.0f);
    // An impulse at stream frame 230 (period 3 = frames 200–299 renders block 4, input = frames 192–255): seen.
    CHECK(run(true) == 0.5f);
    CHECK(engine::Engine::inputLatencyFrames() == 64);

    // Aligned periods of 64: an impulse in period k is rendered (and metered) in period k + 1 — exactly one block late.
    auto a = enginetest::make(false);
    std::vector<float> in64(64, 0.0f);
    const float* in2[2] = {in64.data(), in64.data()};
    auto step = [&](float last) {
        in64[63] = last;
        core::rt::ScopedRealtimeThread realtime;
        a->process({out.data(), 1, in2, 2, 64, 0});
        return a->bridge().meters().inputPeak[0];
    };
    CHECK(step(0.25f) == 0.0f);
    CHECK(step(0.0f) == 0.25f);
    CHECK(step(0.0f) == 0.0f);
}

TEST_CASE("block adapter: absent ports are skipped, present silent ports are zeroed", "[engine][adapter]")
{
    auto e = enginetest::make();
    std::vector<float> main(512, 1.0f), out34(512, 1.0f);
    std::array<float*, 12> out {};
    out[0] = main.data();
    out[1] = main.data() + 256;
    out[2] = out34.data();
    out[3] = out34.data() + 256;
    {
        core::rt::ScopedRealtimeThread realtime;
        e->process({out.data(), 6, nullptr, 0, 256, 0});
    }
    CHECK(enginetest::peak(main) > 0.2f);
    CHECK(enginetest::peak(out34) == 0.0f);
}
