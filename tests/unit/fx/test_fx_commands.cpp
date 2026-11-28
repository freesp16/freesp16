// FX commands (core/commands/fx): type selection with type memory and per-slot defaults (FX-006/007/091…093), the
// SYNC toggle with conversion in one undo step (FX-073…075), bypass and FX mode.
#include "core/commands/fx/FxCommands.h"
#include "core/model/Project.h"
#include "core/params/catalogue/fx.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace core;
using model::EffectType;

namespace
{
// Stand-in parameter tables for two model types (independent of the real effects' tables).
constexpr fx::EffectParamDef kChorus[] = {{.key = "rate",
                                           .label = "RATE",
                                           .shortLabel = "RATE",
                                           .k = 0,
                                           .free = {1, 101},
                                           .sync = {0, 10},
                                           .family = fx::Family::Chorus,
                                           .defInsert = 0.2f,
                                           .defSend = 0.2f,
                                           .defMaster = 0.2f},
                                          {.key = "mix",
                                           .label = "MIXRATIO",
                                           .shortLabel = "MIX",
                                           .k = 5,
                                           .free = {0, 101},
                                           .defInsert = 0.0f,
                                           .defSend = 1.0f,
                                           .defMaster = 0.5f}};
constexpr fx::EffectParamDef kDelay[] = {{.key = "time",
                                          .label = "TIME",
                                          .shortLabel = "TIME",
                                          .k = 0,
                                          .free = {10, 1001},
                                          .sync = {0, 6},
                                          .family = fx::Family::InsertDelay,
                                          .defInsert = 490.0f / 990.0f}};

void registerStandIns()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    ParamRegistry scratch; // only the recorded defs matter here
    fx::registerEffect(scratch, "chorus", kChorus, fx::kSlotInsert | fx::kSlotSend | fx::kSlotMaster, "fx.unit");
    fx::registerEffect(scratch, "delay", kDelay, fx::kSlotInsert, "fx.unit");
}

model::ParamPath insertAt(int scene, int track, int unit)
{
    model::ParamPath p{};
    p.scope = model::Scope::InsertFx;
    p.scene = static_cast<std::uint8_t>(scene);
    p.track = static_cast<std::uint8_t>(track);
    p.slot = static_cast<std::uint8_t>(unit);
    return p;
}
} // namespace

TEST_CASE("fx.commands: SelectFxType restores the type memory or the slot defaults (FX-006/092/093)", "[fx][commands]")
{
    registerStandIns();
    auto project = std::make_unique<model::Project>();
    const auto at = insertAt(0, 3, 0);
    SelectFxType(at, EffectType::Chorus).apply(*project);
    auto& s = fxSlotAt(*project, at);
    CHECK(s.type == EffectType::Chorus);
    CHECK(s.params[0] == static_cast<double>(0.2f));
    CHECK(s.params[5] == 0.0); // FX-093: insert Chorus Mix 0
    s.params[5] = 0.7;
    SelectFxType(at, EffectType::InsertDelay).apply(*project);
    CHECK(s.type == EffectType::InsertDelay);
    CHECK(s.params[5] == 0.0);
    SelectFxType(at, EffectType::Chorus).apply(*project); // FX-006: the Chorus comes back with its values
    CHECK(s.params[5] == 0.7);
    CHECK(s.typeMemory.size() == 1);
    CHECK(s.typeMemory[0].type == EffectType::InsertDelay);
    SelectFxType(at, EffectType::SendDelay).apply(*project); // not an insert type (FX-040): ignored
    CHECK(s.type == EffectType::Chorus);
    SelectFxType(at, EffectType::Reverb).apply(*project); // the Reverb is (ours): the insert defaults
    CHECK(s.type == EffectType::Reverb);
    CHECK(s.params[5] == static_cast<double>(0.3f));
    SelectFxType(at, EffectType::Chorus).apply(*project);
    CHECK(s.type == EffectType::Chorus);
    // Send defaults differ (FX-093: Send Mix 1.0, Master 0.5)
    model::ParamPath send{};
    send.scope = model::Scope::SendFx;
    SelectFxType(send, EffectType::Chorus).apply(*project);
    CHECK(project->scenes[0].sendFx.params[5] == 1.0);
    model::ParamPath master = send;
    master.scope = model::Scope::MasterFx;
    SelectFxType(master, EffectType::Chorus).apply(*project);
    CHECK(project->scenes[0].masterFx.params[5] == 0.5);
    // undo through the inverse restores the whole slot
    const auto before = s;
    SelectFxType cmd(at, EffectType::Off);
    auto inv = cmd.inverse(*project);
    cmd.apply(*project);
    CHECK(s.type == EffectType::Off);
    CHECK(s.params[5] == 0.0); // FX-091
    inv->apply(*project);
    CHECK(s == before);
    ChangeSet c;
    cmd.collectChanges(c);
    CHECK(c.paths().front() == "scene[0].track[3].insertFx[0]");
}

