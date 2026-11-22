#include "app/ProjectFeed.h"

#include "core/commands/arrangement/ArrangementPlanBuilder.h"
#include "core/params/ParamRegistry.h"
#include "io/controllers/StripSource.h"
#include "io/midi/MidiSettings.h"
#include "ui/input/scenes/SceneSync.h"
#include "ui/input/steps/PatternSync.h"
#include "ui/screens/setslice/SliceSync.h"
#include "ui/screens/effects/ModelFxSlots.h"
#include "ui/shell/ModelParamAccess.h"

namespace app::feed
{
void forwardAll(const core::model::Project& p, engine::EngineBridge& to)
{
    for (int s = 0; s < 16; ++s)
    {
        ui::forwardAllModel(p, to, core::builtinParams(), s);
        ui::fx::forwardAllFx(p, to, core::builtinParams(), s);
    }
    ui::input::steps::forwardAllSequencer(p, to);
    io::postRibbonSettings(p.ribbon, to, core::builtinParams()); // RIB-009
    ui::input::scenes::SceneSync(core::builtinParams(), to).syncAll(p);
    postRouting(p, to);
    io::midi::postMidiSettings(p, to, core::builtinParams()); // enables, channels, MIDI tracks
}

// The project's routing reaches the engine when it opens — ASSIGN MODE and HP CUE per scene, OUT
// ASSIGN per scene and track, MONITOR LEVEL (MIX-030/031/040/050). Edits after that are model edits
// (ui::ModelParamAccess's routing fields) that ui::forwardModelChanges posts with their scene / track scope.
void postRouting(const core::model::Project& p, engine::EngineBridge& to)
{
    const auto& params = core::builtinParams();
    const auto id = [&](std::string_view name) { return params.idOf(name).value_or(core::ParamId {}); };
    const auto mode = id("routing.assignMode"), cue = id("routing.headphoneCue"), out = id("routing.outAssign");
    for (int s = 0; s < 16; ++s)
    {
        const auto& scene = p.scenes[static_cast<std::size_t>(s)];
        const auto sc = static_cast<std::uint16_t>(s);
        to.post(engine::ParamChange {mode, sc, static_cast<float>(scene.routing.assignMode), 0});
        to.post(engine::ParamChange {cue, sc, static_cast<float>(scene.routing.headphoneSource), 0});
        for (int t = 0; t < 16; ++t)
            to.post(engine::ParamChange {out, static_cast<std::uint16_t>((s << 4) | t),
                                         static_cast<float>(scene.tracks[static_cast<std::size_t>(t)].routing.output),
                                         0});
    }
    to.post(engine::ParamChange {id("routing.monitorLevel"), 0, static_cast<float>(p.settings.monitorVolume), 0});
}

void postPlan(const core::model::Project& p, engine::EngineBridge& to, core::arrangement::SongStatus* status)
{
    auto* plan = new core::arrangement::ArrangementPlan();
    core::commands::arrangement::buildPlan(p, *plan);
    plan->status = status;
    if (! to.post(engine::ObjectSwap {{engine::SwapSlot::SceneConfig, core::arrangement::kSwapScene,
                                       core::arrangement::kSwapIndex},
                                      plan}))
        core::arrangement::destroyArrangementPlan(plan);
}

void postPlay(const core::model::Project& p, engine::EngineBridge& to)
{
    if (const auto id = core::builtinParams().idOf("arrangement.play"))
        to.post(engine::ParamChange {*id, 0, p.settings.playArrangement ? 1.0f : 0.0f, 0});
}

void postSlices(const core::model::Project& p, engine::EngineBridge& to, const services::render::SampleSnapshot& samples)
{
    ui::slice::postAll(to, p,
                       [&samples](int s, int t)
                       {
                           const auto* b = samples.at(s, t);
                           return b != nullptr ? b->frames : std::int64_t {0};
                       });
}

services::render::ProjectFeeder feeder(bool withPlan)
{
    return [withPlan](const core::model::Project& p, engine::EngineBridge& to, core::arrangement::SongStatus* status,
                      const services::render::SampleSnapshot& samples)
    {
        forwardAll(p, to);
        if (withPlan)
            postPlan(p, to, status);
        postSlices(p, to, samples);
    };
}
} // namespace app::feed
