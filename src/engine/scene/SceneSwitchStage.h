// Step 5 of the engine block (engine/stages/Stages.h, audio-engine.md "Per-block processing order"): the scene/pattern
// switcher as a stage of engine::Engine. It owns the ScenePatternSwitcher, takes the scene ParamChanges
// of the FIFO drain (step 1), runs after the transport (step 3) and before the sequencer (step 6) and fills
// BlockBuffers::timing; at the switch frame it hands the mixer the scene and the pattern mute/solo masks (SCN-012),
// the transport the scene BPM (SCN-004) and the message thread a SwitchDone event. Step 14 publishes state() into the
// PlayheadFeed (scene, pattern, step, pending scene/pattern).
#pragma once

#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/scene/ScenePatternSwitcher.h"
#include "engine/stages/BlockBuffers.h"

namespace engine::stages
{
struct SceneSwitchStage
{
    scene::ScenePatternSwitcher switcher;

    void prepare(double) noexcept {} // the switch state survives a device restart (the project did not change)

    // The scene / pattern ParamTargets (engine/bridge/ParamBindings.h), from Engine::apply.
    void param(bridge::ParamTarget target, const ParamChange& c) noexcept
    {
        using T = bridge::ParamTarget;
        const float v = c.value;
        const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
        const int scene = (c.scope >> 4) & 15, index = c.scope & 15; // (scene << 4) | pattern
        switch (target)
        {
            case T::SceneQueue: switcher.applyQueue(v); break;
            case T::PatternQuantize:
                switcher.setUnit(i <= 0 ? scene::SwitchUnit::Step
                                        : (i == 1 ? scene::SwitchUnit::Bar : scene::SwitchUnit::Pattern));
                break;
            case T::SceneSwitchMode: switcher.setContinue(i == 1); break;
            case T::SceneBpm:
                if (c.scope < scene::kScenes)
                    switcher.setSceneBpm(c.scope, v);
                break;
            case T::SceneBpmPolicy: switcher.setFollowSceneBpm(i == 0); break; // ASK: the UI asks first (+256)
            case T::PatternLength: switcher.setPatternLength(scene, index, i); break;
            case T::PatternMutes:
                switcher.setPatternMuteSolo(scene, index, static_cast<std::uint16_t>(i & 0xFFFF),
                                            switcher.patternSolo(scene, index));
                break;
            case T::PatternSolos:
                switcher.setPatternMuteSolo(scene, index, switcher.patternMute(scene, index),
                                            static_cast<std::uint16_t>(i & 0xFFFF));
                break;
            default: break;
        }
    }

    // Transport: command(const TransportCmd&); Mixer: setScene / setPatternMuteSolo; Events: pushEvent(EngineEvent).
    template <class Transport, class Mixer, class Events>
    void process(BlockContext& ctx, BlockBuffers& b, Transport& transport, Mixer& mixer, Events& events) noexcept
    {
        switcher.process(ctx, b.timing, transport, mixer, events);
    }

    void publish(PlayheadFeed& feed, double ppq) const noexcept { switcher.publish(feed, ppq); }
};
} // namespace engine::stages
