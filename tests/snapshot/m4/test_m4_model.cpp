// The performance parameters in the document: ModelParamAccess rows for lfo.* (persisted through the
// native codec, posted to the engine on change and after an open) and seq.* (the paths core::perf::setStepsParam
// writes), the step / SHIFT edits of the SEQUENCE screen through core::perf, and the ×n badge from the track's
// RETRIGGER CNT.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/perf/ShiftSteps.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "services/store/NativeCodec.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "tests/snapshot/m4/M4Fixture.h"
#include "tests/unit/store/StoreTestSupport.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ModelParamAccess.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
struct Doc
{
    core::CommandBus bus;
    fakes::FakeEngineBridge bridge;
    ui::ModelParamAccess values{bus, &bridge};
    Doc()
    {
        core::commands::replaceProject(bus, core::model::newDefaultProject());
        core::commands::setBridge(bus, [this](const core::ChangeSet& c, const core::model::Project& p)
                                  { ui::forwardModelChanges(c, p, bridge); });
    }
    ~Doc() { core::commands::setBridge(bus, {}); }
    const core::ParamSpec& spec(std::string_view id) { return *core::builtinParams().find(id); }
    float get(std::string_view id, int track = 0) { return values.get(spec(id), static_cast<std::uint16_t>(track)); }
    void set(std::string_view id, float v, int track = 0) { values.set(spec(id), static_cast<std::uint16_t>(track), v); }
    const core::model::Track& track(int t = 0) { return core::commands::project(bus).scenes[0].tracks[static_cast<std::size_t>(t)]; }
    // The last ParamChange posted for `id` (NaN = none).
    float posted(std::string_view id, std::uint16_t scope = 0)
    {
        const auto pid = core::builtinParams().idOf(id);
        float v = std::nanf("");
        for (const auto& m : bridge.posted)
            if (const auto* c = std::get_if<engine::ParamChange>(&m); c != nullptr && c->param == *pid && c->scope == scope)
                v = c->value;
        return v;
    }
};
} // namespace

TEST_CASE("m4ui.model: every lfo.* parameter lives in the model, is undoable and reaches the engine", "[m4]")
{
    Doc d;
    const char* ids[] = {"lfo.speed", "lfo.depth", "lfo.phase", "lfo.shape", "lfo.destination", "lfo.sync",
                         "lfo.triggerMode", "lfo.triggerOn", "lfo.randomStart", "lfo.bypass"};
    for (const auto* id : ids)
        CHECK(d.values.modelBacked(d.spec(id)));
    d.set("lfo.speed", 40);
    d.set("lfo.depth", 100);
    d.set("lfo.shape", 4); // PULSE25
    d.set("lfo.destination", static_cast<float>(core::builtinParams().idOf("track.pan")->value));
    d.set("lfo.triggerOn", 1);
    CHECK(d.track().lfo.speed == 40);
    CHECK(d.track().lfo.depth == 100);
    CHECK(d.track().lfo.shape == core::model::LfoShape::Square);
    CHECK(d.get("lfo.shape") == 4);
    CHECK(d.get("lfo.destination") == static_cast<float>(core::builtinParams().idOf("track.pan")->value));
    CHECK(d.track().lfo.triggerOn == core::model::LfoTriggerOn::Both);
    CHECK(d.posted("lfo.depth") == 100);
    CHECK(d.posted("lfo.shape") == 4);
    CHECK(d.posted("lfo.destination") == static_cast<float>(core::builtinParams().idOf("track.pan")->value));
    // NONE clears the stored id; undo brings it back.
    d.set("lfo.destination", -1);
    CHECK(d.get("lfo.destination") == -1);
    CHECK(d.bus.undo());
    CHECK(d.get("lfo.destination") == static_cast<float>(core::builtinParams().idOf("track.pan")->value));
    // After a project open every LFO value is posted (forwardAllModel).
    d.bridge.posted.clear();
    ui::forwardAllModel(core::commands::project(d.bus), d.bridge);
    CHECK(d.posted("lfo.speed") == 40);
    CHECK(d.posted("lfo.shape") == 4);
}

TEST_CASE("m4ui.model: LFO settings survive a native save / open (shape and destination by ParamSpec id)", "[m4]")
{
    Doc d;
    d.set("lfo.shape", 5, 3); // PULSE12 on track 4
    d.set("lfo.destination", static_cast<float>(core::builtinParams().idOf("fx1.filter.cutoff")->value), 3);
    d.set("lfo.phase", 64, 3);
    d.set("lfo.sync", 1, 3);
    storetest::ScratchFolder dir("m4ui-lfo");
    const auto bundle = dir / "lfo";
    REQUIRE(static_cast<bool>(services::store::NativeCodec::save(core::commands::project(d.bus), bundle)));
    auto back = core::model::newDefaultProject();
    services::store::ImportReport report;
    REQUIRE(static_cast<bool>(services::store::NativeCodec::readInto(bundle, *back, report)));
    Doc e;
    core::commands::replaceProject(e.bus, std::move(back));
    CHECK(e.get("lfo.shape", 3) == 5);
    CHECK(e.get("lfo.destination", 3) == static_cast<float>(core::builtinParams().idOf("fx1.filter.cutoff")->value));
    CHECK(e.get("lfo.phase", 3) == 64);
    CHECK(e.get("lfo.sync", 3) == 1);
    CHECK(e.get("lfo.shape", 2) == 0); // the other tracks keep their defaults
    CHECK(e.get("lfo.destination", 2) == -1);
}

