// Project lifecycle (PRJ-002…006, 009…014, 050/051, 060/061): names, new / save / save as / save to /
// open with dirty tracking, rotating backups, failures that keep the old project, list / delete / rename / duplicate.
#include "ProjectTestSupport.h"

#include "services/library/SidecarStore.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "services/store/CodecRegistry.h"

#include "services/store/Migrations.h"

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

using namespace projtest;
using services::store::SaveKind;

TEST_CASE("project.names: validateProjectName follows PRJ-004", "[project]")
{
    using st::validateProjectName;
    CHECK_FALSE(validateProjectName(juce::String("Beat 1")));
    CHECK_FALSE(validateProjectName(juce::String::fromUTF8("Größe 日本")));
    CHECK_FALSE(validateProjectName(juce::String::repeatedString("x", 64)));
    CHECK(validateProjectName(juce::String::repeatedString("x", 65)));
    CHECK(validateProjectName(juce::String()));
    CHECK(validateProjectName(juce::String(".hidden")));
    for (const char* bad : {"a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b"})
        CHECK(validateProjectName(juce::String(bad)));
    CHECK(validateProjectName(juce::String("tab\tname")));
    CHECK(validateProjectName(juce::String("trailing ")));
    CHECK(validateProjectName(juce::String("trailing.")));
    CHECK_FALSE(validateProjectName(juce::String("in.the.middle")));
    // 64 code points of a multi-byte character are still valid.
    CHECK_FALSE(validateProjectName(juce::String::repeatedString(juce::String::fromUTF8("日"), 64)));
}

TEST_CASE("project.lifecycle: create, save, edit, save, open round trip with dirty tracking", "[project]")
{
    Env env("lifecycle");
    REQUIRE(env.store().create("Beat").ok());
    CHECK(env.replaced == 1);
    CHECK_FALSE(env.store().dirty());
    CHECK_FALSE(env.store().hasFile());
    CHECK(env.project().settings.tempo == 120.0); // PRJ-002
    CHECK(env.store().create("Beat").ok()); // not on disk yet: the name is free
    setTempo(env.bus, 99.0);
    CHECK(env.store().dirty());
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    CHECK_FALSE(env.store().dirty());
    const auto bundle = env.projects().getChildFile("Beat.sixteen");
    CHECK(bundle.getChildFile("project.json").existsAsFile());
    CHECK(env.store().file() == bundle);
    CHECK(env.store().hasFile());
    CHECK(env.store().create("Beat").error().message.find("already exists") != std::string::npos);

    setTempo(env.bus, 131.0);
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    const auto saved = std::make_unique<m::Project>(env.project());
    setTempo(env.bus, 80.0); // unsaved
    REQUIRE(env.open(bundle).ok());
    CHECK(env.project() == *saved);
    CHECK_FALSE(env.store().dirty());
    CHECK(env.replaced == 3);
}

TEST_CASE("project.lifecycle: a load posts Stop(1) before anything else", "[project]")
{
    fakes::FakeEngineBridge bridge;
    services::library::Workers workers {1};
    core::CommandBus bus;
    libtest::TempDir tmp("stop");
    st::ProjectStore store(bus, workers, {.projectsDir = tmp.dir().getChildFile("Projects")});
    store.setEngine(&bridge);
    store.createUntitled();
    REQUIRE(bridge.posted.size() == 1);
    const auto* cmd = std::get_if<engine::TransportCmd>(&bridge.posted[0]);
    REQUIRE(cmd != nullptr);
    CHECK(cmd->kind == engine::TransportCmd::Stop);
    CHECK(cmd->value == 1.0);
}

