// Offline render with the live engine's code (REN-001…005). Generated projects only
// (RenderTestSupport.h). The "live" engine in the parity case is a second engine::createEngine driven the way the device
// drives the app's engine: 256-frame periods through AudioProcessor::process (the engine's FIFO adapter cuts them into
// 64-frame blocks) and the app's paced QueuedBridge flushed once per period.
#include "services/library/AudioFile.h"
#include "services/library/SidecarStore.h"
#include "services/render/AudioWriter.h"
#include "services/render/RenderService.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/unit/library/LibTestAudio.h"
#include "tests/unit/render/offline/RenderTestSupport.h"

#include "core/params/ParamRegistry.h"
#include "engine/Engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>

namespace rnd = services::render;
using rendertest::makeFixture;

namespace
{
rnd::Rendered renderJob(const rendertest::Fixture& f, const rnd::RenderJob& job)
{
    return rnd::render(*f.project, job, f.samples, app::feed::feeder(true), f.rate);
}

rnd::RenderJob pattern(int repeats = 1)
{
    rnd::RenderJob j;
    j.source = rnd::Source::Pattern;
    j.repeats = repeats;
    return j;
}

core::ParamId id(std::string_view name)
{
    return core::builtinParams().idOf(name).value_or(core::ParamId {});
}

bool same(const std::vector<float>& a, const std::vector<float>& b, std::size_t n)
{
    return a.size() >= n && b.size() >= n && std::memcmp(a.data(), b.data(), n * sizeof(float)) == 0;
}
} // namespace

TEST_CASE("offline render: a pattern renders its exact length, bit-identically on every run (REN-001/002)")
{
    const auto f = makeFixture();
    const auto a = renderJob(f, pattern());
    const auto b = renderJob(f, pattern());
    REQUIRE(a.left.size() == 88200); // 16 steps at 120 BPM, 44.1 kHz
    CHECK(a.nominalFrames == 88200);
    CHECK(a.hash == b.hash);
    CHECK(same(a.left, b.left, a.left.size()));
    CHECK(same(a.right, b.right, a.right.size()));
    CHECK(rendertest::peak(a.left) > 0.05);
    CHECK(a.sixteenths == 16);
    CHECK(a.bpm == Catch::Approx(120.0));
    // Repeats and an explicit length in bars.
    CHECK(renderJob(f, pattern(3)).left.size() == 3 * 88200);
    auto half = pattern();
    half.bars = 0.5;
    const auto h = renderJob(f, half);
    CHECK(h.left.size() == 44100);
    CHECK(same(h.left, a.left, 44100)); // the same audio, cut earlier
}

TEST_CASE("offline render: a project without samples renders digital silence (no test tone)")
{
    auto f = makeFixture();
    f.samples = {};
    const auto r = renderJob(f, pattern());
    REQUIRE(r.left.size() == 88200);
    CHECK(rendertest::peak(r.left) == 0.0);
    CHECK(rendertest::peak(r.right) == 0.0);
}

TEST_CASE("offline render: the offline render equals the live engine's output of the same pattern bit for bit")
{
    const auto f = makeFixture();
    const auto offline = renderJob(f, pattern(2));
    REQUIRE(offline.left.size() == 2 * 88200);

    // The live engine: the app's set-up, device periods of 256 frames, posts paced per period.
    auto live = engine::createEngine(rnd::builtinEngineSetup());
    live->prepare(f.rate, 256);
    rnd::QueuedBridge q(live->bridge(), 1024);
    app::feed::forwardAll(*f.project, q);
    app::feed::postPlan(*f.project, q, nullptr);
    app::feed::postSlices(*f.project, q, f.samples);
    for (int s = 0; s < 2; ++s)
    {
        core::audio::retain(f.samples.at(0, s));
        REQUIRE(q.post(engine::ObjectSwap {{engine::SwapSlot::TrackSample, 0, static_cast<std::uint8_t>(s)},
                                           f.samples.at(0, s)}));
    }
    q.post(engine::ParamChange {id("arrangement.loop"), 0, 0.0f, 0});
    q.post(engine::ParamChange {id("arrangement.play"), 0, 0.0f, 0});
    q.post(engine::ParamChange {id("scene.queue"), 0xFFFF, 0.0f, 0});
    q.post(engine::TransportCmd {engine::TransportCmd::SetTempo, 120.0});
    std::vector<float> l(256), r(256), outL, outR;
    float* const out[2] = {l.data(), r.data()};
    const engine::AudioIO io {out, 1, nullptr, 0, 256, 0};
    for (int period = 0, idle = 0; idle < 4; ++period) // until everything is posted and applied (stopped: silence)
    {
        idle = q.flush() ? idle + 1 : 0;
        live->process(io);
        REQUIRE(period < 10000);
    }
    q.post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    while (outL.size() < offline.left.size())
    {
        q.flush();
        live->process(io);
        outL.insert(outL.end(), l.begin(), l.end());
        outR.insert(outR.end(), r.begin(), r.end());
    }
    // The same block alignment (Play at a block start, PPQ 0 at frame 0 of the take): identical samples.
    std::size_t first = 0;
    while (first < offline.left.size() && outL[first] == offline.left[first] && outR[first] == offline.right[first])
        ++first;
    INFO("first difference at frame " << first);
    CHECK(same(outL, offline.left, offline.left.size()));
    CHECK(same(outR, offline.right, offline.right.size()));
}

