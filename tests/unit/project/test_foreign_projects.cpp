// OPEN of the reference projects (hardware feedback: OPEN PROJECT listed none of the unit's own projects, so nothing
// could be opened): the store lists the *.prj files of the foreign folders (internal disk, USB sticks) as read-only
// entries and importForeign opens one as a new native project, leaving the source folder untouched.
#include "ProjectTestSupport.h"

#include "tests/unit/interop/Fixtures.h"

#include <catch2/catch_test_macros.hpp>

using namespace projtest;

TEST_CASE("store.foreign: reference projects are listed read-only and open as a new native project", "[project][foreign]")
{
    const auto g = interop_test::fixture("basic");
    if (! g)
        SKIP("python3 is needed to generate the reference project");
    const auto folder = g->prj.getParentDirectory();
    const auto before = projtest::tree(g->diskRoot);

    st::SessionConfig extra;
    extra.foreignFolders = [folder] { return std::vector<st::ForeignFolder> {{folder, "INTERNAL"}}; };
    Env env("foreign", extra);

    const auto list = env.store().list(st::SortBy::Name);
    const st::ProjectInfo* entry = nullptr;
    for (const auto& info : list)
        if (info.foreign)
            entry = &info;
    REQUIRE(entry != nullptr);
    CHECK(entry->path == g->prj);
    CHECK(entry->name == g->prj.getFileNameWithoutExtension());
    CHECK(entry->location == "INTERNAL");
    CHECK(entry->external);

    std::optional<core::Result<void>> r;
    st::Progress progress;
    env.store().importForeign(*entry, progress, [&](core::Result<void> x, st::LoadReport) { r = std::move(x); });
    env.workers.drain();
    REQUIRE(r.has_value());
    CHECK(r->ok());
    CHECK(env.store().name() == g->prj.getFileNameWithoutExtension());
    CHECK(env.replaced >= 1);
    CHECK(env.lastReport.foreign); // the app logs its warnings as "IMPORT …" lines
    CHECK_FALSE(env.lastReport.warnings.empty());

    // A second import of the same file takes the next free name; the reference folder is never written.
    std::optional<core::Result<void>> r2;
    const auto saved = env.save(st::SaveKind::Save, env.store().name());
    INFO((saved.ok() ? std::string() : saved.error().message + " / " + saved.error().detail));
    CHECK(saved.ok());
    env.store().importForeign(*entry, progress, [&](core::Result<void> x, st::LoadReport) { r2 = std::move(x); });
    env.workers.drain();
    REQUIRE(r2.has_value());
    CHECK(r2->ok());
    CHECK(env.store().name() == g->prj.getFileNameWithoutExtension() + " 2");
    CHECK(projtest::tree(g->diskRoot) == before);
}

// Hardware feedback: an import that was never saved was lost at the next start, or reopened under its name with the
// previous project's content (the resume copy of the project open before). The import is written as a native bundle
// before the swap, so the next boot resumes it like any other project.
TEST_CASE("store.foreign: an import is saved at once; the next start resumes it, not the previous project",
          "[project][foreign]")
{
    const auto g = interop_test::fixture("basic");
    if (! g)
        SKIP("python3 is needed to generate the reference project");
    const auto folder = g->prj.getParentDirectory();
    st::SessionConfig extra;
    extra.foreignFolders = [folder] { return std::vector<st::ForeignFolder> {{folder, "INTERNAL"}}; };
    auto d = std::make_shared<libtest::TempDir>("foreign-resume");
    juce::File imported;
    {
        Env env(d, extra);
        env.startup();
        REQUIRE(env.store().create("Previous").ok());
        setTempo(env.bus, 133.0);
        REQUIRE(env.save(st::SaveKind::Save, {}).ok()); // the resume copy now holds "Previous"

        std::optional<st::ProjectInfo> entry;
        for (const auto& info : env.store().list(st::SortBy::Name))
            if (info.foreign)
                entry = info;
        REQUIRE(entry);
        std::optional<core::Result<void>> r;
        st::Progress progress;
        env.store().importForeign(*entry, progress, [&](core::Result<void> x, st::LoadReport) { r = std::move(x); });
        env.workers.drain();
        REQUIRE(r.has_value());
        REQUIRE(r->ok());
        imported = env.store().file();
        CHECK(imported == env.projects().getChildFile(g->prj.getFileNameWithoutExtension() + ".sixteen"));
        CHECK(imported.getChildFile("project.json").existsAsFile()); // written without a SAVE
        CHECK(env.store().hasFile());
        CHECK_FALSE(env.store().dirty());
        // no SAVE, no shutdown: the app is switched off
    }
    {
        Env env(d, extra);
        const auto result = env.startup();
        CHECK(result.resumed);
        CHECK(env.store().file() == imported);
        CHECK(env.project().scenes[0].name == "BASIC");
        CHECK(env.project().settings.tempo != 133.0);
        bool listed = false;
        for (const auto& info : env.store().list(st::SortBy::Name))
            listed = listed || (! info.foreign && info.path == imported);
        CHECK(listed);
        REQUIRE(env.open(env.projects().getChildFile("Previous.sixteen")).ok());
        CHECK(env.project().settings.tempo == 133.0);
        REQUIRE(env.open(imported).ok());
        CHECK(env.project().scenes[0].name == "BASIC");
    }
}

// Backstop: a load recorded for a bundle that does not exist (nothing saved it) drops the resume copy of the project
// before, so the next start says "not found" instead of reopening that copy under the new name.
TEST_CASE("store.resume: markLoaded of a missing bundle never keeps another project's resume copy",
          "[project][foreign]")
{
    auto d = std::make_shared<libtest::TempDir>("resume-stale");
    {
        Env env(d);
        env.startup();
        REQUIRE(env.store().create("Previous").ok());
        REQUIRE(env.save(st::SaveKind::Save, {}).ok());
        REQUIRE(env.system().getChildFile("resume.sixteen/project.json").existsAsFile());
        st::Resume resume({env.system(), env.system().getChildFile("autosave")});
        const auto ghost = env.projects().getChildFile("Ghost.sixteen");
        REQUIRE(resume.markLoaded(ghost, {}).ok());
        CHECK_FALSE(env.system().getChildFile("resume.sixteen").exists());
        // The same project again (resumed from its own copy, the original gone): the copy stays.
        REQUIRE(resume.markLoaded(env.store().file(), {}).ok());
        REQUIRE(env.store().file().deleteRecursively());
        REQUIRE(resume.markLoaded(env.store().file(), {}).ok());
        CHECK(env.system().getChildFile("resume.sixteen/project.json").existsAsFile());
        REQUIRE(resume.markLoaded(ghost, {}).ok());
    }
    Env env(d);
    const auto result = env.startup();
    CHECK_FALSE(result.resumed);
    CHECK(result.message.contains("not found"));
}
