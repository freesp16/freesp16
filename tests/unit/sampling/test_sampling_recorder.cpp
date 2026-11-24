// The Recorder thread, the session and the live take on a track ("the WAV is 24-bit, engine rate, with sidecar", "a
// 10-minute recording keeps RAM flat (≤ 1 MB growth)", "loop-after-rec starts on the boundary", "nudge ±1 frame…±1 beat
// moves the window exactly"; SMP-005/007/009/023/024/025).
#include "SamplingTestUtil.h"

#include "services/library/AudioFile.h"
#include "services/library/SidecarStore.h"
#include "services/library/Workers.h"
#include "services/sampling/Recorder.h"
#include "services/sampling/SamplingSession.h"
#include "services/sampling/TakeBuilder.h"
#include "services/sampling/WavWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <optional>
#include <thread>
#include <unistd.h>

using namespace smptest;
using core::sampling::LiveState;
using services::sampling::TakeInfo;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("smp-test", "", false);
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

std::function<core::ParamId(std::string_view)> resolver()
{
    return [](std::string_view s) { return core::builtinParams().idOf(s).value_or(core::ParamId {}); };
}

InputFn ramp()
{
    return [](std::int64_t pos, int n, float* l, float* r) {
        for (int i = 0; i < n; ++i)
        {
            const auto k = static_cast<std::uint32_t>(pos + i);
            l[i] = static_cast<float>(static_cast<std::int32_t>(k * 2654435761u) >> 8) / 8388608.0f * 0.7f;
            r[i] = 0.25f * l[i];
        }
    };
}

// Reads a 24-bit PCM WAV's samples (planar float, /2^23).
struct Wav
{
    services::library::AudioFileInfo info {};
    std::vector<float> l, r;
};
Wav readWav(const juce::File& f)
{
    Wav w;
    auto probe = services::library::probeAudioFile(f);
    REQUIRE(probe.ok());
    w.info = probe.value();
    juce::MemoryBlock data;
    REQUIRE(f.loadFileAsData(data));
    const auto* p = static_cast<const unsigned char*>(data.getData()) + w.info.dataOffset;
    const int ch = w.info.channels;
    for (std::int64_t i = 0; i < w.info.frames; ++i)
        for (int c = 0; c < ch; ++c)
        {
            const unsigned char* s = p + (i * ch + c) * 3;
            const auto v = static_cast<std::int32_t>(std::uint32_t(s[2]) << 24 | std::uint32_t(s[1]) << 16 |
                                                     std::uint32_t(s[0]) << 8) >> 8;
            (c == 0 ? w.l : w.r).push_back(static_cast<float>(v) / 8388608.0f);
        }
    return w;
}

template <class Pred> bool waitFor(services::library::Workers& w, Pred done, int ms = 10000)
{
    for (int i = 0; i < ms / 2 && ! done(); ++i)
    {
        w.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    w.pump();
    return done();
}

std::int64_t rssBytes()
{
    std::ifstream f("/proc/self/statm");
    long size = 0, resident = 0;
    f >> size >> resident;
    return static_cast<std::int64_t>(resident) * ::sysconf(_SC_PAGESIZE);
}
} // namespace

TEST_CASE("sampling.wav: S24 conversion rounds, clamps and reads back exactly", "[sampling][recorder]")
{
    using services::sampling::WavWriter;
    CHECK(WavWriter::toS24(0.0f) == 0);
    CHECK(WavWriter::toS24(1.0f) == 8388607);
    CHECK(WavWriter::toS24(-1.0f) == -8388608);
    CHECK(WavWriter::toS24(-2.0f) == -8388608);
    CHECK(WavWriter::toS24(0.5f) == 4194304);
    CHECK(WavWriter::toS24(1.5f / 8388608.0f) == 2);
    TempDir t;
    const auto file = t.dir.getChildFile("a.wav");
    WavWriter w;
    REQUIRE(w.open(file, 48000, 1).ok());
    CHECK(WavWriter::partFor(file).existsAsFile());
    CHECK_FALSE(file.exists()); // not listed until finished
    const float x[3] = {0.25f, -0.5f, 0.125f};
    REQUIRE(w.write(x, nullptr, 3).ok());
    REQUIRE(w.finish().ok());
    CHECK_FALSE(WavWriter::partFor(file).exists());
    const auto wav = readWav(file);
    CHECK(wav.info.bitsPerSample == 24);
    CHECK(wav.info.channels == 1);
    CHECK(wav.info.sampleRate == 48000.0);
    REQUIRE(wav.l.size() == 3);
    CHECK(wav.l[1] == -0.5f);
}