TEST_CASE("offline render: the live engine's resample tap equals the offline render, also while muted (REN-005/007)")
{
    // REN-007's cross-check at the engine boundary: SOURCE RESAMPLE records OutputRouter::resampleTap (the master after
    // the master fader, before the output trim and the mute-all fade; checked at the tap, not through the recorder).
    // The live engine renders the pattern while render.muteLive fades its outputs: MAIN goes silent, the tap does not.
    const auto f = makeFixture();
    const auto offline = renderJob(f, pattern());
    engine::EngineOptions options;
    auto live = std::make_unique<engine::Engine>(rnd::builtinEngineSetup(), options);
    live->prepare(f.rate, 64);
    rnd::QueuedBridge q(live->bridge(), 512);
    q.post(engine::ParamChange {id("render.offline"), 0, 1.0f, 0}); // as the offline engine (no test tone)
    app::feed::forwardAll(*f.project, q);
    app::feed::postPlan(*f.project, q, nullptr);
    app::feed::postSlices(*f.project, q, f.samples);
    for (int s = 0; s < 2; ++s)
    {
        core::audio::retain(f.samples.at(0, s));
        REQUIRE(q.post(engine::ObjectSwap {{engine::SwapSlot::TrackSample, 0, static_cast<std::uint8_t>(s)},
                                           f.samples.at(0, s)}));
    }
    q.post(engine::ParamChange {id("arrangement.loop"), 0, 0.0f, 0});
    q.post(engine::ParamChange {id("arrangement.play"), 0, 0.0f, 0});
    q.post(engine::ParamChange {id("scene.queue"), 0xFFFF, 0.0f, 0});
    q.post(engine::TransportCmd {engine::TransportCmd::SetTempo, 120.0});
    std::vector<float> l(64), r(64), tapL, tapR, mainL;
    float* const out[2] = {l.data(), r.data()};
    const engine::AudioIO io {out, 1, nullptr, 0, 64, 0};
    for (int block = 0, idle = 0; idle < 16; ++block)
    {
        idle = q.flush() ? idle + 1 : 0;
        live->process(io);
        REQUIRE(block < 100000);
    }
    q.post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    q.post(engine::ParamChange {id("render.muteLive"), 0, 1.0f, 0});
    while (tapL.size() < offline.left.size())
    {
        q.flush();
        live->process(io);
        const float* tl = live->routing().resampleTap(0);
        const float* tr = live->routing().resampleTap(1);
        tapL.insert(tapL.end(), tl, tl + 64);
        tapR.insert(tapR.end(), tr, tr + 64);
        mainL.insert(mainL.end(), l.begin(), l.end());
    }
    double diff = 0;
    for (std::size_t i = 0; i < offline.left.size(); ++i)
        diff = std::max({diff, static_cast<double>(std::fabs(tapL[i] - offline.left[i])),
                         static_cast<double>(std::fabs(tapR[i] - offline.right[i]))});
    INFO("max difference " << diff);
    CHECK(diff <= 1e-5); // within -100 dBFS (here: identical, the output trim is unity)
    CHECK(rendertest::peak(mainL, 64) == 0.0); // MAIN silent after the 64-sample fade (REN-005)
    CHECK(rendertest::peak(tapL) > 0.05);
}

