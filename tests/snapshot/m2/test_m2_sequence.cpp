// SEQUENCE against sequence.svg (gate ≥ 97 %, 5 × 5 blur, max channel difference 24) and its scripted behaviour:
// step keys toggle targets (a hold + turn does not), a step written through the session appears on the grid and in the
// model, CLEAR TRIGGERS asks and is undoable, QUANTIZE zeroes offsets with a toast, BAR follows the bar keys with bars
// past the length disabled, the columns 1–3 act on the targets, SHIFT rotates the triggers.
#include "tests/snapshot/m2/M2Fixture.h"
#include "ui/screens/sequence/SequenceAccess.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/Message.h"

#include <catch2/catch_test_macros.hpp>

using namespace snapshot;
using m2test::M2Fixture;
namespace L = ui::layout;
using core::model::TriggerType;

namespace
{
// sequence.py STEPS: step → (type, offset in steps, retrigger count); track 1, bar 1.
void mockupSteps(M2Fixture& f)
{
    f.trigger(1, 0);
    f.trigger(1, 4);
    f.trigger(1, 6);
    f.lock(1, 6, ui::m2::kSlotOffset, -6); // −0.25 step
    f.trigger(1, 8);
    f.lock(1, 8, ui::m2::kSlotRetrigCount, 2);
    f.trigger(1, 10, TriggerType::Half);
    f.lock(1, 10, ui::m2::kSlotOffset, 6);
    f.trigger(1, 12);
    f.trigger(1, 15, TriggerType::LockOnly);
}

void playing(M2Fixture& f, int step0)
{
    auto& st = f.shell->state();
    st.playing = true;
    st.position.set("001:02:02");
    engine::MeterFeed m{};
    engine::PlayheadFeed p{};
    p.blockIndex = 1;
    p.playing = true;
    p.step = static_cast<std::uint8_t>(step0);
    p.ppq = step0 * 0.25;
    f.shell->feeds().inject(m, p);
}
} // namespace