TEST_CASE("sampling.recorder: REC LENGTH 2 bars → a 24-bit WAV at the engine rate with its sidecar", "[sampling][recorder]")
{
    TempDir t;
    auto e = make(48000.0);
    services::library::Workers workers(1);
    services::sampling::SamplingSession s(e->bridge(), workers, {t.dir.getChildFile("Recordings"), 48000.0, resolver()});
    REQUIRE(s.ensureCapture(120.0));
    Driver d;
    d.run(*e, 1024, ramp());
    REQUIRE(s.hub() != nullptr);
    REQUIRE(param(*e, "sampling.length", 2.0f));
    std::optional<TakeInfo> got;
    s.onTake = [&](const TakeInfo& i) { got = i; };
    auto file = s.record(0, 6, 2);
    REQUIRE(file.ok());
    CHECK(file.value().getFileName() == "REC 001.wav");
    d.run(*e, 2 * 96000 + 4096, ramp());
    REQUIRE(waitFor(workers, [&] { return got.has_value(); }));
    REQUIRE(got->ok);
    CHECK(got->frames == 2 * 96000);
    CHECK(got->track == 6);
    CHECK(got->bars == 2);
    const auto wav = readWav(got->file);
    CHECK(wav.info.bitsPerSample == 24);
    CHECK(wav.info.encoding == services::library::PcmEncoding::S24);
    CHECK(wav.info.sampleRate == 48000.0);
    CHECK(wav.info.channels == 2);
    REQUIRE(wav.info.frames == 2 * 96000);
    // The file is the ring's take, to the 24-bit LSB.
    std::vector<float> l(4096), r(4096);
    REQUIRE(s.hub()->ring.read(got->ringStart + 50000, 4096, l.data(), r.data()));
    for (int i = 0; i < 4096; ++i)
    {
        REQUIRE(std::fabs(wav.l[static_cast<std::size_t>(50000 + i)] - l[static_cast<std::size_t>(i)]) <= 0.6f / 8388608.0f);
        REQUIRE(std::fabs(wav.r[static_cast<std::size_t>(50000 + i)] - r[static_cast<std::size_t>(i)]) <= 0.6f / 8388608.0f);
    }
    core::model::SampleMetadata md;
    REQUIRE(services::library::SidecarStore().tryRead(got->file, md));
    CHECK(md.bpm == 120.0);
    CHECK(md.bars == 2.0);
    // The next take gets the next running number (SMP-009).
    CHECK(services::sampling::proposeName(t.dir.getChildFile("Recordings")).getFileName() == "REC 002.wav");
}

TEST_CASE("sampling.recorder: LINE IN L records a mono take; REC in standby cancels and deletes the part file",
          "[sampling][recorder]")
{
    TempDir t;
    auto e = make();
    services::library::Workers workers(1);
    services::sampling::SamplingSession s(e->bridge(), workers, {t.dir, 44100.0, resolver()});
    REQUIRE(s.ensureCapture(120.0));
    Driver d;
    d.run(*e, 512, ramp());
    REQUIRE(param(*e, "sampling.source", 1.0f));
    REQUIRE(param(*e, "sampling.length", 1.0f));
    std::optional<TakeInfo> got;
    s.onTake = [&](const TakeInfo& i) { got = i; };
    REQUIRE(s.record(0, 0, 1).ok());
    d.run(*e, 88200 + 2048, ramp());
    REQUIRE(waitFor(workers, [&] { return got.has_value(); }));
    REQUIRE(got->ok);
    CHECK(readWav(got->file).info.channels == 1);

    got.reset();
    d.run(*e, 256, {}); // silence from here (the adapter's last input block first)
    REQUIRE(param(*e, "sampling.threshold", 30.0f)); // −31 dB: silence keeps the take in standby
    auto f2 = s.record(0, 0, 2);
    REQUIRE(f2.ok());
    d.run(*e, 1024, {}); // no input: stays in standby
    s.stopRecord(0, 0);
    d.run(*e, 256, {});
    REQUIRE(waitFor(workers, [&] { return got.has_value(); }));
    CHECK(got->cancelled);
    CHECK_FALSE(f2.value().exists());
    CHECK_FALSE(services::sampling::WavWriter::partFor(f2.value()).exists());
}

TEST_CASE("sampling.recorder: a take dropped while recording deletes its part file (the pad again, SAMPLING/EDIT)",
          "[sampling][recorder][quantize]")
{
    TempDir t;
    auto e = make();
    services::library::Workers workers(1);
    services::sampling::SamplingSession s(e->bridge(), workers, {t.dir, 44100.0, resolver()});
    REQUIRE(s.ensureCapture(120.0));
    Driver d;
    d.run(*e, 512, ramp());
    std::optional<TakeInfo> got;
    s.onTake = [&](const TakeInfo& i) { got = i; };
    auto f = s.record(0, 3, 2);
    REQUIRE(f.ok());
    d.run(*e, 44100, ramp()); // a second of audio on its way to the disk
    s.dropRecord(0, 3);
    d.run(*e, 256, ramp());
    REQUIRE(waitFor(workers, [&] { return got.has_value(); }));
    CHECK(got->cancelled);
    CHECK_FALSE(got->ok);
    CHECK_FALSE(f.value().exists());
    CHECK_FALSE(services::sampling::WavWriter::partFor(f.value()).exists());
}