TEST_CASE("offline render: a track renders alone, with or without the bus FX (REN-001 Track)")
{
    auto f = makeFixture();
    auto job = pattern();
    job.source = rnd::Source::Track;
    job.track = 0;
    job.includeBusFx = true;
    const auto soloed = renderJob(f, job);
    // The same project with track 2's sample gone: the master then carries track 1 alone.
    auto g = makeFixture();
    g.samples.set(0, 1, nullptr);
    const auto alone = renderJob(g, pattern());
    REQUIRE(soloed.left.size() == alone.left.size());
    CHECK(same(soloed.left, alone.left, alone.left.size()));
    CHECK(same(soloed.right, alone.right, alone.right.size()));

    job.includeBusFx = false; // OUT 3-4 of 8CH: post insert FX and fader, nothing of the master
    job.track = 1;
    const auto direct = renderJob(f, job);
    CHECK(direct.left.size() == 88200);
    CHECK(rendertest::peak(direct.left) > 0.01);
    job.track = 5; // an empty track
    CHECK(rendertest::peak(renderJob(f, job).left) == 0.0);
}

TEST_CASE("offline render: REMAINDER LEAVE appends the tail until -90 dBFS, WRAP folds it onto the start (REN-002)")
{
    const auto f = makeFixture(true);
    const auto cut = renderJob(f, pattern());
    auto leave = pattern();
    leave.tails = true;
    const auto tail = renderJob(f, leave);
    REQUIRE(cut.left.size() == 88200);
    CHECK(tail.left.size() > 88200 + 44100 / 2); // the ring started at step 16 (1.875 s) lasts ~1 s
    CHECK(tail.left.size() < 88200 + 10 * 44100);
    CHECK(tail.nominalFrames == 88200);
    const std::size_t blockStart = 88200 / 64 * 64; // the transport pauses at the block that contains the end
    CHECK(same(tail.left, cut.left, blockStart));
    // The ring (1 s from step 16 = frame 82687.5) is the last sound: the tail ends with it (trimmed after the last
    // sample at or above -90 dBFS).
    CHECK(std::llabs(static_cast<long long>(tail.left.size()) - (82688 + 44100)) <= 64);
    CHECK(rendertest::peak(tail.left, 88200, 88200 + 4410) > 0.01);           // the ring sounds past the end

    auto wrap = pattern();
    wrap.wrapTail = true;
    const auto w = renderJob(f, wrap);
    REQUIRE(w.left.size() == 88200);
    // Start = the cut render + the tail; later (past the tail's wrapped length) the cut render.
    for (std::size_t i = 0; i < 1000; ++i)
        CHECK(w.left[i] == cut.left[i] + tail.left[88200 + i]);
}

TEST_CASE("offline render: a scene renders its patterns with content in turn, an arrangement until it stops")
{
    auto f = makeFixture();
    rnd::RenderJob scene;
    scene.source = rnd::Source::Scene;
    const auto s = renderJob(f, scene);
    CHECK(s.sixteenths == 16 + 8);
    CHECK(s.left.size() == static_cast<std::size_t>(std::llround(24 * 15.0 / 120.0 * 44100)));
    // The second pattern (kick on 1 and 5 of 8) follows the first: the start of pattern 2 sounds.
    CHECK(rendertest::peak(s.left, 88200, 88200 + 2000) > 0.05);

    // Song: 1.1 once, 1.2 twice at 140 BPM.
    auto& arr = f.project->arrangement.sections;
    arr.push_back({0, 0, 1, std::nullopt, {}});
    arr.push_back({0, 1, 2, 140.0, {}});
    rnd::RenderJob song;
    song.source = rnd::Source::Arrangement;
    const auto a = renderJob(f, song);
    CHECK(a.sixteenths == 32);
    const double expect = 16 * 15.0 / 120.0 * 44100 + 16 * 15.0 / 140.0 * 44100;
    CHECK(std::fabs(static_cast<double>(a.left.size()) - expect) <= 3 * 64);
    CHECK(renderJob(f, song).hash == a.hash);
}

