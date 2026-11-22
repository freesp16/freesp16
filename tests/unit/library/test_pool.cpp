// Sample pool, buffer references and the engine sync (LIB-030/031): the same file on
// 16 tracks is resident once, editing a file on disk reloads it on every track, the memory limit refuses, references
// are counted RT-safely and nothing is freed while referenced.
#include "LibTestSupport.h"

#include "core/audio/SampleBufferStorage.h"
#include "core/model/ModelOps.h"
#include "core/util/Realtime.h"
#include "services/library/TrackSampleSync.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace services::library;
using libtest::Fixture;
using libtest::SwapEngine;

namespace
{
std::unique_ptr<core::model::Project> projectWith(const std::vector<std::pair<int, core::model::SampleRef>>& tracks)
{
    auto p = core::model::newDefaultProject();
    for (const auto& [t, ref] : tracks)
    {
        if (p->sampleRefs.find({ref.id}) == nullptr)
            p->sampleRefs.entries.push_back(ref);
        p->scenes[0].tracks[static_cast<std::size_t>(t)].playback.sample = {ref.id};
    }
    return p;
}

core::ChangeSet trackChange(int scene, int track)
{
    core::ChangeSet c;
    c.add("scene[" + std::to_string(scene) + "].track[" + std::to_string(track) + "].playback.sample");
    return c;
}
} // namespace

TEST_CASE("lib.buffer: retain/release count without allocating on a real-time thread", "[library][buffer][realtime]")
{
    core::audio::SampleBuffer* b = core::audio::allocateSampleBuffer(100, 2, 44100);
    REQUIRE(b != nullptr);
    CHECK(core::audio::sampleBufferBytes(b) >= 2 * 108 * sizeof(float));
    const std::uint64_t zeros = core::audio::zeroReleaseCount();
    core::audio::retain(b);
    core::audio::retain(b);
    std::thread audio(
        [b]
        {
            core::rt::ScopedRealtimeThread rt; // APP_RT_CHECKS: any allocation here aborts
            core::audio::retain(b);
            core::audio::release(b);
            core::audio::release(b);
            core::audio::release(b); // reaches 0: only counted, never freed here
            core::audio::release(nullptr);
        });
    audio.join();
    CHECK(b->refs.load() == 0);
    CHECK(core::audio::zeroReleaseCount() == zeros + 1);
    CHECK(core::audio::allocateSampleBuffer(-1, 1, 44100) == nullptr);
    CHECK(core::audio::allocateSampleBuffer(10, 3, 44100) == nullptr);
    core::audio::freeSampleBuffer(b);
}

TEST_CASE("lib.pool: the same file on 16 tracks is resident once and installed on every slot", "[library][pool]")
{
    Fixture f;
    const auto file = libtest::writeSample(f.samples().getChildFile("kick.wav"));
    SwapEngine engine;
    TrackSampleSync sync(f.pool, engine);
    core::model::SampleRef ref{"sha256:kick", "$Lib/Samples/kick.wav", "kick", false, {}};
    std::vector<std::pair<int, core::model::SampleRef>> all;
    for (int t = 0; t < 16; ++t)
        all.push_back({t, ref});
    auto project = projectWith(all);
    sync.syncAll(*project);
    f.workers.drain();
    CHECK(f.pool.bufferCount() == 1);
    const auto* buffer = sync.current(0, 0);
    REQUIRE(buffer != nullptr);
    for (int t = 0; t < 16; ++t)
    {
        CHECK(sync.current(0, t) == buffer);
        CHECK(engine.track(0, t) == buffer);
    }
    CHECK(buffer->refs.load() == 32); // 16 document uses + 16 engine slots
    CHECK(f.pool.residentBytes() == static_cast<std::int64_t>(core::audio::sampleBufferBytes(buffer)));
    CHECK(f.pool.uses(file));
    CHECK(f.pool.overview(buffer).levels.size() >= 1);
    CHECK(f.pool.fileOf(buffer) == file);

    // A second file with the same audio shares the buffer (content key).
    REQUIRE(file.copyFileTo(f.samples().getChildFile("kick copy.wav")));
    bool shared = false;
    f.pool.requestFile(f.samples().getChildFile("kick copy.wav"), false,
                       [&](core::Result<SamplePool::Handle> r, const LoadInfo&)
                       { shared = r.ok() && r.value() == buffer; });
    f.workers.drain();
    CHECK(shared);
    CHECK(f.pool.bufferCount() == 1);
}