TEST_CASE("sampling.recorder: a 10-minute recording keeps RAM flat (≤ 1 MB growth)", "[sampling][recorder][ram]")
{
    TempDir t;
    auto e = make();
    auto hub = installHub(*e, 30.0);
    Driver d;
    d.period = 512;
    d.run(*e, 1024, ramp());
    services::sampling::Recorder rec(*hub.hub);
    REQUIRE(rec.start(t.dir.getChildFile("long.wav"), 44100.0, 2).ok());
    REQUIRE(param(*e, "sampling.rec", 1.0f));
    std::optional<TakeInfo> got;
    std::atomic<bool> done {false};
    rec.stop([&](TakeInfo i) {
        got = std::move(i);
        done = true;
    });
    const std::int64_t total = 10 * 60 * 44100;
    std::int64_t produced = 0, baseline = 0, peak = 0;
    while (produced < total)
    {
        d.run(*e, 44100, ramp()); // one second
        produced += 44100;
        // Keep the writer within 10 s of the head (the ring holds 30 s; a real-time recorder is never this far back).
        // Up to 60 s of waiting: this test renders faster than real time, and an ASan build under a parallel ctest
        // can leave the writer thread far behind; a writer that still lags then is a real failure.
        for (int i = 0; i < 60000 && produced - rec.framesWritten() > 10 * 44100; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(produced - rec.framesWritten() <= 10 * 44100);
        if (produced == 30 * 44100)
            baseline = rssBytes(); // after the first 30 s: every buffer of the path is allocated
        if (produced > 30 * 44100)
            peak = std::max(peak, rssBytes());
    }
    REQUIRE(param(*e, "sampling.rec", 0.0f));
    d.run(*e, 1024, ramp());
    for (int i = 0; i < 20000 && ! done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(done);
    REQUIRE(got->ok);
    CHECK(got->frames >= total);
    CHECK(got->file.getSize() == 44 + got->frames * 6);
    INFO("RSS growth after 30 s: " << (peak - baseline) << " bytes");
#if defined(__SANITIZE_ADDRESS__)
    (void) peak;
    SKIP("RSS flatness needs a build without sanitizers (ASan's allocator quarantine inflates RSS); the 10-minute take "
         "itself was checked above");
#else
    CHECK(peak - baseline <= 1024 * 1024);
#endif
}

TEST_CASE("sampling.session: live take with LOOP AFTER REC — installed on the track, the loop starts on a bar",
          "[sampling][live]")
{
    TempDir t;
    auto e = make();
    services::library::Workers workers(1);
    services::sampling::SamplingSession s(e->bridge(), workers, {t.dir, 44100.0, resolver()});
    REQUIRE(s.ensureCapture(120.0));
    REQUIRE(param(*e, "project.patternQuantize", 1.0f)); // BAR
    REQUIRE(param(*e, "live.mode", 1.0f));
    REQUIRE(param(*e, "live.length", 2.0f)); // 1 bar
    REQUIRE(param(*e, "live.loopAfterRec", 1.0f));
    const std::int64_t playAt = 8192, beat = 22050, bar = 88200;
    const InputFn in = clicks(playAt, beat, 0, 0.5f, 256); // 256-frame bursts on every beat
    Driver d;
    d.collect = true;
    d.run(*e, playAt, in);
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    d.run(*e, 30000, in);
    s.liveArm(0, 5);
    d.run(*e, 2 * bar - 30000 + 2048, in); // bar 2 recorded, into bar 3
    auto* hub = s.hub();
    REQUIRE(static_cast<LiveState>(hub->status.liveState.load()) == LiveState::Done);
    s.frame();
    REQUIRE(waitFor(workers, [&] { return s.liveBuffer() != nullptr; }));
    const auto* take = s.liveBuffer();
    CHECK(take->frames == bar + 2 * beat);
    CHECK(s.liveMargin() == beat);
    d.run(*e, bar + 4096, in);
    REQUIRE(static_cast<LiveState>(hub->status.liveState.load()) == LiveState::Looping);
    const std::int64_t origin = 64 * static_cast<std::int64_t>(e->blocksRendered() - hub->status.blocks.load());
    const std::int64_t loopRing = hub->status.loopStartFrame.load();
    const std::int64_t startT = hub->status.liveStart.load() - 64; // the transport frame the take started on (ring)
    CHECK((loopRing - startT) % bar == 0); // on a bar line
    CHECK(loopRing - startT >= bar);        // after the take
    // MAIN plays the take from the loop start: its first burst (window frame 0) at the boundary.
    const std::int64_t loopDevice = loopRing + origin;
    const std::int64_t onset = firstAbove(d.mainL.data(), static_cast<std::int64_t>(d.mainL.size()), 0.05f, loopDevice - 2000);
    CHECK(std::llabs(onset - loopDevice) <= 2);

    // NUDGE −1000: the window moves 1000 frames earlier; the loop restarts on the next bar, the burst 1000 frames later
    // (the old loop's own burst at the bar line fades out within its 256 frames: search after it).
    s.setNudge(-1000);
    d.run(*e, 2 * bar, in);
    REQUIRE(static_cast<LiveState>(hub->status.liveState.load()) == LiveState::Looping);
    CHECK(hub->status.liveNudge.load() == -1000);
    const std::int64_t loop2 = hub->status.loopStartFrame.load() + origin;
    CHECK(loop2 > loopDevice);
    CHECK((loop2 - loopDevice) % bar == 0);
    const std::int64_t onset2 = firstAbove(d.mainL.data(), static_cast<std::int64_t>(d.mainL.size()), 0.05f, loop2 + 400);
    CHECK(std::llabs(onset2 - (loop2 + 1000)) <= 2);
}

TEST_CASE("sampling.session: NUDGE ±1 frame … ±1 beat moves the saved window exactly (clamped to one beat)",
          "[sampling][live]")
{
    TempDir t;
    auto e = make();
    services::library::Workers workers(1);
    services::sampling::SamplingSession s(e->bridge(), workers, {t.dir, 44100.0, resolver()});
    REQUIRE(s.ensureCapture(120.0));
    REQUIRE(param(*e, "project.patternQuantize", 1.0f));
    REQUIRE(param(*e, "live.mode", 1.0f));
    REQUIRE(param(*e, "live.length", 2.0f));
    const std::int64_t playAt = 8192, beat = 22050, bar = 88200;
    const InputFn in = clicks(playAt, beat, 0); // 1-frame clicks on every beat
    Driver d;
    d.run(*e, playAt, in);
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    d.run(*e, 30000, in);
    s.liveArm(0, 1);
    d.run(*e, 2 * bar - 30000 + beat + 2048, in); // the take and its trailing margin are in the ring
    s.frame();
    REQUIRE(waitFor(workers, [&] { return s.liveBuffer() != nullptr; }));
    for (const std::int32_t nudge : {0, -1, 1, -1000, 1000, -22050, 22050, -30000, 30000})
    {
        INFO("nudge " << nudge);
        s.setNudge(nudge);
        d.run(*e, 256, in);
        const std::int32_t applied = std::clamp<std::int32_t>(nudge, -beat, beat);
        CHECK(s.hub()->status.liveNudge.load() == applied);
        std::optional<core::Result<juce::File>> saved;
        const auto file = t.dir.getChildFile("n" + juce::String(nudge) + ".wav");
        s.saveLive(file, [&](core::Result<juce::File> r) { saved.emplace(std::move(r)); });
        REQUIRE(waitFor(workers, [&] { return saved.has_value(); }));
        REQUIRE(saved->ok());
        const auto wav = readWav(file);
        REQUIRE(wav.info.frames == bar);
        // Clicks sit on every beat of the ring: the first one in the window is at (−nudge) mod beat.
        const std::int64_t want = ((-static_cast<std::int64_t>(applied)) % beat + beat) % beat;
        CHECK(firstAbove(wav.l.data(), static_cast<std::int64_t>(wav.l.size()), 0.25f) == want);
        core::model::SampleMetadata md;
        REQUIRE(services::library::SidecarStore().tryRead(file, md));
        CHECK(md.bars == 1.0);
    }
}

TEST_CASE("sampling.session: ring size — ≥ 30 s, 8 bars + margins when longer, capped at 64 s", "[sampling][live]")
{
    using S = services::sampling::SamplingSession;
    CHECK(S::ringFramesFor(120.0, 44100.0) == 30 * 44100);
    CHECK(S::ringFramesFor(50.0, 44100.0) == static_cast<std::int64_t>(34 * 60.0 / 50.0 * 44100 + 0.5)); // 40.8 s
    CHECK(S::ringFramesFor(20.0, 44100.0) == 64 * 44100);
    CHECK(S::liveLengthFits(5, 120.0, 44100.0, S::ringFramesFor(120.0, 44100.0)));
    CHECK_FALSE(S::liveLengthFits(5, 20.0, 44100.0, S::ringFramesFor(20.0, 44100.0))); // 8 bars at 20 BPM: 96 s
    CHECK(S::liveLengthFits(3, 20.0, 44100.0, S::ringFramesFor(20.0, 44100.0)));      // 2 bars: 24 s
}