TEST_CASE("offline render: files in 16 / 24 / 32f with the sidecar, normalized after render (REN-002/003/010)")
{
    libtest::TempDir tmp("render-files");
    const auto f = makeFixture();
    const auto r = renderJob(f, pattern());
    for (const int bits : {16, 24, 32})
    {
        auto job = pattern();
        job.bitDepth = bits;
        const auto file = rnd::renderFileFor(tmp.dir(), "Demo", job);
        CHECK(file.getFileName() == juce::String("Demo_1_1_") + juce::String(bits == 16 ? 1 : (bits == 24 ? 2 : 3)) + ".wav");
        const auto w = rnd::writeRendered(r, job, file);
        REQUIRE(w);
        CHECK(w.value().bars == 1.0);
        auto info = services::library::probeAudioFile(file);
        REQUIRE(info);
        CHECK(info.value().frames == 88200);
        CHECK(info.value().channels == 2);
        CHECK(info.value().encoding == (bits == 16   ? services::library::PcmEncoding::S16
                                        : bits == 24 ? services::library::PcmEncoding::S24
                                                     : services::library::PcmEncoding::F32));
        const auto meta = services::library::SidecarStore().read(file);
        CHECK(meta.bars == 1.0);
        CHECK(meta.bpm == Catch::Approx(120.0));
    }
    auto norm = pattern();
    norm.normalize = true;
    norm.bitDepth = 24;
    const auto file = tmp / "norm.wav";
    const auto w = rnd::writeRendered(r, norm, file);
    REQUIRE(w);
    REQUIRE(w.value().normalized);
    CHECK(w.value().normalized->changed);
    // A second normalize finds the file at the target already.
    const auto again = services::normalize::normalize(file, tmp / "again.wav", -1.0);
    REQUIRE(again);
    CHECK(again.value().atTarget);
    CHECK_FALSE((tmp / "norm.normalizing.wav").exists());
}

TEST_CASE("offline render: the writer's WAV / AIFF files read back exactly")
{
    libtest::TempDir tmp("render-writer");
    std::vector<float> l(1000), r(1000);
    for (int i = 0; i < 1000; ++i)
    {
        l[static_cast<std::size_t>(i)] = static_cast<float>(i - 500) / 512.0f;
        r[static_cast<std::size_t>(i)] = -l[static_cast<std::size_t>(i)] * 0.5f;
    }
    for (const auto fmt : {rnd::SampleFormat::Pcm16, rnd::SampleFormat::Pcm24, rnd::SampleFormat::Float32})
        for (const bool aiff : {false, true})
        {
            const auto file = tmp.dir().getChildFile(juce::String("w") + juce::String(static_cast<int>(fmt)) + (aiff ? ".aif" : ".wav"));
            auto w = rnd::AudioFileWriter::create(file, {fmt, 2, 48000, aiff});
            REQUIRE(w);
            const float* ch[2] = {l.data(), r.data()};
            REQUIRE(w.value()->writePlanar(ch, 1000));
            REQUIRE(w.value()->finish());
            CHECK_FALSE(file.getSiblingFile(file.getFileName() + ".part").exists());
            auto info = services::library::probeAudioFile(file);
            REQUIRE(info);
            CHECK(info.value().frames == 1000);
            CHECK(info.value().sampleRate == 48000);
            CHECK(info.value().container == (aiff ? services::library::AudioContainer::Aiff : services::library::AudioContainer::Wav));
            juce::MemoryBlock mb;
            REQUIRE(file.loadFileAsData(mb));
            std::vector<float> dl(1000), dr(1000);
            float* out[2] = {dl.data(), dr.data()};
            services::library::convertPcm(static_cast<const unsigned char*>(mb.getData()) + info.value().dataOffset,
                                          info.value().encoding, info.value().bigEndian, 2, 1000, out, 2);
            for (int i = 0; i < 1000; ++i) // x / 512 is exact in every format (±1 → clamp at +1 − 1 LSB)
                if (l[static_cast<std::size_t>(i)] < 0.99f)
                {
                    CHECK(dl[static_cast<std::size_t>(i)] == l[static_cast<std::size_t>(i)]);
                    CHECK(dr[static_cast<std::size_t>(i)] == r[static_cast<std::size_t>(i)]);
                }
        }
    // Destroyed without finish(): no file under the target's name, no part file.
    {
        auto w = rnd::AudioFileWriter::create(tmp / "gone.wav", {});
        REQUIRE(w);
    }
    CHECK_FALSE((tmp / "gone.wav").exists());
    CHECK_FALSE((tmp / "gone.wav.part").exists());
}

