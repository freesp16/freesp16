// Resume with a crash guard (PRJ-020…023; research docs/storage.md §7): the last saved/loaded project and selection
// reopen; a planted guard quarantines the resume copy into System/crashed/; a project that stopped the app twice
// after resuming is not reopened; resume is a preference; a newer autosave is offered and restored as an undoable
// edit. Each Env on a shared disk is one "boot"; destroying an Env without shutdown() is a crash.
#include "ProjectTestSupport.h"

#include <catch2/catch_test_macros.hpp>

using namespace projtest;
using services::store::SaveKind;

namespace
{
std::shared_ptr<libtest::TempDir> disk(const char* name) { return std::make_shared<libtest::TempDir>(name); }
} // namespace

TEST_CASE("project.resume: the last project and its selection reopen", "[project]")
{
    auto d = disk("resume");
    juce::File bundle;
    {
        Env env(d);
        CHECK_FALSE(env.startup().resumed); // nothing to resume yet: a new project
        CHECK(env.store().name() == st::kUntitledName);
        REQUIRE(env.store().create("Song").ok());
        setTempo(env.bus, 128.0);
        env.selection = {3, 5, 7, "SEQUENCE"};
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        bundle = env.store().file();
        CHECK(env.system().getChildFile("resume.json").existsAsFile());
        CHECK(env.system().getChildFile("resume.sixteen/project.json").existsAsFile());
        env.selection = {4, 6, 8, "MIXER"};
        env.session->finishSession(); // keeps the selection at quit
    }
    Env env(d);
    const auto result = env.startup();
    CHECK(result.resumed);
    CHECK(result.message.isEmpty());
    CHECK(result.selection == st::Selection {4, 6, 8, "MIXER"});
    CHECK(env.store().file() == bundle);
    CHECK(env.store().hasFile());
    CHECK(env.project().settings.tempo == 128.0);
    CHECK_FALSE(env.store().dirty());
    CHECK_FALSE(env.system().getChildFile("resuming").exists()); // the guard is removed after the load
    CHECK(env.system().getChildFile("resumed").exists());        // the early-exit watch runs
    env.session->frame(0);
    env.session->frame(st::Resume::kSettleMs + 1);
    env.workers.drain();
    CHECK_FALSE(env.system().getChildFile("resumed").exists()); // settled
}

TEST_CASE("project.resume: a planted guard moves the resume copy to System/crashed/", "[project]")
{
    auto d = disk("guard");
    std::map<std::string, std::string> copy;
    {
        Env env(d);
        env.startup();
        REQUIRE(env.store().create("Crashy").ok());
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        copy = tree(env.system().getChildFile("resume.sixteen"));
        REQUIRE_FALSE(copy.empty());
        REQUIRE(env.system().getChildFile("resuming").createDirectory()); // a boot load that never finished
    }
    Env env(d);
    const auto result = env.startup();
    CHECK_FALSE(result.resumed);
    CHECK(result.message.contains("System/crashed"));
    CHECK(env.store().name() == st::kUntitledName); // a new project
    CHECK_FALSE(env.system().getChildFile("resuming").exists());
    CHECK_FALSE(env.system().getChildFile("resume.json").exists());
    CHECK_FALSE(env.system().getChildFile("resume.sixteen").exists());
    CHECK(env.system().getChildFile("crashed/resume.json").existsAsFile());
    CHECK(tree(env.system().getChildFile("crashed/resume.sixteen")) == copy);
    {
        Env again(d); // the next boot does not resume the quarantined project either
        CHECK_FALSE(again.startup().resumed);
    }
    CHECK(env.open(env.projects().getChildFile("Crashy.sixteen")).ok()); // the project itself is untouched
}

