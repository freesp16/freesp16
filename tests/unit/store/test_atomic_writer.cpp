// AtomicWriter (FMT-002, PRJ-010/011/014): fault injection at every commit step (crash, torn FAT rename, failure)
// never loses both the old and the new bundle; failures leave the old bundle and no staging behind; start-up recovery.
#include "tests/unit/store/StoreTestSupport.h"

#include "services/store/AtomicWriter.h"
#include "services/store/CodecRegistry.h"

#include <catch2/catch_test_macros.hpp>

using namespace services::store;
using storetest::ScratchFolder;
using Step = AtomicWriter::Step;
using Fault = AtomicWriter::Fault;
using Mode = AtomicWriter::Mode;

namespace
{
// Writes `p` into `bundle` with `mode` and a fault at `at` (or none).
Result<void> saveWithFault(const core::model::Project& p, const juce::File& bundle, Mode mode, Step at, Fault f)
{
    AtomicWriter w(bundle, mode);
    for (const auto& file : NativeCodec::encode(p))
        if (auto r = w.writeFile(juce::String::fromUTF8(file.name.c_str()), file.text.data(), file.text.size()); ! r)
            return r;
    REQUIRE(w.writeFile("samples/carried.wav", "RIFF", 4).ok());
    w.setFaultHook([=](Step s) { return s == at ? f : Fault::None; });
    return w.commit();
}

std::unique_ptr<core::model::Project> tryOpen(const juce::File& bundle)
{
    auto p = std::unique_ptr<core::model::Project>(new core::model::Project);
    ImportReport report;
    if (! NativeCodec::readInto(bundle, *p, report))
        return nullptr;
    return p;
}

int leftovers(const juce::File& folder, const juce::String& bundleName, bool countOrphans)
{
    int n = 0;
    for (const auto& f : folder.findChildFiles(juce::File::findFilesAndDirectories, false))
    {
        const auto name = f.getFileName();
        if (name == bundleName)
            continue;
        if (name.contains(".orphan-") && ! countOrphans)
            continue;
        ++n;
    }
    return n;
}
} // namespace

TEST_CASE("store: a fault at any commit step never loses both bundles", "[store][atomic]")
{
    const auto oldProject = storetest::randomProject(11);
    const auto newProject = storetest::randomProject(12);
    REQUIRE_FALSE(*oldProject == *newProject);

    int cases = 0;
    for (const auto mode : {Mode::Posix, Mode::Fat})
        for (const bool firstSave : {false, true})
            for (const auto step : {Step::Seal, Step::MoveOld, Step::Install, Step::Clean})
                for (const auto fault : {Fault::Crash, Fault::Torn, Fault::Fail})
                {
                    if (fault == Fault::Torn && mode != Mode::Fat)
                        continue; // a POSIX rename is atomic
                    if (firstSave && (step == Step::MoveOld || step == Step::Clean))
                        continue; // steps that only exist when there is an old bundle
                    if (step == Step::Clean && fault == Fault::Torn)
                        continue; // not a rename
                    ScratchFolder dir("fault");
                    const auto bundle = dir / "Song.sixteen";
                    if (! firstSave)
                        REQUIRE(saveWithFault(*oldProject, bundle, mode, Step::Seal, Fault::None).ok());
                    const auto r = saveWithFault(*newProject, bundle, mode, step, fault);
                    INFO("mode " << (mode == Mode::Fat ? "fat" : "posix") << ", first save " << firstSave << ", step "
                                 << static_cast<int>(step) << ", fault " << static_cast<int>(fault));
                    // a failed clean-up is not a failed save: the new bundle is in place, B.bak is left for recovery
                    CHECK(r.ok() == (step == Step::Clean && fault == Fault::Fail));

                    if (fault == Fault::Fail && step != Step::Clean)
                    {
                        // PRJ-011: an orderly failure undoes itself at once — old intact, no staging left
                        CHECK(r.error().message.find(dir.get().getFullPathName().toStdString()) != std::string::npos);
                        CHECK(leftovers(dir.get(), "Song.sixteen", true) == 0);
                        if (firstSave)
                            CHECK_FALSE(bundle.exists());
                        else
                        {
                            const auto still = tryOpen(bundle);
                            REQUIRE(still != nullptr);
                            CHECK(*still == *oldProject);
                        }
                        ++cases;
                        continue;
                    }

                    // the process "died" here: start-up recovery
                    AtomicWriter::recoverLeftovers(dir.get());
                    const auto opened = tryOpen(bundle);
                    if (firstSave && step == Step::Seal)
                        CHECK(opened == nullptr); // nothing was saved yet, and nothing is invented
                    else
                    {
                        REQUIRE(opened != nullptr);
                        const bool isOld = ! firstSave && *opened == *oldProject;
                        const bool isNew = *opened == *newProject;
                        CHECK((isOld || isNew));
                        // a sealed bundle always wins: from MoveOld on the new save is in place
                        if (step != Step::Seal)
                            CHECK(isNew);
                        CHECK(bundle.getChildFile("samples/carried.wav").existsAsFile());
                    }
                    CHECK(leftovers(dir.get(), "Song.sixteen", false) == 0);
                    if (fault == Fault::Torn)
                        CHECK(leftovers(dir.get(), "Song.sixteen", true) > 0); // orphans are never deleted
                    ++cases;
                }
    CHECK(cases == 29);
}

