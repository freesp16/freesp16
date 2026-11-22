// BROWSE against browse.svg (USB loops folder open, track 5's Beat loop assigned, focus on Pad_A with the preview
// playing) and browse-setting.svg; behaviour: open expands to the assigned file (LIB-018), push expands / loads, the
// load runs while a (fake) transport plays and never stops it (UID-686), UNLOAD with a toast, SHIFT + turn jumps
// folder to folder, virtualised rows.
#include "tests/snapshot/m1/FakeLibrary.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/ListTable.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m1test::M1Fixture;
namespace L = ui::layout;
using ui::m1::BrowseEntry;

namespace
{
struct BrowseFixture : M1Fixture
{
    m1test::FakeLibrary lib;
    BrowseFixture()
    {
        ui::m1::BrowseSource::install(&lib);
        shell->state().selectedTrack = 5;
        REQUIRE(shell->open("track.menu"));
        settle();
        REQUIRE(shell->open("track.browse"));
        settle();
    }
    ~BrowseFixture() { ui::m1::BrowseSource::install(nullptr); }
    bool drew(std::string_view text)
    {
        for (const auto& c : frame().commands())
            if (c.op == ui::DrawCmd::Op::Text && dl.textOf(c) == text)
                return true;
        return false;
    }
};
} // namespace

TEST_CASE("ui.m1.browse: BROWSE matches browse.svg", "[snapshot][m1]")
{
    Session session;
    BrowseFixture f;
    f.turn(io::Control::Selector, 1); // from the assigned Beat loop to Pad_A: preview plays
    CHECK(f.lib.lastPreview == "$Ext:USB/Samples/loops/Pad_A_120bpm_2bar.wav");
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("browse");
    CHECK(compare(ours, mock, L::canvas, "m1-browse").match >= kGate);
    CHECK(compare(ours, mock, ui::ListTable::kPanel, "m1-browse-list").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 448}, "m1-browse-rail").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m1-browse-strip").match >= kGate);
}

TEST_CASE("ui.m1.browse: SETTING popup matches browse-setting.svg", "[snapshot][m1]")
{
    Session session;
    BrowseFixture f;
    f.tap(112, 376); // SETTING
    REQUIRE(f.shell->popupDepth() == 1);
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("browse-setting");
    CHECK(compare(ours, mock, {320, 316, 640, 136}, "m1-browse-setting-popup").match >= kGate);
}

TEST_CASE("ui.m1.browse: a load runs while the transport plays and never stops it; UNLOAD toasts", "[m1]")
{
    BrowseFixture f;
    auto& st = f.shell->state();
    st.playing = true;
    f.lib.deferLoads = true;
    f.turn(io::Control::Selector, 1);   // Pad_A
    f.press(io::Control::SelectorPush); // load
    CHECK(f.lib.loaded == "$Ext:USB/Samples/loops/Pad_A_120bpm_2bar.wav");
    f.lib.pendingProgress(0.4f);
    CHECK(ui::m1::loadProgress()[4] == 0.4f);
    CHECK(f.shell->body()->dirty(f.now));
    for (int i = 0; i < 10; ++i)
        f.frame(); // the UI keeps running; nothing waits for the load
    CHECK(st.playing);
    CHECK(f.shell->popupDepth() == 0); // never blocking (UID-686)
    f.turn(io::Control::Selector, -1); // the list stays usable during the load
    f.lib.assignedPath = f.lib.loaded;
    f.lib.pendingDone(true, {});
    CHECK(ui::m1::loadProgress()[4] < 0);
    CHECK(st.playing);
    f.tap(112, 560); // UNLOAD
    CHECK(f.lib.assignedPath.empty());
    CHECK(f.drew("Unloaded"));
}

TEST_CASE("ui.m1.browse: push expands and collapses, SHIFT + turn jumps folders, rows are virtualised", "[m1]")
{
    BrowseFixture f;
    f.lib.bigFolder = 500;
    auto* body = f.shell->body();
    REQUIRE(body != nullptr);
    f.turn(io::Control::Selector, -20); // to the first root (INTERNAL)
    f.press(io::Control::SelectorPush); // expand 500 files
    f.frame();
    // only 9 rows are built, whatever the folder size
    int rows = 0;
    for (const auto& c : f.frame().commands())
        if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c).rfind("Hit_", 0) == 0)
            ++rows;
    CHECK(rows <= 9);
    f.press(io::Control::Shift);
    f.turn(io::Control::Selector, 1); // jump over 500 files to the next folder (USB Samples)
    f.release(io::Control::Shift);
    CHECK(f.drew("Samples"));
    f.turn(io::Control::Selector, -1);
    f.press(io::Control::Shift);
    f.turn(io::Control::Selector, -1);
    f.release(io::Control::Shift);
    f.press(io::Control::SelectorPush); // collapse the INTERNAL root
    int hits = 0;
    for (const auto& c : f.frame().commands())
        if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c).rfind("Hit_", 0) == 0)
            ++hits;
    CHECK(hits == 0);
}
