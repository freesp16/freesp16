// Offline render, normalize and peak benchmarks. Host
// numbers are ns at 1 GHz (harness convention) and rank / guard only; A9 numbers come from the hardware (open).
//
//   bench_render_offline    one 64-frame block of the offline engine (OfflineEngine::process) playing the NFR-001
//                           scene: 16 tracks with samples hitting every step, 2 insert FX each (FILTER + EQ), the
//                           Reverb send, the master Compressor. Budget: ≥ 1.8× real time = 64 / 44100 s / 1.8
//                           = 806 µs per block (806 k cycles)
//   bench_render_normalize  normalize of 1 min stereo PCM24 (read, peak, gain, write; REN-010): ≤ 1 s
//   bench_render_peaks      the peak overview of 1 min stereo (REN-020, NEON on the device): ≤ 0.3 s
// The last two time themselves (one iteration = one minute of audio, median of 5) and report through bench::record.
#include "tests/bench/harness/Bench.h"

#include "app/ProjectFeed.cpp" // the app's project forwarding (the app executable is not a library)
#include "core/audio/SampleBufferStorage.h"
#include "services/analysis/Analysis.h"
#include "services/normalize/Normalizer.h"
#include "services/render/OfflineEngine.h"
#include "tests/unit/library/LibTestAudio.h"
#include "tests/unit/render/offline/RenderTestSupport.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace
{
namespace rnd = services::render;
namespace model = core::model;

rendertest::Fixture nfr001()
{
    auto f = rendertest::makeFixture();
    auto& sc = f.project->scenes[0];
    for (int t = 0; t < 16; ++t)
    {
        auto& tr = sc.tracks[static_cast<std::size_t>(t)];
        tr.playback.sample.id = "s" + std::to_string(t);
        tr.playback.length = f.buffers[static_cast<std::size_t>(t % 2)]->frames;
        tr.insertFx[0].type = model::EffectType::MultiModeFilter;
        tr.insertFx[1].type = model::EffectType::ParametricEq2;
        tr.send.amount = 64;
        f.samples.set(0, t, f.buffers[static_cast<std::size_t>(t % 2)].get());
        for (int s = 0; s < 16; ++s)
            rendertest::hit(sc.patterns[0], s, t, 60 + 4 * t);
    }
    sc.sendFx.type = model::EffectType::Reverb;
    sc.masterFx.type = model::EffectType::Compressor;
    f.project->settings.tempo = 140.0;
    return f;
}

core::ParamId idOf(std::string_view n)
{
    return core::builtinParams().idOf(n).value_or(core::ParamId {});
}

template <typename Fn> double medianMs(int runs, Fn&& fn)
{
    std::vector<double> ms;
    for (int i = 0; i < runs; ++i)
    {
        const auto t0 = bench::nowNs();
        fn();
        ms.push_back(static_cast<double>(bench::nowNs() - t0) / 1.0e6);
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}
} // namespace

BENCH(bench_render_offline, 806000)
{
    const auto f = nfr001();
    rnd::OfflineEngine e({44100.0, 1});
    e.begin(*f.project, f.samples, app::feed::feeder(true), nullptr);
    e.bridge().post(engine::ParamChange {idOf("arrangement.play"), 0, 0.0f, 0});
    e.bridge().post(engine::ParamChange {idOf("scene.queue"), 0xFFFF, 0.0f, 0});
    e.bridge().post(engine::TransportCmd {engine::TransportCmd::SetTempo, 140.0});
    e.settle();
    e.bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    for (int i = 0; i < 100; ++i)
        e.process();
    state.run([&] {
        e.process();
        bench::doNotOptimize(e.out(0, 0)[0]);
    });
}

TEST_CASE("bench_render_normalize", "[bench]")
{
    libtest::TempDir tmp("bench-norm");
    libtest::Spec s;
    s.encoding = services::library::PcmEncoding::S24;
    s.channels = 2;
    const std::int64_t frames = 60 * 44100;
    std::vector<std::vector<float>> planar {libtest::noise(frames, 1, 0.5), libtest::noise(frames, 2, 0.5)};
    REQUIRE(libtest::writeFile(tmp / "minute.wav", libtest::encode(s, planar)));
    int n = 0;
    const double ms = medianMs(5, [&] {
        const auto out = tmp.dir().getChildFile("out" + juce::String(n++) + ".wav");
        const auto r = services::normalize::normalize(tmp / "minute.wav", out, -1.0);
        REQUIRE(r);
        REQUIRE(r.value().changed);
        out.deleteFile();
    });
    bench::record("bench_render_normalize", bench::msToCycles(1000.0), bench::msToCycles(ms));
    CHECK(ms <= 1000.0);
}

TEST_CASE("bench_render_peaks", "[bench]")
{
    const std::int64_t frames = 60 * 44100;
    std::unique_ptr<core::audio::SampleBuffer, rendertest::BufferDeleter> b(
        core::audio::allocateSampleBuffer(frames, 2, 44100));
    for (int c = 0; c < 2; ++c)
    {
        const auto v = libtest::noise(frames, static_cast<std::uint32_t>(c + 5), 0.7);
        std::copy(v.begin(), v.end(), core::audio::writableChannel(b.get(), c));
    }
    std::size_t buckets = 0;
    const double ms = medianMs(5, [&] { buckets += services::analysis::peaks(*b).min.size(); });
    bench::record("bench_render_peaks", bench::msToCycles(300.0), bench::msToCycles(ms));
    CHECK(buckets == 5 * static_cast<std::size_t>((frames + 255) / 256));
    CHECK(ms <= 300.0);
}