TEST_CASE("project.resume: a project that stopped the app twice after resuming is not reopened", "[project]")
{
    auto d = disk("early");
    {
        Env env(d);
        env.startup();
        REQUIRE(env.store().create("Boom").ok());
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        env.session->finishSession();
    }
    {
        Env boot1(d);
        CHECK(boot1.startup().resumed); // … and "crashes" (no shutdown): the watch marker stays
    }
    {
        Env boot2(d);
        CHECK(boot2.startup().resumed); // one early exit counted
    }
    Env boot3(d);
    const auto result = boot3.startup();
    CHECK_FALSE(result.resumed); // two: not reopened
    CHECK(result.message.contains("twice"));
    CHECK(boot3.system().getChildFile("crashed/resume.json").existsAsFile());

    // A session that settles resets the count.
    auto d2 = disk("early2");
    {
        Env env(d2);
        env.startup();
        REQUIRE(env.store().create("Fine").ok());
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        env.session->finishSession();
    }
    for (int i = 0; i < 4; ++i)
    {
        Env boot(d2);
        CHECK(boot.startup().resumed);
        boot.session->frame(0);
        boot.session->frame(st::Resume::kSettleMs);
        boot.workers.drain(); // ran a minute, then "crashed": not an early exit
    }
}

TEST_CASE("project.resume: resume off starts a new project; a missing project reopens from its copy", "[project]")
{
    auto d = disk("fallback");
    juce::File bundle;
    {
        Env env(d);
        env.startup();
        REQUIRE(env.store().create("Gone").ok());
        setTempo(env.bus, 111.0);
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        bundle = env.store().file();
        env.session->finishSession();
    }
    {
        Env off(d, st::SessionConfig {.resumeEnabled = false}); // PRJ-022
        CHECK_FALSE(off.startup().resumed);
        CHECK(off.store().name() == st::kUntitledName);
        off.session->finishSession();
    }
    REQUIRE(bundle.deleteRecursively()); // removed behind the app's back
    Env env(d);
    const auto result = env.startup();
    CHECK(result.resumed);
    CHECK(result.message.contains("resume copy"));
    CHECK(env.project().settings.tempo == 111.0);
    CHECK(env.store().file() == bundle); // saved back where it was
    REQUIRE(env.save(SaveKind::Save, {}).ok());
    CHECK(bundle.getChildFile("project.json").existsAsFile());
}

TEST_CASE("project.resume: a damaged last project is set aside and a new one starts", "[project]")
{
    auto d = disk("damaged");
    juce::File bundle;
    {
        Env env(d);
        env.startup();
        REQUIRE(env.store().create("Broken").ok());
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        bundle = env.store().file();
        env.session->finishSession();
    }
    REQUIRE(bundle.getChildFile("project.json").replaceWithText("{ not json"));
    Env env(d);
    const auto result = env.startup();
    CHECK_FALSE(result.resumed);
    CHECK(result.message.contains("could not be opened"));
    CHECK(env.store().name() == st::kUntitledName);
    CHECK(env.system().getChildFile("crashed/resume.json").existsAsFile());
}

TEST_CASE("project.resume: a newer autosave is offered and restored as an undoable edit", "[project]")
{
    auto d = disk("restore");
    juce::File autosaveFile;
    {
        Env env(d);
        env.startup();
        REQUIRE(env.store().create("Draft").ok());
        setTempo(env.bus, 100.0);
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        juce::Thread::sleep(1100); // file times have 1 s resolution on some file systems
        setTempo(env.bus, 150.0);
        REQUIRE(env.session->autosave().writeNow());
        env.workers.drain();
        autosaveFile = env.session->autosave().file();
        REQUIRE(autosaveFile.existsAsFile());
    } // crash with unsaved changes
    int asked = 0;
    {
        Env env(d);
        env.session->askRestore = [&](const juce::File& f, std::function<void(bool)> answer)
        {
            ++asked;
            CHECK(f == autosaveFile);
            answer(true);
        };
        const auto result = env.startup();
        CHECK(result.resumed);
        CHECK(asked == 1);
        CHECK(env.project().settings.tempo == 150.0);
        CHECK(env.store().dirty()); // restored changes are unsaved
        CHECK(st::openProject(env.store().file()).value()->settings.tempo == 100.0); // the user's file is untouched
        REQUIRE(env.bus.undo());
        CHECK(env.project().settings.tempo == 100.0);
        CHECK_FALSE(env.store().dirty());
        env.session->finishSession();
    }
    {
        Env env(d); // declined: the copy is set aside and not offered again
        env.session->askRestore = [&](const juce::File&, std::function<void(bool)> answer)
        {
            ++asked;
            answer(false);
        };
        env.startup();
        CHECK(asked == 2);
        CHECK(env.project().settings.tempo == 100.0);
        CHECK_FALSE(autosaveFile.existsAsFile());
        CHECK(autosaveFile.getSiblingFile("Draft.discarded.json").existsAsFile());
        env.session->finishSession();
    }
    Env env(d);
    env.session->askRestore = [&](const juce::File&, std::function<void(bool)>) { ++asked; };
    env.startup();
    CHECK(asked == 2);
}