TEST_CASE("ui.m2.sequence: SEQUENCE matches sequence.svg", "[snapshot][m2]")
{
    Session session;
    M2Fixture f;
    mockupSteps(f);
    REQUIRE(f.open("track.menu"));
    REQUIRE(f.open("track.sequence"));
    REQUIRE(f.shell->router().current().bodyComponent == "StepGrid");
    f.click(io::step(9)); // target step 9
    playing(f, 5);
    const auto ours = f.image();
    const auto& mock = m2test::m2mockup("sequence");
    CHECK(compare(ours, mock, L::canvas, "m2-sequence").match >= kGate);
    // The four locked steps (7, 9, 11, 16) are drawn in the lighter locked tint (by request), not the mockup's plain
    // track colour: their cells differ by design, so this region's gate allows them (≈ 3.5 % of its pixels).
    CHECK(compare(ours, mock, {16, 152, 1248, 336}, "m2-sequence-steps").match >= kGate - 0.02);
    CHECK(compare(ours, mock, {16, 500, 1248, 112}, "m2-sequence-buttons").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m2-sequence-strip").match >= kGate);
    writePng(f.image(800, 480), "m2-sequence-800.png");
}

TEST_CASE("ui.m2.sequence: a written step appears on the grid and in the model; step keys toggle targets", "[m2]")
{
    M2Fixture f;
    REQUIRE(f.open("track.sequence"));
    f.trigger(1, 2, TriggerType::Half);
    f.settle();
    const auto* m = ui::sequenceModel(f.shell->body(), f.now);
    REQUIRE(m != nullptr);
    CHECK(m->cells[2].kind == ui::StepKind::Half);
    const auto& cell = f.session().project().scenes[0].patterns[0].steps[2][0];
    CHECK(cell.trigger != core::model::Step::kEmpty);
    CHECK(cell.triggerType == TriggerType::Half);

    f.click(io::step(3));
    f.click(io::step(7));
    CHECK(ui::sequenceTargets(f.shell->body()) == ((1u << 2) | (1u << 6)));
    f.click(io::step(7));
    CHECK(ui::sequenceTargets(f.shell->body()) == (1u << 2));
    f.tap(ui::StepGrid::cell(5).x + 20, 240); // a tap on a cell toggles its target too
    CHECK(ui::sequenceTargets(f.shell->body()) == ((1u << 2) | (1u << 4)));
    // Leaving clears the targets (UI-246).
    f.press(io::Control::Back);
    f.settle();
    REQUIRE(f.open("track.sequence"));
    CHECK(ui::sequenceTargets(f.shell->body()) == 0);
}

TEST_CASE("ui.m2.sequence: CLEAR TRIGGERS asks, clears the bar's triggers and is undoable", "[m2]")
{
    M2Fixture f;
    f.trigger(1, 0);
    f.trigger(1, 4);
    f.trigger(1, 9);
    f.trigger(1, 20); // bar 2: untouched
    REQUIRE(f.open("track.sequence"));
    f.turn(io::Control::Selector, 2); // focus TRIGGER TYPE → BAR → CLEAR TRIGGERS
    f.press(io::Control::SelectorPush);
    f.settle();
    REQUIRE(f.shell->popupDepth() == 1);
    CHECK(m2test::hasText(f.dl, "Clear 3 triggers on track 1, bar 1?"));
    CHECK(f.session().patternHasContent(0, 0));
    f.turn(io::Control::Selector, 1); // CANCEL → CLEAR
    f.press(io::Control::SelectorPush);
    f.settle();
    CHECK(f.shell->popupDepth() == 0);
    auto& s = f.session();
    CHECK(s.cell(0, 0, 0, 0).kind == ui::StepKind::Empty);
    CHECK(s.cell(0, 0, 0, 9).kind == ui::StepKind::Empty);
    CHECK(s.cell(0, 0, 0, 20).kind == ui::StepKind::Full);
    REQUIRE(s.undo());
    CHECK(s.cell(0, 0, 0, 0).kind == ui::StepKind::Full);
    CHECK(s.cell(0, 0, 0, 4).kind == ui::StepKind::Full);
    CHECK(s.cell(0, 0, 0, 9).kind == ui::StepKind::Full);
}

TEST_CASE("ui.m2.sequence: QUANTIZE zeroes the targets' offsets with a toast; undo restores them", "[m2]")
{
    M2Fixture f;
    f.trigger(1, 0);
    f.lock(1, 0, ui::m2::kSlotOffset, 5);
    f.trigger(1, 1);
    f.lock(1, 1, ui::m2::kSlotOffset, -3);
    REQUIRE(f.open("track.sequence"));
    f.click(io::step(1)); // target step 1 only
    f.tap(1144 + 60, 512 + 44);
    f.settle();
    auto& s = f.session();
    CHECK(s.cell(0, 0, 0, 0).offset24 == 0);
    CHECK(s.cell(0, 0, 0, 1).offset24 == -3);
    CHECK(m2test::hasText(f.dl, "Quantized 1 trigger"));
    REQUIRE(s.undo());
    CHECK(s.cell(0, 0, 0, 0).offset24 == 5);
}

TEST_CASE("ui.m2.sequence: BAR follows the bar keys; bars past the pattern length are disabled", "[m2]")
{
    M2Fixture f;
    auto& s = f.session();
    s.execute(ui::m2::cmd::setPatternLength(s.project(), 0, 0, 32));
    f.trigger(1, 17);
    REQUIRE(f.open("track.sequence"));
    f.press(io::Control::Bar2);
    f.settle();
    const auto* m = ui::sequenceModel(f.shell->body(), f.now);
    REQUIRE(m != nullptr);
    CHECK(m->bar == 2);
    CHECK(m->cells[1].kind == ui::StepKind::Full);
    CHECK(m2test::hasText(f.dl, "2.1"));
    f.press(io::Control::Bar4); // past the 2-bar length: ignored
    f.settle();
    CHECK(ui::sequenceModel(f.shell->body(), f.now)->bar == 2);
}

TEST_CASE("ui.m2.sequence: columns 1-3 act on the targets; with none they edit the track value", "[m2]")
{
    M2Fixture f;
    f.trigger(1, 0);
    f.trigger(1, 1);
    f.lock(1, 1, ui::m2::kSlotOffset, 4);
    REQUIRE(f.open("track.sequence"));
    f.click(io::step(1));
    f.click(io::step(2));
    f.settle();
    CHECK(f.get("seq.offset") == 0); // the first target's value
    f.turn(io::knob(1), 3);
    f.settle();
    auto& s = f.session();
    CHECK(s.cell(0, 0, 0, 0).offset24 == 3); // relative to each target's own value
    CHECK(s.cell(0, 0, 0, 1).offset24 == 7);
    f.turn(io::knob(2), 2); // RETRIGGER CNT 1 → 3 on both
    f.settle();
    CHECK(s.cell(0, 0, 0, 0).retrigCount == 3);
    CHECK(s.cell(0, 0, 0, 1).retrigCount == 3);
    // Clearing the targets restores the track value; turning then edits it, not the steps.
    f.click(io::step(1));
    f.click(io::step(2));
    f.settle();
    CHECK(f.get("seq.offset") == 0);
    f.turn(io::knob(1), 2);
    f.settle();
    CHECK(f.get("seq.offset") == 2);
    CHECK(s.cell(0, 0, 0, 0).offset24 == 3);
}

TEST_CASE("ui.m2.sequence: SHIFT rotates the track's triggers inside SHIFT AREA", "[m2]")
{
    M2Fixture f;
    f.trigger(1, 0);
    f.trigger(1, 15);
    REQUIRE(f.open("track.sequence"));
    f.turn(io::knob(4), 1);
    f.settle();
    auto& s = f.session();
    CHECK(s.cell(0, 0, 0, 1).kind == ui::StepKind::Full);
    CHECK(s.cell(0, 0, 0, 0).kind == ui::StepKind::Full); // step 16 wrapped to step 1
    CHECK(s.cell(0, 0, 0, 15).kind == ui::StepKind::Empty);
}

TEST_CASE("ui.m2.sequence: TRIGGER TYPE segments set seq.triggerType; the playing step is outlined", "[m2]")
{
    M2Fixture f;
    REQUIRE(f.open("track.sequence"));
    f.tap(16 + 472 * 5 / 6, 512 + 44); // PARAM
    CHECK(f.get("seq.triggerType") == 2);
    f.press(io::Control::SelectorPush); // focused TRIGGER TYPE: next segment, wraps to FULL
    CHECK(f.get("seq.triggerType") == 0);
    playing(f, 3);
    f.settle();
    CHECK(ui::sequenceModel(f.shell->body(), f.now)->playStep == 4);
}