TEST_CASE("project.lifecycle: edits made while the worker writes keep the project dirty", "[project]")
{
    Env env("dirty");
    REQUIRE(env.store().create("Live").ok());
    setTempo(env.bus, 100.0);
    std::optional<core::Result<void>> r;
    st::Progress progress;
    env.store().save(SaveKind::Save, {}, {}, progress, [&](core::Result<void> x) { r = std::move(x); });
    CHECK(env.store().busy());
    setTempo(env.bus, 101.0); // after the snapshot
    env.workers.drain();
    REQUIRE(r.has_value());
    REQUIRE(r->ok());
    CHECK(env.store().dirty());
    // The file holds the snapshot, not the later edit.
    const auto back = st::openProject(env.store().file());
    REQUIRE(back.ok());
    CHECK(back.value()->settings.tempo == 100.0);
    // A second save while one runs is refused.
    std::optional<core::Result<void>> a, b;
    env.store().save(SaveKind::Save, {}, {}, progress, [&](core::Result<void> x) { a = std::move(x); });
    env.store().save(SaveKind::Save, {}, {}, progress, [&](core::Result<void> x) { b = std::move(x); });
    REQUIRE(b.has_value());
    CHECK_FALSE(b->ok());
    env.workers.drain();
    CHECK(a->ok());
    CHECK_FALSE(env.store().dirty());
}

TEST_CASE("project.lifecycle: save as and save to", "[project]")
{
    Env env("saveas");
    REQUIRE(env.store().create("One").ok());
    const auto kick = env.sample("kick.wav", 100);
    // A bundle sample ($Bundle) must survive Save As (PRJ-014: carried over, never moved).
    env.project().sampleRefs.entries.push_back(kick);
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    const auto one = env.projects().getChildFile("One.sixteen");
    one.getChildFile("samples").createDirectory();
    REQUIRE(one.getChildFile("samples/snare.wav").replaceWithText("not really audio"));

    REQUIRE(env.save(SaveKind::SaveAs, "Two").ok());
    const auto two = env.projects().getChildFile("Two.sixteen");
    CHECK(env.store().file() == two);
    CHECK(two.getChildFile("samples/snare.wav").loadFileAsString() == "not really audio");
    CHECK(one.getChildFile("samples/snare.wav").existsAsFile()); // copied/linked, not moved
    CHECK(env.save(SaveKind::SaveAs, "One").error().message.find("already exists") != std::string::npos);
    CHECK(env.save(SaveKind::SaveAs, "bad/name").error().message.find("cannot contain") != std::string::npos);

    // Save to… (PRJ-060): a copy on the volume; the open project stays where it is, listed as external.
    const auto usb = env.tmp->dir().getChildFile("USB");
    usb.createDirectory();
    setTempo(env.bus, 140.0);
    REQUIRE(env.save(SaveKind::SaveTo, "Two", usb).ok());
    CHECK(env.store().file() == two);
    CHECK(env.store().dirty()); // the open project's own file is not saved by a copy
    CHECK(usb.getChildFile("Two.sixteen/samples/snare.wav").existsAsFile());
    st::StoreConfig cfg; // a store that lists the volume
    cfg.projectsDir = env.projects();
    cfg.externalFolders = [usb] { return std::vector<juce::File> {usb}; };
    services::library::Workers w {1};
    core::CommandBus b2;
    st::ProjectStore lister(b2, w, cfg);
    const auto all = lister.list(st::SortBy::Name);
    REQUIRE(all.size() == 3);
    int external = 0;
    for (const auto& i : all)
        external += i.external ? 1 : 0;
    CHECK(external == 1);
    CHECK(env.save(SaveKind::SaveTo, "X", env.tmp->dir().getChildFile("gone")).error().message.find("not available") !=
          std::string::npos);
}

TEST_CASE("project.backups: the last 10 versions rotate in Backups/<name>/ and restore", "[project]")
{
    Env env("backups");
    REQUIRE(env.store().create("Rot").ok());
    for (int i = 0; i < 13; ++i)
    {
        setTempo(env.bus, 100.0 + i);
        REQUIRE(env.save(SaveKind::Save, {}).ok());
    }
    const auto folder = env.data().getChildFile("Backups/Rot");
    const auto backups = st::listBackups(folder);
    REQUIRE(backups.size() == 10);
    // Oldest kept = version 2 (tempo 102: versions 100…111 were backed up, 100 and 101 rotated out).
    auto oldest = st::openProject(backups.front());
    REQUIRE(oldest.ok());
    CHECK(oldest.value()->settings.tempo == 102.0);
    auto newest = st::openProject(backups.back());
    REQUIRE(newest.ok());
    CHECK(newest.value()->settings.tempo == 111.0);

    const st::ProjectInfo current = *st::readProjectInfo(env.store().file(), false);
    const auto listed = env.store().backups(current);
    REQUIRE(listed.size() == 10);
    CHECK(listed.front().path == backups.back()); // newest first

    // Restore is refused for the open project; from another project it works and backs up the current version.
    CHECK_FALSE(env.store().restoreBackup(listed.back(), current).ok());
    REQUIRE(env.store().create("Other").ok());
    REQUIRE(env.store().restoreBackup(listed.back(), current).ok());
    auto restored = st::openProject(current.path);
    REQUIRE(restored.ok());
    CHECK(restored.value()->settings.tempo == 102.0);
    CHECK(st::listBackups(folder).size() == 11); // the version before the restore was kept (rotates on next save)
}

