// The registered codec, `.wav.dat` sidecars, the disk importer (LIB-040) and FMT-045 (sources never modified).
#include "core/model/ModelOps.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "services/interop/sp16/ValueTreeCodec.h"
#include "services/store/CodecRegistry.h"
#include "services/store/SampleRefs.h"
#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace services::interop::sp16;
namespace store = services::store;

namespace
{
// path → (size, modification time, content id) of every file below `dir`, and the folders.
std::map<std::string, std::string> snapshot(const juce::File& dir)
{
    std::map<std::string, std::string> out;
    for (const auto& e : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFilesAndDirectories |
                                                                          juce::File::ignoreHiddenFiles))
    {
        const auto f = e.getFile();
        std::string v = e.isDirectory() ? "dir" : std::to_string(f.getSize());
        if (! e.isDirectory())
        {
            v += " " + std::to_string(f.getLastModificationTime().toMilliseconds());
            auto id = store::contentId(f);
            v += " " + (id ? id.value() : std::string("?"));
        }
        out[f.getRelativePathFrom(dir).toStdString()] = v;
    }
    return out;
}

juce::File copyTree(const juce::File& from, const std::string& name)
{
    const auto to = interop_test::temp().dir().getChildFile(name);
    to.deleteRecursively();
    REQUIRE(from.copyDirectoryTo(to));
    return to;
}
} // namespace

TEST_CASE("prj codec: registered read-only, sniffed by its header, opened through the store API", "[interop]")
{
    const auto* c = store::findCodec("sp16-prj");
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->info().canWrite);
    CHECK(store::codecForExtension(".PRJ") == c);
    CHECK(store::codecForExtension(".rsm") == c);
    INTEROP_FIXTURE(g, "basic");
    CHECK(store::codecFor(g.prj) == c);
    CHECK(store::codecFor(g.diskRoot.getChildFile("System/data.rsm")) == c); // the resume file is a project (§7)
    CHECK_FALSE(c->canRead(g.diskRoot.getChildFile("Samples/kick.wav.dat")));
    CHECK_FALSE(c->canRead(g.diskRoot.getChildFile("Samples/kick.wav")));
    CHECK_FALSE(c->write(*core::model::newDefaultProject(), interop_test::temp().dir().getChildFile("x.prj")));

    store::ImportReport report;
    auto p = store::openProject(g.prj, report);
    REQUIRE(p);
    CHECK(p.value()->scenes[0].name == "BASIC");
    CHECK_FALSE(report.warnings.empty());
    auto rsm = store::openProject(g.diskRoot.getChildFile("System/data.rsm"));
    REQUIRE(rsm);
    CHECK(*rsm.value() == *p.value());
    // The pool entries resolve as they are (absolute paths into the source tree).
    store::SampleRoots roots;
    CHECK(store::resolvePool(p.value()->sampleRefs, roots) == 0);
}

TEST_CASE("prj codec: damaged files are errors with a reason, never a crash", "[interop]")
{
    const auto dir = interop_test::temp().dir().getChildFile("damaged");
    dir.createDirectory();
    const auto f = dir.getChildFile("bad.prj");
    REQUIRE(f.replaceWithText("vltrgzip-not-zlib"));
    store::ImportReport report;
    auto r = importProjectFile(f, {}, report);
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("bad.prj") != std::string::npos);
    // A valid container whose root is not a project (a sidecar tree).
    Node n;
    n.type = "SampleAdjunctData";
    const auto c = writeContainer(n);
    REQUIRE(f.replaceWithData(c.data(), c.size()));
    auto r2 = importProjectFile(f, {}, report);
    REQUIRE_FALSE(r2);
    CHECK(r2.error().message.find("Not a project tree") != std::string::npos);
}

TEST_CASE("prj wavdat: SampleAdjunctData sidecars (FMT-041)", "[interop]")
{
    INTEROP_FIXTURE(g, "full");
    auto w = readWavDat(g.diskRoot.getChildFile("Samples/kick.wav.dat"));
    REQUIRE(w);
    CHECK(w.value().bar == 0.25);
    CHECK(w.value().bpm == 0.0);
    CHECK(w.value().sampleNum == 20671);
    CHECK_FALSE(readWavDat(g.diskRoot.getChildFile("Samples/kick.wav")));
    CHECK_FALSE(readWavDat(g.prj)); // a project is not a sidecar
    Node other;
    other.type = "Other";
    CHECK_FALSE(parseWavDat(encodeTree(other)));
}