TEST_CASE("lib.pool: requests for one file while it decodes are served by one decode", "[library][pool]")
{
    Fixture f;
    const auto file = libtest::writeSample(f.samples().getChildFile("a.wav"), 44100);
    std::vector<SamplePool::Handle> got;
    for (int i = 0; i < 5; ++i)
        f.pool.requestFile(file, i == 4,
                           [&](core::Result<SamplePool::Handle> r, const LoadInfo& info)
                           {
                               REQUIRE(r.ok());
                               got.push_back(r.value());
                               if (got.size() == 5)
                                   CHECK(info.contentId.rfind("sha256:", 0) == 0);
                           });
    CHECK(f.pool.pendingCount() == 1);
    f.workers.drain();
    REQUIRE(got.size() == 5);
    for (auto* h : got)
        CHECK(h == got[0]);
    CHECK(got[0]->refs.load() == 0); // nobody kept it: cached, not freed
    CHECK(f.pool.bufferCount() == 1);
    // Cached: served without decoding.
    bool cached = false;
    f.pool.requestFile(file, false,
                       [&](core::Result<SamplePool::Handle>, const LoadInfo& info) { cached = info.cached; });
    CHECK(cached);
}

TEST_CASE("lib.pool: editing a file on disk reloads it on every track", "[library][pool]")
{
    Fixture f;
    const auto file = libtest::writeSample(f.samples().getChildFile("loop.wav"), 4410, 440);
    SwapEngine engine;
    TrackSampleSync sync(f.pool, engine);
    f.pool.onReplaced = [&](SamplePool::Handle a, SamplePool::Handle b) { sync.replace(a, b); };
    core::model::SampleRef ref{"sha256:loop", "$Lib/Samples/loop.wav", "loop", false, {}};
    auto project = projectWith({{2, ref}, {9, ref}});
    sync.syncAll(*project);
    f.workers.drain();
    const auto* before = sync.current(0, 2);
    REQUIRE(before != nullptr);

    // Unchanged file: nothing happens.
    f.pool.reloadAllIfChanged();
    f.workers.drain();
    CHECK(sync.current(0, 2) == before);

    libtest::writeSample(file, 8820, 880); // edited (new size and content)
    file.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::seconds(5));
    f.pool.reloadIfChanged(ref);
    f.workers.drain();
    const auto* after = sync.current(0, 2);
    REQUIRE(after != nullptr);
    CHECK(after != before);
    CHECK(after->frames == 8820);
    CHECK(sync.current(0, 9) == after);
    CHECK(engine.track(0, 2) == after);
    CHECK(engine.track(0, 9) == after);
    CHECK(before->refs.load() == 0);
    CHECK(f.pool.collect() == 1); // the replaced buffer goes at once
    CHECK(f.pool.bufferCount() == 1);
}