TEST_CASE("offline render: the service renders on its worker, mutes the live output and assigns to a track (REN-003…005)")
{
    libtest::TempDir tmp("render-service");
    auto f = makeFixture();
    fakes::FakeEngineBridge live;
    std::vector<std::pair<int, int>> assigned;
    rnd::RenderService service({.feed = app::feed::feeder(true),
                                .samples = [&f] { return f.samples; },
                                .live = &live,
                                .sampleRate = 44100,
                                .outputDir = tmp / "RenderedAudio",
                                .projectName = [] { return std::string("My Song"); },
                                .assignToTrack = [&](int s, int t, const juce::File&) { assigned.push_back({s, t}); }});
    const auto mutes = [&] {
        std::vector<float> v;
        for (const auto& m : live.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == id("render.muteLive"))
                v.push_back(c->value);
        return v;
    };
    auto runToEnd = [&](rnd::JobHandle h, std::optional<core::Result<rnd::RenderResult>>& out) {
        REQUIRE(h != 0);
        for (int i = 0; i < 20000 && ! out; ++i)
        {
            service.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(out);
    };

    std::optional<core::Result<rnd::RenderResult>> done;
    float lastProgress = 0;
    auto job = pattern();
    job.toTrack = true;
    const auto h = service.start(*f.project, job, [&](float x) { lastProgress = x; },
                                 [&](core::Result<rnd::RenderResult> r) { done = std::move(r); });
    CHECK(service.busy());
    CHECK(mutes() == std::vector<float> {1.0f}); // REN-005: muted at the start
    // A second job while busy is refused at once.
    std::optional<core::Result<rnd::RenderResult>> refused;
    CHECK(service.start(*f.project, job, {}, [&](core::Result<rnd::RenderResult> r) { refused = std::move(r); }) == 0);
    REQUIRE(refused);
    CHECK_FALSE(*refused);
    runToEnd(h, done);
    REQUIRE(*done);
    CHECK_FALSE(service.busy());
    CHECK(lastProgress == 1.0f);
    CHECK(mutes() == std::vector<float> {1.0f, 0.0f}); // restored on done
    CHECK(done->value().file.getFileName() == "My Song_1_1_1.wav");
    CHECK(done->value().file.existsAsFile());
    CHECK(done->value().frames == 88200);
    CHECK(assigned == std::vector<std::pair<int, int>> {{0, 2}}); // REN-004: the first empty track (3)
    CHECK(done->value().assignedTrack == 2);

    // Cancel: the live output comes back, nothing is assigned, no file.
    done.reset();
    rnd::RenderJob song;
    song.source = rnd::Source::Pattern;
    song.repeats = 64;
    const auto h2 = service.start(*f.project, song, {}, [&](core::Result<rnd::RenderResult> r) { done = std::move(r); });
    service.cancel(h2);
    runToEnd(h2, done);
    REQUIRE(*done);
    CHECK(done->value().cancelled);
    CHECK(mutes() == std::vector<float> {1.0f, 0.0f, 1.0f, 0.0f});
    CHECK_FALSE((tmp / "RenderedAudio").getChildFile("My Song_1_1_2.wav").exists());
    CHECK(assigned.size() == 1);
}

TEST_CASE("offline render: the job follows the render.* preferences; helpers")
{
    const auto j = rnd::jobFromPreferences(
        [](std::string_view k) -> float
        {
            if (k == "render.source") return 3;
            if (k == "render.repeats") return 4;
            if (k == "render.remainder") return 1;
            if (k == "render.bitDepth") return 2;
            if (k == "render.destination") return 1;
            if (k == "render.normalizeTarget") return -3;
            return 0;
        },
        2, 5, 7);
    CHECK(j.source == rnd::Source::Arrangement);
    CHECK(j.repeats == 4);
    CHECK(j.wrapTail);
    CHECK_FALSE(j.tails);
    CHECK(j.bitDepth == 32);
    CHECK(j.toTrack);
    CHECK_FALSE(j.includeBusFx);
    CHECK(j.normalizeDbfs == -3.0);
    CHECK(j.scene == 2);
    CHECK(j.pattern == 5);
    CHECK(j.track == 7);
    for (const auto* k : {"render.source", "render.repeats", "render.bars", "render.remainder", "render.includeFx",
                          "render.bitDepth", "render.normalize", "render.destination", "render.normalizeTarget",
                          "render.muteLive"})
        CHECK(core::builtinParams().find(k) != nullptr);
    const auto f = makeFixture();
    CHECK(rnd::firstEmptyTrack(*f.project, 0) == 2);
    CHECK(rnd::firstEmptyTrack(*f.project, 1) == 0);
    rnd::RenderJob a;
    a.source = rnd::Source::Arrangement;
    CHECK(rnd::renderFileFor(juce::File("/nonexistent-dir"), "P", a).getFileName() == "P_ARR_1.wav");
}
