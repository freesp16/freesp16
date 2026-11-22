// Slice benches (budgets in A9 cycles, 1 cycle = 1 ns at 1 GHz):
//   bench_slice_pad_region   pad → slice → region as the engine does it per SLICE hit (SlicePadLocks + TrackSource's
//                            sliceRegion): ≤ 20 cycles per trigger, 64 hits per iteration → 1280
//   bench_slice_build        one slice table per edit on the message thread (64 points, moved + added): < 10 µs
//   bench_slice_zero_cross   SLC-006 zero-crossing snap over ±5 ms at 44.1 kHz (NEON on the device): ≤ 0.1 ms
//   bench_slice_block        a router + track-source block with 16 SLICE pad hits on one track (live path end to end)
#include "core/commands/slice/SlicePoints.h"
#include "engine/modulation/AmpEnvelopeBank.h"
#include "engine/trigger/TrackSource.h"
#include "engine/trigger/TriggerRouter.h"
#include "engine/voice/SliceMapPads.h"
#include "services/analysis/SliceDetect.h"
#include "tests/bench/harness/Bench.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
struct Buffer
{
    std::vector<float> data;
    core::audio::SampleBuffer buf {};
    explicit Buffer(std::int64_t frames)
    {
        data.assign(static_cast<std::size_t>(frames + 16), 0.0f);
        for (std::int64_t i = 0; i < frames; ++i) // a low tone with few crossings: the scan reads the whole window
            data[static_cast<std::size_t>(i + 8)] = 0.5f * std::sin(static_cast<float>(i) * 0.002f) + 0.01f;
        buf.frames = frames;
        buf.channels = 1;
        buf.sampleRate = 44100.0;
        buf.ch[0] = buf.ch[1] = data.data() + 8;
        buf.contentId = 0;
        buf.refs.store(1);
    }
};

core::model::SliceSettings edited()
{
    core::model::SliceSettings s;
    s.gridCount = 32;
    for (int g = 1; g < 32; g += 3)
        s.grids.insert(s.grids.end(), {g, static_cast<std::int64_t>(g) * 5000 + 777});
    for (int a = 0; a < 20; ++a)
        s.grids.insert(s.grids.end(), {core::slice::kAddedTag, static_cast<std::int64_t>(a) * 7919 + 1234});
    return s;
}
} // namespace

BENCH(bench_slice_pad_region, 1280)
{
    core::model::SliceSettings s;
    s.gridCount = 64;
    s.windowOffset = 2;
    const auto table = core::slice::buildTable(s, 1 << 20);
    engine::voice::SlicePadLocks pads;
    pads.setMap(3, &table);
    state.run([&] {
        std::int64_t sum = 0;
        for (int i = 0; i < 64; ++i)
        {
            const int slice = pads.slice(3, i & 15);
            const auto* lock = pads.lock(slice);
            float v = 0;
            lock->get(engine::voice::kLockSlotSlice, v);
            engine::voice::SliceRegion r;
            if (engine::voice::sliceRegion(&table, 1 << 20, static_cast<int>(v), r))
                sum += r.end - r.start;
        }
        pads.endBlock();
        bench::doNotOptimize(sum);
    });
}

BENCH(bench_slice_build, 10000)
{
    const auto s = edited();
    state.run([&] {
        const auto t = core::slice::buildTable(s, 400000);
        bench::doNotOptimize(t);
    });
}

BENCH(bench_slice_zero_cross, 100000)
{
    Buffer b(441000);
    const int window = services::analysis::windowFrames(b.buf);
    std::int64_t at = 1000;
    state.run([&] {
        const auto z = services::analysis::nearestZeroCrossing(b.buf, at, window);
        at = (at + 99991) % 440000;
        bench::doNotOptimize(z);
    });
}

BENCH(bench_slice_block, 30000)
{
    using namespace engine::trigger;
    constexpr std::int64_t frames = 64 * 4096;
    Buffer b(frames);
    auto bank = std::make_unique<engine::modulation::AmpEnvelopeBank>();
    auto sources = std::make_unique<std::array<TrackSource, engine::kTracks>>();
    auto router = std::make_unique<TriggerRouter>();
    engine::voice::SlicePadLocks pads;
    bank->prepare(44100.0);
    for (int t = 0; t < engine::kTracks; ++t)
    {
        (*sources)[static_cast<std::size_t>(t)].attach(bank.get(), t);
        (*sources)[static_cast<std::size_t>(t)].prepare(44100.0, engine::kBlockFrames);
    }
    router->attach(*sources, nullptr);
    router->prepare(44100.0, engine::kBlockFrames);
    router->setSlicePads(&pads);
    router->setPadParam(PadField::Mode, static_cast<float>(PadMode::Slice));
    core::model::SliceSettings s;
    s.gridCount = 64;
    const auto table = core::slice::buildTable(s, frames);
    (*sources)[0].setSample(&b.buf);
    (*sources)[0].setSliceMap(&table);
    pads.setMap(0, &table);
    engine::BlockContext ctx {};
    ctx.sampleRate = 44100.0;
    ctx.frames = engine::kBlockFrames;
    alignas(16) float l[engine::kBlockFrames], r[engine::kBlockFrames];
    int pad = 0;
    state.run([&] {
        for (int i = 0; i < 16; ++i)
            router->pushLive(engine::TriggerNow {0, 100, 60, static_cast<std::int8_t>((pad + i) & 15)});
        pad = (pad + 1) & 15;
        router->process(ctx);
        (*sources)[0].render(ctx, l, r);
        bench::doNotOptimize(l);
    });
    (*sources)[0].setSample(nullptr);
}