TEST_CASE("lib.pool: the memory limit refuses a sample; unreferenced buffers are evicted first", "[library][pool]")
{
    SamplePool::Config config;
    config.limitBytes = 300 * 1024; // room for one 1.5 s mono sample (≈ 260 KB), not two
    config.cacheBytes = 0;
    Fixture f(config);
    const auto a = libtest::writeSample(f.samples().getChildFile("a.wav"), 66150, 100);
    const auto b = libtest::writeSample(f.samples().getChildFile("b.wav"), 66150, 200);
    SamplePool::Handle held = nullptr;
    f.pool.requestFile(a, false,
                       [&](core::Result<SamplePool::Handle> r, const LoadInfo&)
                       {
                           REQUIRE(r.ok());
                           held = r.value();
                           core::audio::retain(held);
                       });
    f.workers.drain();
    std::string error;
    f.pool.requestFile(b, false,
                       [&](core::Result<SamplePool::Handle> r, const LoadInfo&)
                       {
                           if (! r)
                               error = r.error().message;
                       });
    f.workers.drain();
    CHECK(error.find("memory") != std::string::npos);
    CHECK(f.pool.residentBytes() <= f.pool.limitBytes());

    core::audio::release(held); // a is unreferenced now: b may evict it
    bool ok = false;
    f.pool.requestFile(b, false, [&](core::Result<SamplePool::Handle> r, const LoadInfo&) { ok = r.ok(); });
    f.workers.drain();
    CHECK(ok);
    CHECK(f.pool.find(a) == nullptr);
    CHECK(f.pool.bufferCount() == 1);
    CHECK(f.pool.collect() == 1); // cache budget 0: b (unreferenced) goes too
    CHECK(f.pool.residentBytes() == 0);
}

TEST_CASE("lib.sync: changes install, replace and clear slots; a missing file reports an error", "[library][sync]")
{
    Fixture f;
    libtest::writeSample(f.samples().getChildFile("a.wav"), 1000, 100);
    libtest::writeSample(f.samples().getChildFile("b.wav"), 1000, 300);
    SwapEngine engine;
    TrackSampleSync sync(f.pool, engine);
    int errors = 0;
    sync.onError = [&](int, int, const core::Error&) { ++errors; };
    core::model::SampleRef a{"sha256:a", "$Lib/Samples/a.wav", "a", false, {}};
    core::model::SampleRef b{"sha256:b", "$Lib/Samples/b.wav", "b", false, {}};
    core::model::SampleRef gone{"sha256:c", "$Lib/Samples/gone.wav", "gone", false, {}};
    auto project = projectWith({{3, a}});
    sync.onChange(trackChange(0, 3), *project);
    CHECK(sync.loading(0, 3));
    f.workers.drain();
    const auto* ba = sync.current(0, 3);
    REQUIRE(ba != nullptr);

    // Two quick changes: only the last one is installed.
    project->sampleRefs.entries.push_back(b);
    project->scenes[0].tracks[3].playback.sample = {b.id};
    sync.onChange(trackChange(0, 3), *project);
    project->scenes[0].tracks[3].playback.sample = {a.id};
    sync.onChange(trackChange(0, 3), *project);
    f.workers.drain();
    CHECK(sync.current(0, 3) == ba);

    project->scenes[0].tracks[3].playback.sample = {};
    sync.onChange(trackChange(0, 3), *project);
    CHECK(sync.current(0, 3) == nullptr);
    CHECK(engine.track(0, 3) == nullptr);
    CHECK(ba->refs.load() == 0);

    project->sampleRefs.entries.push_back(gone);
    project->scenes[0].tracks[4].playback.sample = {gone.id};
    sync.onChange(trackChange(0, 4), *project);
    f.workers.drain();
    CHECK(errors == 1);
    CHECK(sync.current(0, 4) == nullptr);

    // A refused post gives the engine's reference back.
    engine.accept = false;
    project->scenes[0].tracks[5].playback.sample = {b.id};
    sync.onChange(trackChange(0, 5), *project);
    f.workers.drain();
    REQUIRE(sync.current(0, 5) != nullptr);
    CHECK(sync.current(0, 5)->refs.load() == 1);

    int s = 0, t = 0;
    CHECK(parseSamplePath("scene[15].track[0].playback.sample", s, t));
    CHECK((s == 15 && t == 0));
    CHECK_FALSE(parseSamplePath("scene[1].track[2].playback.samples", s, t));
    CHECK_FALSE(parseSamplePath("scene[16].track[2].playback.sample", s, t));
    CHECK_FALSE(parseSamplePath("scene[1].track[2].volume", s, t));
}
