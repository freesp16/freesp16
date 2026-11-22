// PROJECT, OPEN PROJECT, the SAVE AS dialog and RENDER AUDIO against project.svg, project-open.svg, save-as.svg and
// render-audio.svg, and the scripted flows: the name dialog rejects `a/b` and `.x`, SAVE of a saved project asks
// OVERWRITE? then saves with a toast, CREATE NEW warns when dirty, OPEN confirms and loads, DELETE confirms and never
// offers the open project, the table sorts by NAME / MODIFIED.
#include "services/library/Workers.h"
#include "services/store/ProjectStore.h"
#include "tests/snapshot/m2/M2Fixture.h"
#include "ui/screens/project/StoreProjectSource.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/Dialog.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m2test::M2Fixture;
namespace L = ui::layout;
using m2test::FakeProjectSource;
using ui::m2::ProjectEntry;

namespace
{
void demoProjects()
{
    struct P
    {
        const char *name, *date;
        int scenes;
        bool usb;
    };
    static constexpr P projects[] = {
        {"Groove_Sketch_03", "Today 14:12", 4, false}, {"Live_Set_Nov", "Yesterday 22:40", 12, false},
        {"Dub_Techno_Idea", "18 Nov 2026", 3, false},  {"Breaks_120", "12 Nov 2026", 6, true},
        {"Ambient_Pads", "02 Nov 2026", 2, false},     {"Demo_Project_01", "28 Oct 2026", 16, true},
        {"Workshop_Template", "14 Oct 2026", 1, false}};
    auto& src = FakeProjectSource::instance();
    std::int64_t key = 100;
    for (const auto& p : projects)
    {
        ProjectEntry e;
        e.name = p.name;
        e.modified = p.date;
        e.modifiedKey = key--;
        e.scenes = p.scenes;
        e.external = p.usb;
        e.location = p.usb ? "USB" : "";
        src.addEntry(std::move(e));
    }
    src.setCurrent("Groove_Sketch_03", true);
}

void makeDirty(M2Fixture& f)
{
    f.trigger(1, 0);
}

ui::Dialog* dialog(M2Fixture& f)
{
    return dynamic_cast<ui::Dialog*>(f.shell->topPopup());
}
} // namespace

TEST_CASE("ui.m2.project: PROJECT matches project.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    demoProjects();
    makeDirty(f);
    REQUIRE(f.open("project"));
    REQUIRE(f.shell->router().current().bodyComponent == "ProjectList");
    CHECK(f.shell->contextBar().model().back); // slot A = BACK (UID-851)
    f.turn(io::Control::Selector, 3);          // SAVE AS
    const auto ours = f.image();
    CHECK(m2test::hasText(f.dl, "Groove_Sketch_03 \xC2\xB7  edited"));
    const auto& mock = m2test::m2mockup("project");
    CHECK(compare(ours, mock, L::canvas, "m2-project").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 1248, 448}, "m2-project-list").match >= kGate);
}

TEST_CASE("ui.m2.project: OPEN PROJECT matches project-open.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    demoProjects();
    REQUIRE(f.open("project"));
    REQUIRE(f.open("project.open"));
    f.turn(io::Control::Selector, 3); // Breaks_120
    CHECK(ui::projectFocus(f.shell->body()) == 3);
    const auto ours = f.image();
    const auto& mock = m2test::m2mockup("project-open");
    CHECK(compare(ours, mock, L::canvas, "m2-project-open").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 1248, 456}, "m2-project-open-table").match >= kGate);
}

TEST_CASE("ui.m2.project: the SAVE AS dialog with the software keyboard matches save-as.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    demoProjects();
    makeDirty(f);
    REQUIRE(f.open("project"));
    f.turn(io::Control::Selector, 3);
    f.press(io::Control::SelectorPush); // SAVE AS: proposes Groove_Sketch_04
    f.settle();
    auto* d = dialog(f);
    REQUIRE(d != nullptr);
    CHECK(d->field().text() == "Groove_Sketch_04");
    const auto ours = f.image();
    const auto& mock = m2test::m2mockup("save-as");
    CHECK(compare(ours, mock, L::canvas, "m2-save-as").match >= kGate);
    CHECK(compare(ours, mock, L::dialog, "m2-save-as-dialog").match >= kGate);
}

TEST_CASE("ui.m2.project: RENDER AUDIO matches render-audio.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    for (auto& t : f.shell->state().tracks)
        t.hasContent = true;
    REQUIRE(f.open("project"));
    REQUIRE(f.open("project.renderAudio"));
    f.tap(32 + 90, 316 + 68 * 3 + 30);       // TRACK 1
    f.tap(32 + 188 + 90, 316 + 68 * 3 + 30); // TRACK 2 (focused)
    const auto ours = f.image();
    CHECK(m2test::hasText(f.dl, "3 files \xE2\x86\x92 Library / RenderedAudio"));
    const auto& mock = m2test::m2mockup("render-audio");
    CHECK(compare(ours, mock, L::canvas, "m2-render-audio").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 1248, 456}, "m2-render-audio-body").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m2-render-audio-strip").match >= kGate);
}