TEST_CASE("m4ui.model: seq.* are model-backed at setStepsParam's paths; mask 0 edits reach the engine", "[m4]")
{
    Doc d;
    for (const auto* id : {"seq.offset", "seq.retrigCount", "seq.retrigSpeed", "seq.transpose", "seq.shiftArea"})
        CHECK(d.values.modelBacked(d.spec(id)));
    CHECK(d.get("seq.retrigCount", 2) == 1); // the model's default (SEQ-030)
    const auto pid = core::builtinParams().idOf("seq.retrigCount");
    d.bus.execute(core::perf::setStepsParam(0, 0, 2, 0, *pid, 4)); // the track value, as SEQUENCE without targets
    CHECK(d.track(2).seq.retriggerCount == 4);
    CHECK(d.get("seq.retrigCount", 2) == 4);
    CHECK(d.posted("seq.retrigCount", 2) == 4); // forwarded through the shared path
    d.set("seq.transpose", -5, 2);
    CHECK(d.track(2).seq.transpose == -5);
    CHECK(d.posted("seq.transpose", 2) == -5);
}

TEST_CASE("m4ui.sequence: targets turn through setStepsParam, SHIFT through perf::shiftSteps; ×n from the track", "[m4]")
{
    m4test::M4Fixture m;
    auto& s = ui::m2::Session::instance();
    using core::model::TriggerType;
    s.execute(core::commands::sequencer::toggleStep(0, 0, 0, 0, TriggerType::Full, 127));
    s.execute(core::commands::sequencer::toggleStep(0, 0, 0, 4, TriggerType::Full, 127));
    // The track's RETRIGGER CNT draws on every unlocked step (the double marker).
    const auto pid = core::builtinParams().idOf("seq.retrigCount");
    s.execute(core::perf::setStepsParam(0, 0, 0, 0, *pid, 3));
    CHECK(s.cell(0, 0, 0, 0).retrigCount == 3);
    CHECK(s.cell(0, 0, 0, 4).retrigCount == 3);
    m.shell().selectTrack(1);
    REQUIRE(m.open("track.sequence"));
    m.click(io::step(1));
    m.click(io::step(5));
    m.f.settle();
    m.f.turn(io::knob(2), 2); // RETRIGGER CNT on both targets: locks (setStepsParam with their mask)
    m.f.settle();
    CHECK(s.cell(0, 0, 0, 0).retrigCount == 5);
    CHECK(s.cell(0, 0, 0, 4).retrigCount == 5);
    CHECK(m.shell().column(1).valueText == "5");
    m.click(io::step(5)); // one target left; different values on the two steps → `—` when both are targets
    m.f.settle();
    m.f.turn(io::knob(2), 1);
    m.f.settle();
    CHECK(s.cell(0, 0, 0, 0).retrigCount == 6);
    m.click(io::step(5));
    m.f.settle();
    CHECK(m.shell().column(1).valueText == "\xE2\x80\x94");
    m.click(io::step(1));
    m.click(io::step(5));
    m.f.settle();
    // SHIFT: one detent rotates the track's cells with their locks (ALL = the pattern).
    m.f.turn(io::knob(4), 1);
    m.f.settle();
    CHECK(s.cell(0, 0, 0, 1).kind == ui::StepKind::Full);
    CHECK(s.cell(0, 0, 0, 1).retrigCount == 6);
    CHECK(s.cell(0, 0, 0, 5).retrigCount == 5);
    CHECK(s.cell(0, 0, 0, 0).kind == ui::StepKind::Empty);
    CHECK(s.undo());
    CHECK(s.cell(0, 0, 0, 0).kind == ui::StepKind::Full);
}

TEST_CASE("ui.m1.trackSetting: tap two tracks to exchange them and tap TRACKS to return home", "[snapshot][m1]")
{
    snapshot::Session isolated;
    m1test::M1Fixture ui;
    auto& state = ui::m2::Session::instance();
    auto& firstScene = core::commands::project(state.bus()).scenes.front();
    firstScene.tracks[0].name = "Kick";
    firstScene.tracks[5].name = "Bass";
    firstScene.patterns[0].steps[2][0].trigger = 90;

    const auto tapCentre = [&](const ui::RectF& box) { ui.tap(box.x + box.w / 2, box.y + box.h / 2); };
    const auto trackButton = ui::layout::menuButton(0);
    tapCentre(trackButton);
    ui.settle();
    REQUIRE(ui.shell->current().id == "trackSetting");

    tapCentre(ui::layout::trackTile(1));
    tapCentre(trackButton);
    ui.settle();
    REQUIRE(ui.shell->current().id == "home");
    tapCentre(trackButton);
    ui.settle();
    tapCentre(ui::layout::trackTile(6));
    CHECK(firstScene.tracks[0].name == "Kick"); // leaving cancels the selection of the first slot

    tapCentre(ui::layout::trackTile(1));
    CHECK(firstScene.tracks[0].name == "Bass");
    CHECK(firstScene.tracks[5].name == "Kick");
    CHECK(firstScene.patterns[0].steps[2][5].trigger == 90);
    REQUIRE(state.undo());
    CHECK(firstScene.tracks[0].name == "Kick");
    CHECK(firstScene.patterns[0].steps[2][0].trigger == 90);
}
