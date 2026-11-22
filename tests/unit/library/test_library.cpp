// Library tree, listing, search and file operations (LIB-001…004, LIB-010, LIB-014…016): listing hides '.' names; a
// move across two temp volumes fsyncs before deleting.
#include "LibTestSupport.h"

#include "services/library/SidecarStore.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace services::library;

namespace
{
std::vector<std::string> names(const std::vector<Entry>& entries)
{
    std::vector<std::string> out;
    for (const auto& e : entries)
        out.push_back(e.name.toStdString());
    return out;
}

void touch(const juce::File& f, const char* text = "x")
{
    f.getParentDirectory().createDirectory();
    REQUIRE(f.replaceWithText(text));
}
} // namespace

TEST_CASE("lib.library: setRoot creates the LIB-001 layout and the $Lib location", "[library][tree]")
{
    libtest::Fixture f;
    for (const char* sub : SampleLibrary::kLayout)
        CHECK(f.library.root().getChildFile(sub).isDirectory());
    REQUIRE(f.library.locations().size() == 1);
    CHECK(f.library.locations()[0].alias == "$Lib");
    CHECK(f.library.freeBytes(f.library.locations()[0]) > 0);

    const auto root = f.library.listNow("", SortOrder::AToZ);
    REQUIRE(root.size() == 1);
    CHECK(root[0].folder);
    CHECK(root[0].aliasPath == "$Lib");
    CHECK(names(f.library.listNow("$Lib", SortOrder::AToZ)) ==
          std::vector<std::string>{"Samples", "RenderedAudio", "Recordings"});
}

TEST_CASE("lib.library: listing hides '.' names and non-audio files, sorts, and reads headers and sidecars",
          "[library][tree]")
{
    libtest::Fixture f;
    const juce::File s = f.samples();
    libtest::writeSample(s.getChildFile("b kick.wav"), 4410, 100, 2, 48000);
    libtest::writeSample(s.getChildFile("A snare.aif"), 100);
    libtest::writeSample(s.getChildFile("c10.wav"), 10);
    libtest::writeSample(s.getChildFile("c9.wav"), 10);
    libtest::writeSample(s.getChildFile(".hidden.wav"), 10);
    touch(s.getChildFile("._b kick.wav"));
    touch(s.getChildFile(".DS_Store"));
    touch(s.getChildFile("notes.txt"));
    touch(s.getChildFile("broken.wav"), "not really audio");
    s.getChildFile("Loops").createDirectory();
    s.getChildFile(".git").createDirectory();
    REQUIRE(
        SidecarStore().write(s.getChildFile("c9.wav"), core::model::SampleMetadata{2.0, 128.0, 0, 0, 0, 0, {}}).ok());

    auto list = f.library.listNow("$Lib/Samples", SortOrder::AToZ);
    CHECK(names(list) ==
          std::vector<std::string>{"Loops", "A snare.aif", "b kick.wav", "broken.wav", "c9.wav", "c10.wav"});
    CHECK(list[0].folder);
    CHECK(list[2].frames == 4410);
    CHECK(list[2].channels == 2);
    CHECK(list[2].rate == 48000);
    CHECK(list[2].aliasPath == "$Lib/Samples/b kick.wav");
    CHECK_FALSE(list[3].readable);
    CHECK(list[4].bpm == 128.0);
    CHECK(list[4].bars == 2.0);
    CHECK_FALSE(list[2].bpm.has_value());

    CHECK(names(f.library.listNow("$Lib/Samples", SortOrder::ZToA)) ==
          std::vector<std::string>{"Loops", "c10.wav", "c9.wav", "broken.wav", "b kick.wav", "A snare.aif"});
    CHECK(f.library.listNow("$Lib/Samples/../..", SortOrder::AToZ).empty()); // never outside a root
    CHECK(f.library.listNow("$Nope", SortOrder::AToZ).empty());

    // The same listing through a worker.
    std::vector<Entry> async;
    f.library.list("$Lib/Samples", SortOrder::AToZ, [&](std::vector<Entry> e) { async = std::move(e); });
    f.workers.drain();
    CHECK(names(async) == names(list));
}