TEST_CASE("prj disk: finds projects and the resume file; hidden staging files listed apart", "[interop]")
{
    INTEROP_FIXTURE(g, "full");
    const auto disk = copyTree(g.out, "disk-scan");
    const auto root = disk.getChildFile("PIONEER DJ SAMPLER");
    REQUIRE(root.getChildFile("Projects/Full.prj").copyFileTo(root.getChildFile("Projects/.Old.prj")));
    REQUIRE(root.getChildFile("Projects/._Full.prj").replaceWithText("macOS metadata"));
    Sp16DiskImporter importer;
    auto found = importer.findProjects(disk);
    REQUIRE(found);
    REQUIRE(found.value().size() == 2);
    CHECK(found.value()[0].getFileName() == "Full.prj");
    CHECK(found.value()[1].getFileName() == "data.rsm");
    const auto hidden = importer.findHiddenStaging(disk);
    REQUIRE(hidden.size() == 1);
    CHECK(hidden[0].getFileName() == ".Old.prj");
    CHECK(inferDiskRoot(root.getChildFile("Projects/Full.prj")) == root);
    CHECK(inferDiskRoot(root.getChildFile("System/data.rsm")) == root);
    CHECK_FALSE(importer.findProjects(disk.getChildFile("nope")));
}

TEST_CASE("prj disk: import copies samples with their .wav.dat into the library; sources untouched (FMT-045)",
          "[interop]")
{
    INTEROP_FIXTURE(g, "full");
    const auto disk = copyTree(g.out, "disk-import");
    const auto root = disk.getChildFile("PIONEER DJ SAMPLER");
    const auto before = snapshot(disk);
    const auto library = interop_test::temp().dir().getChildFile("library");
    library.deleteRecursively();
    Sp16DiskImporter importer(library);
    store::ImportReport report;
    auto p = importer.importProject(root.getChildFile("Projects/Full.prj"), root, report);
    REQUIRE(p);
    const auto copied = library.getChildFile("Samples/Imported/Full/Samples/kick.wav");
    CHECK(copied.existsAsFile());
    CHECK(juce::File(copied.getFullPathName() + ".dat").existsAsFile());
    const auto& t0 = p.value()->scenes[0].tracks[0];
    const auto* ref = p.value()->sampleRefs.find(t0.playback.sample);
    REQUIRE(ref != nullptr);
    CHECK(ref->path == "$Lib/Samples/Imported/Full/Samples/kick.wav");
    store::SampleRoots roots;
    roots.library = library;
    auto pool = p.value()->sampleRefs;
    CHECK(store::resolvePool(pool, roots) == 2); // missing.wav and the refused "../.." path
    CHECK_FALSE(pool.find(t0.playback.sample)->missing);
    bool sawCopied = false;
    for (const auto& w : report.warnings)
        sawCopied = sawCopied || w.find("copied=1") != std::string::npos;
    CHECK(sawCopied);

    // Again: the same content is reused, nothing copied twice.
    store::ImportReport again;
    REQUIRE(importer.importProject(root.getChildFile("Projects/Full.prj"), root, again));
    bool reused = false;
    for (const auto& w : again.warnings)
        reused = reused || w.find("copied=0") != std::string::npos;
    CHECK(reused);
    // The codec's open path too.
    store::ImportReport viaCodec;
    REQUIRE(store::openProject(root.getChildFile("System/data.rsm"), viaCodec));

    CHECK(snapshot(disk) == before); // no file changed, added or removed in the source tree
}