TEST_CASE("fx.commands: ToggleFxSync converts and flips in one command (FX-073…075)", "[fx][commands]")
{
    registerStandIns();
    auto project = std::make_unique<model::Project>();
    const auto at = insertAt(1, 0, 1);
    SelectFxType(at, EffectType::InsertDelay).apply(*project);
    auto& s = fxSlotAt(*project, at);
    s.params[0] = static_cast<double>(fx::valueOfStep(500, {10, 1001})); // 500 ms
    ToggleFxSync on(at, 120.0);
    auto undo = on.inverse(*project);
    on.apply(*project);
    CHECK(s.sync);
    CHECK(fx::syncBeats(fx::Family::InsertDelay, fx::stepIndex(static_cast<float>(s.params[0]), {0, 6})) == 1.0f);
    ToggleFxSync(at, 60.0).apply(*project); // back to free at 60 BPM: 1 beat = 1000 ms
    CHECK_FALSE(s.sync);
    CHECK(fx::stepIndex(static_cast<float>(s.params[0]), {10, 1001}) == 1000);
    undo->apply(*project);
    CHECK_FALSE(s.sync);
    CHECK(fx::stepIndex(static_cast<float>(s.params[0]), {10, 1001}) == 500);
}

TEST_CASE("fx.commands: bypass and FX mode are plain SetParams", "[fx][commands]")
{
    auto project = std::make_unique<model::Project>();
    const auto at = insertAt(2, 5, 1);
    auto b = makeSetFxBypass(at, true);
    b->apply(*project);
    CHECK(project->scenes[2].tracks[5].insertFx[1].bypass);
    auto m = makeSetFxMode(2, model::FxMode::Send);
    auto inv = m->inverse(*project);
    m->apply(*project);
    CHECK(project->scenes[2].routing.fxMode == model::FxMode::Send);
    inv->apply(*project);
    CHECK(project->scenes[2].routing.fxMode == model::FxMode::Both);
    ChangeSet c;
    m->collectChanges(c);
    CHECK(c.paths().front() == "scene[2].routing.fxMode");
}

TEST_CASE("fx.commands: the Send and Master bypass are the scene's slot fields (FX-046)", "[fx][commands]")
{
    auto project = std::make_unique<model::Project>();
    for (const auto scope : {model::Scope::SendFx, model::Scope::MasterFx})
    {
        model::ParamPath at{};
        at.scope = scope;
        at.scene = 4;
        auto b = makeSetFxBypass(at, true);
        auto inv = b->inverse(*project);
        b->apply(*project);
        CHECK(fxSlotAt(*project, at).bypass);
        CHECK_FALSE(project->scenes[3].sendFx.bypass); // only that scene's slot
        CHECK_FALSE(project->scenes[4].tracks[0].insertFx[0].bypass);
        ChangeSet c;
        b->collectChanges(c);
        CHECK(c.paths().front() == fxSlotPath(at) + ".bypass");
        inv->apply(*project);
        CHECK_FALSE(fxSlotAt(*project, at).bypass);
    }
}