TEST_CASE("lib.library: text search across every location, hidden names skipped", "[library][tree]")
{
    libtest::Fixture f;
    libtest::writeSample(f.samples().getChildFile("Drums/Kick 01.wav"), 10);
    libtest::writeSample(f.samples().getChildFile("Drums/.kick hidden.wav"), 10);
    libtest::writeSample(f.library.root().getChildFile("Recordings/take kick.wav"), 10);
    libtest::writeSample(f.library.root().getChildFile("Projects/kick in a project.wav"), 10); // not browsable
    libtest::writeSample(f.samples().getChildFile("snare.wav"), 10);
    const juce::File ext = f.tmp.dir().getChildFile("Stick");
    libtest::writeSample(ext.getChildFile("deep/er/KICK.aiff"), 10);
    f.library.addLocation({"$Ext:Stick", ext, true, false});

    const auto hits = f.library.searchNow("kick");
    CHECK(names(hits) == std::vector<std::string>{"Kick 01.wav", "KICK.aiff", "take kick.wav"});
    CHECK(hits[1].aliasPath == "$Ext:Stick/deep/er/KICK.aiff");
    CHECK(hits[1].removable);
    CHECK(f.library.searchNow("  ").empty());
    CHECK(f.library.searchNow("kick", 1).size() == 1);
    std::vector<Entry> async;
    f.library.search("snare", [&](std::vector<Entry> e) { async = std::move(e); });
    f.workers.drain();
    CHECK(names(async) == std::vector<std::string>{"snare.wav"});

    // Root listing shows both locations; alias paths resolve both ways.
    CHECK(names(f.library.listNow("", SortOrder::AToZ)) == std::vector<std::string>{"Library", "Stick"});
    const auto file = f.library.resolve("$Ext:Stick/deep/er/KICK.aiff");
    REQUIRE(file);
    CHECK(file->existsAsFile());
    CHECK(f.library.aliasPathOf(*file) == juce::String("$Ext:Stick/deep/er/KICK.aiff"));
    CHECK_FALSE(f.library.resolve("$Lib/Samples/../../x"));
    CHECK_FALSE(f.library.resolve("relative/x.wav"));
    CHECK_FALSE(f.library.aliasPathOf(juce::File("/somewhere/else.wav")));
    const auto roots = f.library.roots();
    CHECK(roots.library == f.library.root());
    CHECK(roots.externals.at("Stick") == ext);
    // services::store resolves the same strings to the same files (FMT-011).
    CHECK(services::store::resolveLocation(services::store::splitLocation("$Ext:Stick/deep/er/KICK.aiff"), roots) ==
          file);
}

TEST_CASE("lib.library: removable locations from /proc/mounts and the provider poll", "[library][tree]")
{
    const auto parsed = ProcMountsProvider::parse("/dev/root / ext4 rw 0 0\n"
                                                  "/dev/sda1 /media/usb0 vfat rw,relatime 0 0\n"
                                                  "/dev/sdb1 /media/MY\\040STICK exfat rw 0 0\n"
                                                  "proc /proc proc rw 0 0\n"
                                                  "garbage\n");
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].alias == "$Ext:usb0");
    CHECK(parsed[0].removable);
    CHECK(parsed[1].alias == "$Ext:MY STICK");
    CHECK(parsed[1].root.getFullPathName() == "/media/MY STICK");

    libtest::Fixture f;
    const juce::File mount = f.tmp.dir().getChildFile("usbstick");
    mount.getChildFile("TORAIZ/Samples").createDirectory(); // M p.41: the stick's Samples folder is the root
    const juce::File mounts = f.tmp.dir().getChildFile("mounts");
    REQUIRE(mounts.replaceWithText("/dev/sda1 " + mount.getFullPathName() + " vfat rw 0 0\n"));
    f.library.setLocationProvider(std::make_shared<ProcMountsProvider>(mounts));
    int changes = 0;
    f.library.onLocationsChanged = [&] { ++changes; };
    f.library.pollLocations();
    f.workers.drain();
    REQUIRE(f.library.locations().size() == 2);
    CHECK(f.library.findLocation("$Ext:usbstick")->root == mount.getChildFile("TORAIZ/Samples"));
    f.library.pollLocations(); // unchanged: no notification
    f.workers.drain();
    CHECK(changes == 1);
    REQUIRE(mounts.replaceWithText(""));
    f.library.pollLocations(); // unplugged
    f.workers.drain();
    CHECK(changes == 2);
    CHECK(f.library.locations().size() == 1);
}