TEST_CASE("project.atomic: a failed rename keeps the old project and reports the path and the OS error", "[project]")
{
    using Step = st::AtomicWriter::Step;
    using Fault = st::AtomicWriter::Fault;
    for (const Step failing : {Step::Seal, Step::MoveOld, Step::Install})
    {
        Env env("atomic");
        REQUIRE(env.store().create("Safe").ok());
        setTempo(env.bus, 123.0);
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        const auto bundle = env.store().file();
        const auto before = tree(bundle);
        setTempo(env.bus, 77.0);
        env.store().setWriteFault([failing](Step s) { return s == failing ? Fault::Fail : Fault::None; });
        const auto r = env.save(SaveKind::Save, {});
        REQUIRE_FALSE(r.ok());
        INFO(r.error().message);
        CHECK(r.error().message.find(env.projects().getFullPathName().toStdString()) != std::string::npos);
        CHECK(r.error().message.find("(") != std::string::npos);
        CHECK_FALSE(r.error().detail.empty()); // the OS error text (strerror)
        CHECK(tree(bundle) == before);         // PRJ-011: the old project is intact
        CHECK(env.store().dirty());
        // No temp / sealed / backup leftovers next to it.
        for (const auto& e : juce::RangedDirectoryIterator(env.projects(), false, "*", juce::File::findFilesAndDirectories))
            CHECK(e.getFile() == bundle);
        auto back = st::openProject(bundle);
        REQUIRE(back.ok());
        CHECK(back.value()->settings.tempo == 123.0);
        env.store().setWriteFault({});
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        CHECK(st::openProject(bundle).value()->settings.tempo == 77.0);
    }
}

TEST_CASE("project.atomic: a volume removed during Save to aborts cleanly and is reported", "[project]")
{
    Env env("volume");
    REQUIRE(env.store().create("Gig").ok());
    const auto usb = env.tmp->dir().getChildFile("USB");
    usb.createDirectory();
    env.store().setWriteFault(
        [usb](st::AtomicWriter::Step s)
        {
            if (s == st::AtomicWriter::Step::Seal)
                usb.deleteRecursively(); // the stick is pulled while the files are written
            return st::AtomicWriter::Fault::None;
        });
    const auto r = env.save(SaveKind::SaveTo, "Gig", usb);
    REQUIRE_FALSE(r.ok());
    INFO(r.error().message);
    CHECK(r.error().message.find("removed") != std::string::npos);
    CHECK_FALSE(env.store().busy());
}

TEST_CASE("project.atomic: sidecars keep the old version when the write fails (PRJ-013)", "[project]")
{
    libtest::TempDir tmp("sidecar");
    const auto audio = libtest::writeSample(tmp.dir().getChildFile("loop.wav"));
    services::library::SidecarStore sidecars;
    m::SampleMetadata meta;
    meta.bpm = 120;
    meta.bars = 2;
    REQUIRE(sidecars.write(audio, meta).ok());
    const auto json = services::library::SidecarStore::jsonFor(audio);
    const auto before = stamp(json);
    REQUIRE(::chmod(tmp.dir().getFullPathName().toRawUTF8(), 0555) == 0); // the folder refuses the temp file
    meta.bpm = 90;
    const auto r = sidecars.write(audio, meta);
    ::chmod(tmp.dir().getFullPathName().toRawUTF8(), 0755);
    if (r.ok()) // running as root: permissions do not apply
        SKIP("folder permissions are not enforced for this user");
    CHECK(r.error().message.find(tmp.dir().getFullPathName().toStdString()) != std::string::npos);
    CHECK(stamp(json) == before);
    for (const auto& e : juce::RangedDirectoryIterator(tmp.dir(), false, "*.tmp-*", juce::File::findFiles))
        FAIL("temp file left: " << e.getFile().getFullPathName());
}