// Hardware feedback: a project on the internal disk whose samples are on a stick ("$Ext0/…") found none of them, and
// a missing one kept the raw URL, which never resolved. The URL's volume is looked up first, then the project's own
// disk, then by name below all roots; a missing sample keeps its expected location (in the volume's "$Ext:" form).
TEST_CASE("prj samples: resolved on the URL's volume, by name across volumes; a miss resolves once present",
          "[interop]")
{
    INTEROP_FIXTURE(g, "basic"); // tracks 1, 2: "$Ext0/%Smp/kick.wav", "$Ext0/%Smp/tone.wav"
    const auto base = interop_test::temp().dir().getChildFile("two-volumes");
    base.deleteRecursively();
    const auto internal = base.getChildFile("emmc/PIONEER DJ SAMPLER");
    const auto stick = base.getChildFile("stick/PIONEER DJ SAMPLER");
    const auto prj = internal.getChildFile("Projects/Basic.prj");
    REQUIRE(prj.getParentDirectory().createDirectory());
    REQUIRE(g.prj.copyFileTo(prj));
    REQUIRE(stick.getChildFile("Samples").createDirectory());
    REQUIRE(g.diskRoot.getChildFile("Samples/kick.wav").copyFileTo(stick.getChildFile("Samples/kick.wav")));

    ImportOptions opts;
    opts.verifyRoundTrip = false;
    opts.volumes = {{"$Int0", internal, "$Ext:INTERNAL/PIONEER DJ SAMPLER"},
                    {"$Ext0", stick, "$Ext:STICK/PIONEER DJ SAMPLER"}};
    const auto sampleOf = [](const core::model::Project& p, int track)
    { return p.sampleRefs.find(p.scenes[0].tracks[static_cast<std::size_t>(track)].playback.sample); };
    {
        store::ImportReport report;
        auto p = importProjectFile(prj, opts, report);
        REQUIRE(p);
        const auto* kick = sampleOf(*p.value(), 0);
        const auto* tone = sampleOf(*p.value(), 1);
        REQUIRE(kick != nullptr);
        REQUIRE(tone != nullptr);
        CHECK_FALSE(kick->missing);
        CHECK(kick->path == "$Ext:STICK/PIONEER DJ SAMPLER/Samples/kick.wav");
        CHECK(tone->missing);
        CHECK(tone->path == "$Ext:STICK/PIONEER DJ SAMPLER/Samples/tone.wav"); // not "$Ext0/%Smp/tone.wav"
        CHECK(tone->displayName == "tone.wav");

        // The stick comes back with the file: the pool resolves the entry without a relink.
        REQUIRE(g.diskRoot.getChildFile("Samples/tone.wav").copyFileTo(stick.getChildFile("Samples/tone.wav")));
        store::SampleRoots roots;
        roots.externals["STICK"] = stick.getParentDirectory();
        auto pool = p.value()->sampleRefs;
        CHECK(store::resolvePool(pool, roots) == 0);
        REQUIRE(stick.getChildFile("Samples/tone.wav").deleteFile());
    }
    {
        // Elsewhere on the stick: found by name (another volume than the project's disk); no table = absolute.
        REQUIRE(stick.getChildFile("Loops").createDirectory());
        REQUIRE(g.diskRoot.getChildFile("Samples/tone.wav").copyFileTo(stick.getChildFile("Loops/tone.wav")));
        ImportOptions plain = opts;
        for (auto& v : plain.volumes)
            v.modelPrefix.clear();
        store::ImportReport report;
        auto p = importProjectFile(prj, plain, report);
        REQUIRE(p);
        const auto* tone = sampleOf(*p.value(), 1);
        REQUIRE(tone != nullptr);
        CHECK_FALSE(tone->missing);
        CHECK(tone->path == stick.getChildFile("Loops/tone.wav").getFullPathName().toStdString());
        bool byName = false;
        for (const auto& w : report.warnings)
            byName = byName || w.find("byName=1") != std::string::npos;
        CHECK(byName);
    }
    {
        // The codec's open path takes the table from setVolumeRoots; without one only the project's disk is searched.
        store::ImportReport none;
        auto p = store::openProject(prj, none);
        REQUIRE(p);
        CHECK(sampleOf(*p.value(), 0)->missing);
        setVolumeRoots(opts.volumes);
        store::ImportReport report;
        auto q = store::openProject(prj, report);
        setVolumeRoots({});
        REQUIRE(q);
        CHECK_FALSE(sampleOf(*q.value(), 0)->missing);
    }
}
