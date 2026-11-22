// RENDER AUDIO (project.md "Layout — RENDER AUDIO", UID-858…860; REN-001…005): the screen against
// render-audio.svg from its new home (ui/screens/render), the batch it hands the RenderHost (MIX = the pattern with the
// bus FX, a track = its insert FX on a direct output, TO TRACK targets, ARRANGEMENT only with rows), the Progress with
// CANCEL and the closing Toast, and the keyboard-only render workflow (UI-420).
#include "core/commands/arrangement/ArrangementCommands.h"
#include "tests/snapshot/m5/M5Fixture.h"
#include "ui/screens/render/RenderHost.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/Progress.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m5test::M5Fixture;
namespace L = ui::layout;
namespace rnd = services::render;

namespace ui
{
int renderFocus(const Widget* body) noexcept; // ui/screens/render/RenderScreen.cpp
void resetRenderScreen() noexcept;
} // namespace ui

namespace
{
class FakeRenderHost final : public ui::m5::RenderHost
{
public:
    FakeRenderHost() { install(this); }
    ~FakeRenderHost() override { install(nullptr); }
    bool start(std::vector<rnd::RenderJob> j, bool ext) override
    {
        if (ext)
        {
            s.error = "no external location";
            return false;
        }
        jobs = std::move(j);
        s.busy = true;
        s.count = static_cast<int>(jobs.size());
        s.index = 0;
        s.fraction = 0;
        return true;
    }
    void cancel() override
    {
        cancelled = true;
        finish(true);
    }
    const ui::m5::RenderStatus& status() const override { return s; }
    std::string_view destinationName(bool) const override { return "Library / RenderedAudio"; }
    void finish(bool wasCancelled = false)
    {
        s.busy = false;
        s.cancelled = wasCancelled;
        s.written = wasCancelled ? 0 : s.count;
        s.peakDb = -0.4;
        s.bars = 2;
        s.bpm = 120;
        ++s.finished;
    }
    std::vector<rnd::RenderJob> jobs;
    ui::m5::RenderStatus s;
    bool cancelled = false;
};

struct RenderFixture : M5Fixture
{
    FakeRenderHost host;
    RenderFixture()
    {
        ui::resetRenderScreen();
        for (auto& t : state().tracks)
            t.hasContent = true;
    }
    ~RenderFixture() { ui::resetRenderScreen(); }
};
} // namespace

TEST_CASE("snapshot.m5.render: RENDER AUDIO (ui/screens/render) matches render-audio.svg", "[snapshot][m5]")
{
    Session session;
    RenderFixture m;
    REQUIRE(m.open("project"));
    REQUIRE(m.open("project.renderAudio"));
    m.f.tap(32 + 90, 316 + 68 * 3 + 30);       // TRACK 1
    m.f.tap(32 + 188 + 90, 316 + 68 * 3 + 30); // TRACK 2 (focused)
    const auto ours = m.f.image();
    CHECK(m.hasText("3 files \xE2\x86\x92 Library / RenderedAudio"));
    CHECK(m.shell().column(1).valueText == "24-bit");
    CHECK(m.shell().column(0).valueText == "1\xC3\x97");
    const auto& mock = m5test::m5mockup("render-audio");
    CHECK(compare(ours, mock, {16, 152, 1248, 456}, "m5-render-body").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m5-render-strip").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m5-render-audio").match >= kGate);
}

TEST_CASE("m5ui.render: the batch, ARRANGEMENT only with rows, TO TRACK targets, progress, cancel, the Toast", "[m5]")
{
    RenderFixture m;
    REQUIRE(m.open("project.renderAudio"));
    // ARRANGEMENT is disabled while the arrangement is empty.
    m.f.tap(808 + 340, 200 + 28);
    m.f.settle();
    m.f.tap(32 + 90, 316 + 68 * 3 + 30); // TRACK 1
    m.f.tap(1072 + 96, 520 + 44);        // RENDER
    m.f.settle();
    REQUIRE(m.host.jobs.size() == 2);
    CHECK(m.host.jobs[0].source == rnd::Source::Pattern); // MIX: still PATTERN
    CHECK(m.host.jobs[0].includeBusFx);
    CHECK(m.host.jobs[1].source == rnd::Source::Track);
    CHECK(m.host.jobs[1].track == 0);
    CHECK_FALSE(m.host.jobs[1].includeBusFx);
    CHECK(m.host.jobs[0].bitDepth == 24);
    // The Progress (after 300 ms) shows the batch; the host finishing closes it with a Toast.
    m.host.s.fraction = 0.5f;
    m.f.settle();
    auto* progress = dynamic_cast<ui::Progress*>(m.shell().topPopup());
    REQUIRE(progress != nullptr);
    CHECK(progress->fraction() == 0.5f);
    m.host.finish();
    m.f.settle();
    CHECK(m.shell().popupDepth() == 0);
    CHECK(m.hasText("2 files rendered \xC2\xB7 peak -0.4 dBFS \xC2\xB7 2.00 bars 120.0 BPM"));
    // With rows: ARRANGEMENT renders the song; REPEATS is disabled there.
    auto& s = m.session();
    s.execute(core::commands::arrangement::appendCurrent(s.project()));
    m.f.tap(808 + 340, 200 + 28);
    m.f.settle();
    CHECK(m.shell().column(0).disabled);
    // TO TRACK: one empty track per file (the fixture's model has no samples: 16 free).
    m.f.tap(808 + 380, 408 + 28);
    m.f.settle();
    CHECK(m.hasText("2 files \xE2\x86\x92 empty tracks"));
    m.f.tap(1072 + 96, 520 + 44);
    m.f.settle();
    REQUIRE(m.host.jobs.size() == 2);
    CHECK(m.host.jobs[0].source == rnd::Source::Arrangement);
    CHECK(m.host.jobs[0].toTrack);
    CHECK(m.host.jobs[0].targetTrack == 0);
    CHECK(m.host.jobs[1].targetTrack == 1);
    // CANCEL on the Progress cancels the batch.
    m.f.settle();
    REQUIRE(m.shell().topPopup() != nullptr);
    m.push();
    m.f.settle();
    CHECK(m.host.cancelled);
    CHECK(m.hasText("Render cancelled"));
}

TEST_CASE("m5ui.keyboard_only.render: render the pattern and track 1 with the UI-120 keys alone (W14, UI-420)", "[m5]")
{
    RenderFixture m;
    m.jump(io::Control::ExtProject, false, "project"); // ⌘4
    REQUIRE(m.shell().current().id == "project");
    m.turnSel(-20); // ',' to the top, '.' × 6 to RENDER AUDIO, Return
    m.turnSel(6);
    m.push();
    m.f.settle();
    REQUIRE(m.shell().current().id == "project.renderAudio");
    m.turnSel(-30);
    m.turnSel(12); // TRACK 1 (pad order: 13 14 15 16, 9 … 12, 5 … 8, 1)
    CHECK(ui::renderFocus(m.shell().body()) == 12);
    m.push();
    m.knob(2, -1); // Ctrl+2 ↓: BIT DEPTH 16-bit
    m.turnSel(11); // RENDER
    CHECK(ui::renderFocus(m.shell().body()) == 23);
    m.push();
    m.f.settle();
    REQUIRE(m.host.jobs.size() == 2);
    CHECK(m.host.jobs[0].bitDepth == 16);
    CHECK(m.host.jobs[1].track == 0);
    m.host.finish();
    m.f.settle();
    CHECK(m.hasText("2 files rendered \xC2\xB7 peak -0.4 dBFS \xC2\xB7 2.00 bars 120.0 BPM"));
}