TEST_CASE("project.browse: list, delete to trash, rename and duplicate", "[project]")
{
    Env env("browse");
    for (const char* name : {"b", "A", "c"})
    {
        REQUIRE(env.store().create(name).ok());
        if (std::string(name) == "c") // content in scene 3
            env.project().scenes[2].patterns[0].length = 32;
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        juce::Thread::sleep(5);
    }
    auto byName = env.store().list(st::SortBy::Name);
    REQUIRE(byName.size() == 3);
    CHECK(byName[0].name == "A");
    CHECK(byName[2].name == "c");
    CHECK(byName[2].scenesWithContent == 1);
    CHECK(byName[0].scenesWithContent == 0);
    auto byDate = env.store().list(st::SortBy::Modified);
    CHECK(byDate[0].name == "c"); // newest first

    // The open project cannot be deleted; another goes to the trash folder (host tests: never the real OS trash).
    CHECK_FALSE(env.store().remove(byDate[0]).ok());
    REQUIRE(env.store().remove(byName[0]).ok());
    CHECK_FALSE(byName[0].path.exists());
    CHECK(env.system().getChildFile("trash").getNumberOfChildFiles(juce::File::findDirectories) == 1);

    // Rename the open project: the store follows, backups follow.
    const auto open = *st::readProjectInfo(env.store().file(), false);
    REQUIRE(env.store().rename(open, "Renamed").ok());
    CHECK(env.store().name() == "Renamed");
    CHECK(env.store().file() == env.projects().getChildFile("Renamed.sixteen"));
    CHECK_FALSE(env.store().rename(*st::readProjectInfo(env.store().file(), false), "b").ok()); // exists
    CHECK_FALSE(env.store().rename(*st::readProjectInfo(env.store().file(), false), "x:y").ok());

    const auto copy = env.store().duplicate(*st::readProjectInfo(env.store().file(), false));
    REQUIRE(copy.ok());
    CHECK(copy.value().name == "Renamed COPY");
    CHECK(env.store().duplicate(*st::readProjectInfo(env.store().file(), false)).value().name == "Renamed COPY 2");
    auto a = st::openProject(copy.value().path);
    REQUIRE(a.ok());
    CHECK(*a.value() == env.project());
}

TEST_CASE("project.load: cancelling keeps the open project", "[project]")
{
    Env env("cancel");
    REQUIRE(env.store().create("Keep").ok());
    setTempo(env.bus, 90.0);
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    auto other = storetest::randomProject(7, 0.05);
    REQUIRE(st::saveProject(*other, env.projects().getChildFile("Other.sixteen")).ok());
    std::optional<core::Result<void>> r;
    st::LoadReport report;
    st::Progress progress;
    env.store().open(env.projects().getChildFile("Other.sixteen"), progress,
                     [&](core::Result<void> x, st::LoadReport rep)
                     {
                         r = std::move(x);
                         report = std::move(rep);
                     });
    progress.cancel = true;
    env.workers.drain();
    REQUIRE(r.has_value());
    CHECK_FALSE(r->ok());
    CHECK(report.cancelled);
    CHECK(env.project().settings.tempo == 90.0);
    CHECK(env.store().name() == "Keep");
    CHECK_FALSE(env.store().busy());
}

TEST_CASE("project.load: a newer format opens read-only and Save refuses it (PRJ-052)", "[project]")
{
    Env env("newer");
    auto p = m::newDefaultProject();
    const auto bundle = env.projects().getChildFile("Future.sixteen");
    env.projects().createDirectory();
    REQUIRE(st::saveProject(*p, bundle).ok());
    auto meta = bundle.getChildFile("meta.json").loadFileAsString();
    meta = meta.replaceFirstOccurrenceOf("\"version\": " + juce::String(services::store::kSchemaVersion), "\"version\": 99");
    REQUIRE(bundle.getChildFile("meta.json").replaceWithText(meta));
    st::LoadReport report;
    REQUIRE(env.open(bundle, &report).ok());
    CHECK(report.readOnly);
    CHECK(env.store().readOnly());
    CHECK_FALSE(env.save(SaveKind::Save, {}).ok());
    CHECK(env.save(SaveKind::SaveAs, "Now").ok());
}