TEST_CASE("ui.m2.project: the name dialog rejects a/b and .x; the primary button stays disabled", "[m2]")
{
    CHECK(ui::m2::validateProjectName("a/b").has_value());
    CHECK(ui::m2::validateProjectName(".x").has_value());
    CHECK(ui::m2::validateProjectName("").has_value());
    CHECK(ui::m2::validateProjectName(std::string(65, 'a')).has_value());
    CHECK_FALSE(ui::m2::validateProjectName("Groove_Sketch_04").has_value());
    CHECK_FALSE(ui::m2::validateProjectName("\xC3\x9C"
                                            "ber Beat")
                    .has_value());
    CHECK(ui::m2::nextName("Groove_Sketch_03") == "Groove_Sketch_04");
    CHECK(ui::m2::nextName("Beat") == "Beat_2");

    M2Fixture f;
    REQUIRE(f.open("project"));
    f.turn(io::Control::Selector, 3);
    f.press(io::Control::SelectorPush);
    f.settle();
    auto* d = dialog(f);
    REQUIRE(d != nullptr);
    f.shell->typeText("a/b"); // replaces the selected proposal
    CHECK(d->field().text() == "a/b");
    CHECK_FALSE(d->valid());
    f.press(io::Control::SelectorPush); // push = primary: refused while invalid
    f.settle();
    CHECK(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "A name cannot contain / \\ : * ? \" < > |"));
    f.press(io::Control::Back);
    f.settle();
    f.press(io::Control::SelectorPush);
    d = dialog(f);
    REQUIRE(d != nullptr);
    f.shell->typeText(".x");
    CHECK_FALSE(d->valid());
    f.shell->textKey(ui::TextKey::Home);
    f.shell->textKey(ui::TextKey::Right);
    f.shell->typeText("ok"); // ".okx" still starts with '.'
    CHECK_FALSE(d->valid());
    f.press(io::Control::Back);
    f.settle();
    CHECK(f.shell->popupDepth() == 0);
}

TEST_CASE("ui.m2.project: SAVE of a saved project asks OVERWRITE?, then saves with a toast and stays", "[m2]")
{
    M2Fixture f;
    demoProjects();
    makeDirty(f);
    auto& src = FakeProjectSource::instance();
    CHECK(src.dirty());
    REQUIRE(f.open("project"));
    f.turn(io::Control::Selector, 2); // SAVE
    f.press(io::Control::SelectorPush);
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Overwrite Groove_Sketch_03?"));
    f.press(io::Control::SelectorPush); // OVERWRITE (default)
    f.settle();
    CHECK(f.shell->popupDepth() == 0);
    CHECK_FALSE(src.dirty());
    CHECK(m2test::hasText(f.dl, "Saved"));
    CHECK(f.shell->router().current().id == "project");
    CHECK_FALSE(m2test::hasText(f.dl, "Groove_Sketch_03 \xC2\xB7  edited"));
}

TEST_CASE("ui.m2.project: an unsaved project's SAVE asks for a name; SAVE AS onto an existing name asks REPLACE",
          "[m2]")
{
    M2Fixture f;
    demoProjects();
    auto& src = FakeProjectSource::instance();
    src.setCurrent("Untitled", false);
    REQUIRE(f.open("project"));
    f.turn(io::Control::Selector, 2);
    f.press(io::Control::SelectorPush);
    auto* d = dialog(f);
    REQUIRE(d != nullptr);
    f.shell->typeText("My_Beat");
    f.shell->textKey(ui::TextKey::Return);
    f.settle();
    CHECK(src.currentName() == "My_Beat");
    CHECK(src.exists("My_Beat"));
    f.turn(io::Control::Selector, 1); // SAVE AS
    f.press(io::Control::SelectorPush);
    f.shell->typeText("Breaks_120");
    f.shell->textKey(ui::TextKey::Return);
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Replace the existing project Breaks_120?"));
}

TEST_CASE("ui.m2.project: CREATE NEW warns when dirty, then names the project and goes HOME", "[m2]")
{
    M2Fixture f;
    demoProjects();
    makeDirty(f);
    REQUIRE(f.open("project"));
    f.press(io::Control::SelectorPush); // CREATE NEW
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Unsaved changes to Groove_Sketch_03 will be lost."));
    f.press(io::Control::SelectorPush); // OK
    f.settle();
    auto* d = dialog(f);
    REQUIRE(d != nullptr);
    CHECK(d->field().text() == "Project_001");
    f.shell->textKey(ui::TextKey::Return);
    f.settle();
    CHECK(f.shell->router().current().id == "home");
    CHECK(FakeProjectSource::instance().currentName() == "Project_001");
    CHECK_FALSE(f.session().patternHasContent(0, 0)); // a new default project
}

