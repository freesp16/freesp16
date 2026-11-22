// Missing media (PRJ-040/041, PRJ-014): a project with 3 missing samples loads, plays its other tracks and lists the
// missing ones; relink finds 2 of them by name + content hash in another folder without touching any file; the last
// one is left empty; every edit is one undo step.
#include "ProjectTestSupport.h"

#include "core/audio/SampleBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace projtest;
using services::store::SaveKind;

namespace
{
std::map<std::string, juce::Time> times(const juce::File& folder)
{
    std::map<std::string, juce::Time> out;
    for (const auto& e : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
        out[e.getFile().getRelativePathFrom(folder).toStdString()] = e.getFile().getLastModificationTime();
    return out;
}
} // namespace

TEST_CASE("project.missing: 3 missing samples load, play silent, are listed, relinked and left empty", "[project]")
{
    Env env("missing");
    env.startup();
    const char* names[] = {"kick.wav", "snare.wav", "hat.wav", "clap.wav", "tom.wav"};
    std::vector<m::SampleRef> refs;
    for (int i = 0; i < 5; ++i)
        refs.push_back(env.sample(names[i], 110.0 * (i + 1)));
    auto p = m::newDefaultProject();
    p->sampleRefs.entries = refs;
    for (int i = 0; i < 5; ++i)
        p->scenes[0].tracks[static_cast<std::size_t>(i)].playback.sample.id = refs[static_cast<std::size_t>(i)].id;
    p->scenes[2].tracks[9].playback.sample.id = refs[0].id; // the same file on two tracks: resident once
    std::vector<std::string> problems;
    REQUIRE(m::validate(*p, problems));
    const auto bundle = env.projects().getChildFile("Media.sixteen");
    REQUIRE(st::saveProject(*p, bundle).ok());

    // Move hat and clap elsewhere (with a decoy hat of other content found first), delete tom.
    const auto moved = env.tmp->dir().getChildFile("Moved");
    REQUIRE(moved.getChildFile("a").createDirectory());
    REQUIRE(env.samples().getChildFile("hat.wav").moveFileTo(moved.getChildFile("a/hat.wav")));
    REQUIRE(env.samples().getChildFile("clap.wav").moveFileTo(moved.getChildFile("a/clap.wav")));
    libtest::writeSample(moved.getChildFile("0/hat.wav"), 2205, 999.0); // same name, other content
    REQUIRE(env.samples().getChildFile("tom.wav").deleteFile());
    const auto movedBefore = tree(moved);
    const auto movedTimes = times(moved);

    st::LoadReport report;
    REQUIRE(env.open(bundle, &report).ok());
    REQUIRE(report.missing.size() == 3); // PRJ-040: listed
    CHECK(report.samples == 2);
    CHECK(report.samplesLoaded == 2);
    auto& mm = env.session->missingMedia();
    CHECK(mm.missing().size() == 3);
    const auto silent = mm.silentTracks();
    CHECK(silent == std::vector<std::pair<int, int>> {{0, 2}, {0, 3}, {0, 4}});
    // PRJ-041: the project plays — the present samples are installed in the engine, the missing tracks are silent.
    env.workers.drain();
    CHECK(env.engine.track(0, 0) != nullptr);
    CHECK(env.engine.track(0, 1) != nullptr);
    CHECK(env.engine.track(2, 9) == env.engine.track(0, 0));
    CHECK(env.engine.track(0, 2) == nullptr);
    CHECK(env.engine.track(0, 4) == nullptr);
    CHECK_FALSE(env.store().dirty());

    std::optional<st::RelinkReport> relinked;
    mm.relink(moved, [&](st::RelinkReport r) { relinked = std::move(r); });
    CHECK(mm.searching());
    env.workers.drain();
    REQUIRE(relinked.has_value());
    CHECK(relinked->relinked == 2);
    REQUIRE(relinked->unmatched.size() == 1);
    CHECK(relinked->unmatched[0].id == refs[4].id);
    CHECK(std::any_of(relinked->notes.begin(), relinked->notes.end(),
                      [](const std::string& n) { return n.find("different content") != std::string::npos; }));
    CHECK(tree(moved) == movedBefore); // PRJ-014: read only
    CHECK(times(moved) == movedTimes);
    CHECK(mm.missing().size() == 1);
    CHECK(env.store().dirty());
    env.workers.drain();
    CHECK(env.engine.track(0, 2) != nullptr); // relinked tracks load (TrackSampleSync)
    CHECK(env.engine.track(0, 3) != nullptr);
    const auto* hat = env.project().sampleRefs.find(m::SampleKey {refs[2].id});
    REQUIRE(hat != nullptr);
    CHECK(hat->path == moved.getChildFile("a/hat.wav").getFullPathName().toStdString()); // outside every root

    // Leave the last one empty; undo brings it back; undo again un-relinks.
    mm.leaveEmpty({refs[4].id});
    CHECK(mm.missing().empty());
    CHECK(env.project().scenes[0].tracks[4].playback.sample.empty());
    CHECK(env.project().sampleRefs.find(m::SampleKey {refs[4].id}) == nullptr);
    REQUIRE(env.bus.undo());
    CHECK(mm.missing().size() == 1);
    CHECK(env.project().scenes[0].tracks[4].playback.sample.id == refs[4].id);
    REQUIRE(env.bus.undo());
    CHECK(mm.missing().size() == 3);
    CHECK_FALSE(env.store().dirty());
    REQUIRE(env.bus.redo());

    // Saved and reopened: relinked paths stick, the missing mark is resolved again (never stored).
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    REQUIRE(env.open(bundle, &report).ok());
    CHECK(report.missing.size() == 1);
}

TEST_CASE("project.missing: a file that comes back is found by refresh; relink under the library uses $Lib", "[project]")
{
    Env env("refresh");
    env.startup();
    auto ref = env.sample("loop.wav", 220.0);
    auto p = m::newDefaultProject();
    p->sampleRefs.entries = {ref};
    p->scenes[0].tracks[0].playback.sample.id = ref.id;
    const auto bundle = env.projects().getChildFile("Back.sixteen");
    REQUIRE(st::saveProject(*p, bundle).ok());
    const auto file = env.samples().getChildFile("loop.wav");
    const auto aside = env.tmp->dir().getChildFile("aside.wav");
    REQUIRE(file.moveFileTo(aside));
    REQUIRE(env.open(bundle).ok());
    auto& mm = env.session->missingMedia();
    CHECK(mm.missing().size() == 1);
    REQUIRE(aside.moveFileTo(file));
    CHECK(mm.refresh() == 0);
    CHECK(mm.missing().empty());
    CHECK_FALSE(env.store().dirty()); // resolved state, not an edit

    // modelPathOf maps library files to $Lib and bundle samples to $Bundle.
    const auto roots = env.store().roots();
    CHECK(st::MissingMedia::modelPathOf(file, roots) == "$Lib/Samples/loop.wav");
    CHECK(st::MissingMedia::modelPathOf(bundle.getChildFile("samples/x.wav"), roots) == "$Bundle/x.wav");
}