TEST_CASE("project.resume: after the power key the last project reopens with its unsaved changes, unasked", "[project]")
{
    auto d = disk("poweroff");
    {
        Env env(d);
        env.startup();
        REQUIRE(env.store().create("Live").ok());
        setTempo(env.bus, 100.0);
        REQUIRE(env.save(SaveKind::Save, {}).ok());
        setTempo(env.bus, 140.0);  // unsaved, younger than the autosave interval
        env.selection = {2, 0, 5, "SEQUENCE"};
        REQUIRE(env.session->saveForPowerOff().ok());
        CHECK(env.session->powerOffMarker().existsAsFile());
        const auto savedProject = env.store().file().getChildFile("meta.json");
        const auto recoveryCopy = env.session->autosave().file();
        REQUIRE(recoveryCopy.setLastModificationTime(savedProject.getLastModificationTime()));
    }
    int asked = 0;
    {
        Env env(d);
        env.session->askRestore = [&](const juce::File&, std::function<void(bool)> answer)
        {
            ++asked;
            answer(false);
        };
        const auto result = env.startup();
        CHECK(result.resumed);
        CHECK(result.message.isEmpty());
        CHECK(asked == 0); // no question: back where the unit was switched off
        CHECK(result.selection == st::Selection {2, 0, 5, "SEQUENCE"});
        CHECK(env.project().settings.tempo == 140.0);
        CHECK(env.store().dirty()); // still unsaved: SAVE writes it, UNDO returns to the saved version
        CHECK(st::openProject(env.store().file()).value()->settings.tempo == 100.0); // the user's file is untouched
        CHECK_FALSE(env.session->powerOffMarker().exists()); // used once
        const auto later =
            env.store().file().getChildFile("meta.json").getLastModificationTime() + juce::RelativeTime::seconds(4);
        REQUIRE(env.session->autosave().file().setLastModificationTime(later));
    } // a crash this time (no shutdown): the next start asks again
    Env env(d);
    env.session->askRestore = [&](const juce::File&, std::function<void(bool)> answer)
    {
        ++asked;
        answer(true);
    };
    env.startup();
    CHECK(asked == 1);
    CHECK(env.project().settings.tempo == 140.0);
}

TEST_CASE("project.resume: an untitled project switched off with the power key comes back too", "[project]")
{
    auto d = disk("poweroff-untitled");
    {
        Env env(d);
        env.startup();
        setTempo(env.bus, 133.0); // never saved
        REQUIRE(env.session->saveForPowerOff().ok());
    }
    int asked = 0;
    Env env(d);
    env.session->askRestore = [&](const juce::File&, std::function<void(bool)>) { ++asked; };
    env.startup();
    CHECK(asked == 0);
    CHECK(env.store().name() == st::kUntitledName);
    CHECK(env.project().settings.tempo == 133.0);
}

TEST_CASE("project.power: failed recovery blocks standby until a later write succeeds", "[project]")
{
    const auto files = disk("standby-storage");
    Env running(files);
    running.startup();
    setTempo(running.bus, 157.0);
    const auto folder = running.session->autosave().file().getParentDirectory();
    REQUIRE(folder.replaceWithText("occupy the autosave directory name"));
    REQUIRE_FALSE(running.session->saveForPowerOff().ok());
    CHECK_FALSE(running.session->powerOffMarker().exists());
    CHECK(running.project().settings.tempo == 157.0);
    REQUIRE(folder.deleteFile());
    REQUIRE(running.session->saveForPowerOff().ok());
    CHECK(running.session->autosave().file().existsAsFile());
    CHECK(running.session->powerOffMarker().existsAsFile());
}
