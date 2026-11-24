// BPM/QUANTIZE against bpm-quantize.svg and its scripted behaviour: the BPM field has the focus on open, turn = 1 BPM,
// SHIFT + turn = 0.1, push moves the focus BPM → SWING → QUANTIZE, two taps 500 ms apart give 120.0 BPM, a QUANTIZE
// tap shows on the context bar at once, ◀ / ▶ act while held, an external clock disables BPM / TAP / NUDGE.
#include "tests/snapshot/m2/M2Fixture.h"
#include "ui/screens/bpmquantize/BpmQuantizeAccess.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/Modal.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m2test::M2Fixture;
namespace L = ui::layout;

namespace
{
ui::Widget* panel(M2Fixture& f)
{
    auto* modal = dynamic_cast<ui::Modal*>(f.shell->topPopup());
    return modal != nullptr ? modal->body() : nullptr;
}
float project(M2Fixture& f, std::string_view id)
{
    const auto* s = f.shell->params().find(id);
    return s != nullptr ? f.shell->values().get(*s, 0) : -1;
}
} // namespace

TEST_CASE("ui.m2.bpmQuantize: BPM/QUANTIZE matches bpm-quantize.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    const auto* swing = f.shell->params().find("seq.swing");
    const auto* metronome = f.shell->params().find("transport.metronome");
    REQUIRE(swing != nullptr);
    REQUIRE(metronome != nullptr);
    f.shell->values().set(*swing, 0, 50);
    f.shell->values().set(*metronome, 0, 1);
    f.tap(312, 88); // the BPM | QUANTIZE context item
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    REQUIRE(ui::bpmQuantizeFocus(panel(f)) == 0); // focus on the BPM field (UID-626)
    const auto ours = f.image();
    const auto& mock = m2test::m2mockup("bpm-quantize");
    CHECK(compare(ours, mock, L::canvas, "m2-bpm-quantize").match >= kGate);
    CHECK(compare(ours, mock, L::modalPanel, "m2-bpm-quantize-panel").match >= kGate);
}

TEST_CASE("ui.m2.bpmQuantize: rotary edits the focused field, push moves the focus, TAP averages", "[m2]")
{
    M2Fixture f;
    REQUIRE(f.shell->open("bpmQuantize"));
    f.settle();
    auto* p = panel(f);
    REQUIRE(p != nullptr);
    f.turn(io::Control::Selector, 3);
    CHECK(project(f, "transport.tempo") == 123.0f);
    f.press(io::Control::Shift);
    f.turn(io::Control::Selector, -2);
    f.release(io::Control::Shift);
    CHECK(std::abs(project(f, "transport.tempo") - 122.8f) < 1e-3f);
    f.settle();
    CHECK(f.shell->state().bpm.view() == "122.8"); // the context bar follows
    f.press(io::Control::SelectorPush);
    CHECK(ui::bpmQuantizeFocus(p) == 1);
    f.turn(io::Control::Selector, 7);
    CHECK(project(f, "seq.swing") == 7.0f);
    f.press(io::Control::SelectorPush);
    CHECK(ui::bpmQuantizeFocus(p) == 2);
    f.turn(io::Control::Selector, 1);
    CHECK(project(f, "seq.recordQuantize") == 4.0f);
    f.settle();
    CHECK(f.shell->state().quantize.view() == "1/8T");
    f.press(io::Control::SelectorPush);
    CHECK(ui::bpmQuantizeFocus(p) == 0);

    // TAP: two taps 500 ms apart = 120.0 BPM after the second tap (UID-627); 2 s later the average restarts.
    f.tap(288, 244);
    CHECK(std::abs(project(f, "transport.tempo") - 122.8f) < 1e-3f);
    f.now += 500;
    f.tap(288, 244);
    CHECK(project(f, "transport.tempo") == 120.0f);
    f.now += 2500;
    f.tap(288, 244);
    CHECK(project(f, "transport.tempo") == 120.0f);
    f.now += 400;
    f.tap(288, 244);
    CHECK(project(f, "transport.tempo") == 150.0f);

    // A vertical drag on the SWING field turns it (8 px per detent).
    f.touch(990, 244, ui::touch::kDown);
    f.touch(990, 244 - 8 * 1.6f * 4 - 6, ui::touch::kDrag);
    f.touch(990, 244 - 8 * 1.6f * 4 - 6, ui::touch::kUp);
    CHECK(project(f, "seq.swing") == 11.0f);

    // QUANTIZE: a tap selects at once and shows on the context item.
    f.tap(120 + 173 * 1 + 80, 560);
    f.settle();
    CHECK(project(f, "seq.recordQuantize") == 1.0f);
    CHECK(f.shell->state().quantize.view() == "1/32");
}

TEST_CASE("ui.m2.bpmQuantize: NUDGE acts while held; an external clock disables BPM, TAP and NUDGE", "[m2]")
{
    M2Fixture f;
    REQUIRE(f.shell->open("bpmQuantize"));
    f.settle();
    f.touch(948, 420, ui::touch::kDown); // ▶
    CHECK(project(f, "transport.nudge") == 1.0f);
    f.touch(948, 420, ui::touch::kUp);
    CHECK(project(f, "transport.nudge") == 0.0f);
    f.shell->state().syncBadge = "MIDI";
    f.shell->state().bpm.set("131.4"); // the followed clock's tempo (TransportController::publish)
    f.settle();
    CHECK(m2test::hasText(f.dl, "MIDI"));
    // The panel shows the followed tempo and leaves it on the context bar: not the project's 120.0.
    CHECK(f.shell->state().bpm.view() == "131.4");
    CHECK(m2test::hasText(f.dl, "131"));
    CHECK(m2test::hasText(f.dl, ".4"));
    f.turn(io::Control::Selector, 5);
    CHECK(project(f, "transport.tempo") == 120.0f);
    f.touch(948, 420, ui::touch::kDown);
    CHECK(project(f, "transport.nudge") == 0.0f);
    f.press(io::Control::Back);
    f.settle();
    CHECK(f.shell->popupDepth() == 0);
}