TEST_CASE("ui.m2.project: OPEN confirms and loads; DELETE confirms and skips the open project; sorting", "[m2]")
{
    M2Fixture f;
    demoProjects();
    auto& src = FakeProjectSource::instance();
    REQUIRE(f.open("project"));
    REQUIRE(f.open("project.open"));
    f.turn(io::Control::Selector, 1); // Live_Set_Nov
    f.press(io::Control::SelectorPush);
    f.settle();
    CHECK(m2test::hasText(f.dl, "Open Live_Set_Nov?"));
    f.press(io::Control::SelectorPush); // OPEN
    f.settle(3);
    CHECK(src.currentName() == "Live_Set_Nov");
    CHECK(f.shell->router().current().id == "home");

    REQUIRE(f.open("project"));
    REQUIRE(f.open("project.delete"));
    CHECK(ui::projectFocus(f.shell->body()) == 0); // Groove_Sketch_03 (row 0) is not the open project any more
    f.turn(io::Control::Selector, 1);              // Live_Set_Nov is open: skipped
    CHECK(ui::projectFocus(f.shell->body()) == 2);
    f.press(io::Control::SelectorPush);
    f.settle();
    CHECK(m2test::hasText(f.dl, "Move Dub_Techno_Idea to the Trash?"));
    f.turn(io::Control::Selector, 1);
    f.press(io::Control::SelectorPush);
    f.settle();
    CHECK_FALSE(src.exists("Dub_Techno_Idea"));

    f.tap(100, 224); // NAME header: sort A → Z
    f.settle();
    CHECK(m2test::hasText(f.dl, "NAME \xE2\x8C\x84"));
    const auto rows = src.list(ui::m2::ProjectSort::Name);
    CHECK(rows.front().name == "Ambient_Pads");
}

TEST_CASE("ui.m2.project: the screens drive the real ProjectStore through StoreProjectSource", "[m2]")
{
    M2Fixture f;
    const auto dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("m2ui-store", "");
    REQUIRE(dir.createDirectory());
    {
        services::library::Workers workers{1};
        services::store::ProjectStore store(f.session().bus(), workers, {.projectsDir = dir.getChildFile("Projects")});
        store.createUntitled();
        ui::m2::StoreProjectSource source(store);
        ui::m2::ProjectSource::install(&source);
        auto pump = [&]
        {
            workers.drain();
            for (int i = 0; i < 3; ++i)
                f.frame();
        };
        f.trigger(1, 0);
        REQUIRE(f.open("project"));
        CHECK(m2test::hasText(f.dl, "Untitled \xC2\xB7  edited"));
        f.turn(io::Control::Selector, 2); // SAVE of an untitled project: name dialog
        f.press(io::Control::SelectorPush);
        REQUIRE(dialog(f) != nullptr);
        f.shell->typeText("Store_Beat");
        f.shell->textKey(ui::TextKey::Return);
        pump();
        CHECK(store.hasFile());
        CHECK(store.name() == "Store_Beat");
        CHECK_FALSE(store.dirty());
        CHECK(m2test::hasText(f.dl, "Saved"));
        CHECK(source.exists("Store_Beat"));
        // CREATE NEW (clean: no warning) → the store's new project, HOME.
        f.turn(io::Control::Selector, -2);
        f.press(io::Control::SelectorPush);
        REQUIRE(dialog(f) != nullptr);
        f.shell->typeText("Second");
        f.shell->textKey(ui::TextKey::Return);
        pump();
        CHECK(store.name() == "Second");
        CHECK(f.shell->router().current().id == "home");
        CHECK_FALSE(f.session().patternHasContent(0, 0));
        // OPEN Store_Beat from the list: confirm, load on the worker, HOME with the saved steps back.
        REQUIRE(f.open("project"));
        REQUIRE(f.open("project.open"));
        CHECK(m2test::hasText(f.dl, "Store_Beat"));
        const auto rows = source.list(ui::m2::ProjectSort::Name);
        int at = 0;
        while (at < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(at)].name != "Store_Beat")
            ++at;
        f.turn(io::Control::Selector, at);
        f.press(io::Control::SelectorPush);
        f.press(io::Control::SelectorPush); // OPEN
        pump();
        CHECK(store.name() == "Store_Beat");
        CHECK(f.session().patternHasContent(0, 0));
        CHECK(f.shell->router().current().id == "home");
        ui::m2::ProjectSource::install(nullptr);
    }
    dir.deleteRecursively();
}