TEST_CASE("lib.fileops: a move across two volumes fsyncs the copy before deleting the source", "[library][fileops]")
{
    libtest::TempDir a("volA"), b("volB");
    const juce::File src = libtest::writeSample(a / "kick.wav", 1000);
    const auto bytes = src.getSize();
    FileOps ops(a / "trash");
    std::vector<std::string> steps;
    ops.setObserver([&](std::string_view s, const juce::File&) { steps.emplace_back(s); });
    ops.setForceCrossVolume(true);
    auto moved = ops.move(src, b.dir());
    REQUIRE(moved.ok());
    CHECK(moved.value() == b / "kick.wav");
    CHECK(moved.value().getSize() == bytes);
    CHECK_FALSE(src.exists());
    CHECK(steps == std::vector<std::string>{"write", "fsync", "rename", "fsync-dir", "delete-source"});
    CHECK((b.dir().findChildFiles(juce::File::findFiles, false, "*.tmp-*").isEmpty()));

    // Same volume: a rename; folders move too; collisions get " 2".
    steps.clear();
    ops.setForceCrossVolume(false);
    libtest::writeSample(a / "kick.wav", 10);
    auto again = ops.move(a / "kick.wav", b.dir());
    REQUIRE(again.ok());
    CHECK(again.value() == b / "kick 2.wav");
    CHECK(steps.front() == "rename");
    libtest::writeSample(a / "Folder/x.wav", 10);
    ops.setForceCrossVolume(true);
    auto folder = ops.move(a / "Folder", b.dir());
    REQUIRE(folder.ok());
    CHECK((b / "Folder/x.wav").existsAsFile());
    CHECK_FALSE((a / "Folder").exists());
}

TEST_CASE("lib.fileops: copy, rename, trash, new folder and the in-use guard", "[library][fileops]")
{
    libtest::Fixture f;
    const juce::File kick = libtest::writeSample(f.samples().getChildFile("kick.wav"), 100);
    const juce::File drums = f.samples().getChildFile("Drums");
    drums.createDirectory();

    auto wait = [&](auto start)
    {
        std::optional<core::Result<juce::File>> out;
        start([&](core::Result<juce::File> r) { out.emplace(std::move(r)); });
        f.workers.drain();
        REQUIRE(out.has_value());
        return std::move(*out);
    };
    auto copied = wait([&](auto done) { f.library.copy(kick, drums, done); });
    REQUIRE(copied.ok());
    CHECK(copied.value().loadFileAsString() == kick.loadFileAsString());
    CHECK(kick.existsAsFile());

    auto renamed = wait([&](auto done) { f.library.rename(copied.value(), "kick/hard?.wav", false, done); });
    REQUIRE(renamed.ok());
    CHECK(renamed.value().getParentDirectory() == drums); // made FAT-safe, stays in its folder
    CHECK(renamed.value().getFileName() != "kick.wav");

    auto folder = wait([&](auto done) { f.library.newFolder(drums, "New", done); });
    REQUIRE(folder.ok());
    CHECK(folder.value().isDirectory());
    auto second = wait([&](auto done) { f.library.newFolder(drums, "New", done); });
    CHECK(second.value().getFileName() == "New 2");

    f.library.setInUse([&](const juce::File& x) { return x == kick; });
    auto refused = wait([&](auto done) { f.library.trash(kick, false, done); });
    CHECK_FALSE(refused.ok());
    CHECK(refused.error().message.find("used") != std::string::npos);
    CHECK(kick.existsAsFile());
    auto trashed = wait([&](auto done) { f.library.trash(kick, true, done); });
    REQUIRE(trashed.ok());
    CHECK_FALSE(kick.exists());
    CHECK(trashed.value().isAChildOf(f.library.root().getChildFile("System/trash")));
    CHECK(f.library.listNow("$Lib/Samples", SortOrder::AToZ).size() == 1); // Drums only

    // LIB-016: importFiles copies only the audio among the dropped paths.
    libtest::TempDir outside("drop");
    const juce::File dropped = libtest::writeSample(outside / "dropped.wav", 10);
    touch(outside / "readme.txt");
    std::vector<juce::File> imported;
    f.library.importFiles(juce::StringArray{dropped.getFullPathName(), (outside / "readme.txt").getFullPathName()},
                          juce::File(), [&](std::vector<juce::File> v) { imported = std::move(v); });
    f.workers.drain();
    REQUIRE(imported.size() == 1);
    CHECK(imported[0] == f.samples().getChildFile("dropped.wav"));
}