TEST_CASE("store: Torn renames are reported as needing a disk check", "[store][atomic]")
{
    ScratchFolder dir("torn");
    const auto bundle = dir / "Song.sixteen";
    const auto a = storetest::randomProject(1), b = storetest::randomProject(2);
    REQUIRE(saveWithFault(*a, bundle, Mode::Fat, Step::Seal, Fault::None).ok());
    CHECK_FALSE(saveWithFault(*b, bundle, Mode::Fat, Step::MoveOld, Fault::Torn).ok());
    CHECK(AtomicWriter::journalOf(bundle).existsAsFile());
    const auto recovery = AtomicWriter::recover(bundle);
    CHECK(recovery.needsDiskCheck);
    CHECK_FALSE(AtomicWriter::journalOf(bundle).exists());
    const auto opened = tryOpen(bundle);
    REQUIRE(opened != nullptr);
    CHECK(*opened == *b);
    // the next save works normally
    REQUIRE(saveWithFault(*a, bundle, Mode::Fat, Step::Seal, Fault::None).ok());
    CHECK(*tryOpen(bundle) == *a);
}

TEST_CASE("store: saves keep the bundle's other files and never touch samples outside it", "[store][atomic]")
{
    ScratchFolder dir("carry");
    const auto bundle = dir / "Keep.sixteen";
    auto p = core::model::newDefaultProject();
    REQUIRE(saveProject(*p, bundle).ok());
    bundle.getChildFile("samples").createDirectory();
    REQUIRE(bundle.getChildFile("samples/kick.wav").replaceWithText("kick"));
    REQUIRE(bundle.getChildFile("previews/last.wav").create().wasOk());
    REQUIRE(bundle.getChildFile("patterns/notes.txt").replaceWithText("mine"));
    const auto created = bundle.getChildFile("meta.json").loadFileAsString();

    p->settings.tempo = 99.0;
    REQUIRE(saveProject(*p, bundle).ok());
    CHECK(bundle.getChildFile("samples/kick.wav").loadFileAsString() == "kick");
    CHECK(bundle.getChildFile("previews/last.wav").existsAsFile());
    CHECK(bundle.getChildFile("patterns/notes.txt").loadFileAsString() == "mine");
    CHECK(tryOpen(bundle)->settings.tempo == 99.0);
    // "created" survives the save
    const auto createdLine = [](const juce::String& meta)
    { return meta.fromFirstOccurrenceOf("\"created\"", false, false).upToFirstOccurrenceOf(",", false, false); };
    CHECK(createdLine(bundle.getChildFile("meta.json").loadFileAsString()) == createdLine(created));
    CHECK(leftovers(dir.get(), "Keep.sixteen", true) == 0);
}

TEST_CASE("store: a writer that is not committed leaves nothing behind", "[store][atomic]")
{
    ScratchFolder dir("abandon");
    {
        AtomicWriter w(dir / "Gone.sixteen");
        REQUIRE(w.writeFile("project.json", "{}", 2).ok());
        CHECK(w.stagingFolder().isDirectory());
    }
    CHECK(leftovers(dir.get(), "-", true) == 0);
    AtomicWriter missingParent(dir.get().getChildFile("no/such/folder/X.sixteen"));
    const auto r = missingParent.commit();
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("does not exist") != std::string::npos);
    // a file where the bundle should be is never replaced
    REQUIRE((dir / "File.sixteen").replaceWithText("not a folder"));
    AtomicWriter onFile(dir / "File.sixteen");
    CHECK_FALSE(onFile.commit().ok());
    CHECK((dir / "File.sixteen").loadFileAsString() == "not a folder");
}

TEST_CASE("store: single files are replaced atomically and leftovers are recovered at start-up", "[store][atomic]")
{
    ScratchFolder dir("single");
    const auto target = dir / "kick.wav.json";
    REQUIRE(AtomicWriter::writeFileAtomically(target, "{\"format\":1}", 12).ok());
    REQUIRE(AtomicWriter::writeFileAtomically(target, "{\"format\":2}", 12).ok());
    CHECK(target.loadFileAsString() == "{\"format\":2}");
    CHECK(leftovers(dir.get(), "kick.wav.json", true) == 0);

    // leftovers of a killed single-file write and of a killed bundle save; unrelated names stay
    REQUIRE((dir / "kick.wav.json.tmp-123-0badf00d").replaceWithText("partial"));
    REQUIRE((dir / "user.bak").replaceWithText("the user's own file"));
    (dir / "Song.sixteen.tmp-77-deadbeef").createDirectory();
    AtomicWriter::recoverLeftovers(dir.get());
    CHECK_FALSE((dir / "kick.wav.json.tmp-123-0badf00d").exists());
    CHECK_FALSE((dir / "Song.sixteen.tmp-77-deadbeef").exists());
    CHECK((dir / "user.bak").existsAsFile());
    CHECK(target.loadFileAsString() == "{\"format\":2}");
}

TEST_CASE("store: openProject finishes an interrupted save of that bundle", "[store][atomic]")
{
    ScratchFolder dir("open");
    const auto bundle = dir / "Song.sixteen";
    const auto a = storetest::randomProject(5), b = storetest::randomProject(6);
    REQUIRE(saveWithFault(*a, bundle, Mode::Posix, Step::Seal, Fault::None).ok());
    CHECK_FALSE(saveWithFault(*b, bundle, Mode::Posix, Step::Install, Fault::Crash).ok());
    CHECK_FALSE(bundle.exists()); // between move-old and install
    auto opened = openProject(bundle);
    REQUIRE(opened.ok());
    CHECK(*opened.value() == *b);
    CHECK(leftovers(dir.get(), "Song.sixteen", true) == 0);
}

TEST_CASE("store: the file system type picks the FAT variant", "[store][atomic]")
{
    ScratchFolder dir("mode");
    CHECK(AtomicWriter::detectMode(dir.get()) == Mode::Posix); // the build folder is never FAT
    AtomicWriter w(dir / "X.sixteen");
    CHECK(w.mode() == Mode::Posix);
}
