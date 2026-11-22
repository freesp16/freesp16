// The engine side of the sequencer commands (threading.md "Object swap protocol"): every pattern a ChangeSet touched is
// rebuilt as the engine's immutable pattern object and installed with ObjectSwap{SwapSlot::Pattern, scene, pattern};
// the engine retires the old one after ≥ 2 blocks. The CommandBus bridge hook calls forwardPatternChanges next to the
// other forwarders (app/Document.cpp: composition-root wiring, like ui::input::pads::forwardTrackChanges); after a
// project is opened or replaced, forwardAllSequencer posts all 256 patterns and the sequencer's project / track values
// (swing, PAD STARTS SEQ, the tracks' OFF SET).
//
// Paths: "scene[s].pattern[p]…" rebuilds that pattern; "scene[s]" (a whole scene replaced) or
// "scene[s].track[t]" with a pattern copy rebuild the scene's 16 patterns. The rebuild is ≈ 9 KB + locks, ≤ 0.3 ms on
// the A9 per pattern. Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"

namespace core::model
{
struct Project;
}

namespace ui::input::steps
{
// Builds and posts one pattern; false when no engine builder is registered or the bridge refused it (the object is
// destroyed then).
bool postPattern(engine::EngineBridge&, const core::model::Project&, int scene, int pattern);
void forwardPatternChanges(const core::ChangeSet&, const core::model::Project&, engine::EngineBridge&);
// Every pattern + seq.swing / seq.padStartsSequence (ProjectSettings) + seq.offset of every track of every scene.
void forwardAllSequencer(const core::model::Project&, engine::EngineBridge&,
                         const core::ParamRegistry& = core::builtinParams());
// SWING as stored in the model (ProjectSettings::swing 50–75 %; our choice) ↔ the ParamSpec's 0–50 % (SEQ-004).
constexpr float swingShownFromStored(int stored) noexcept
{
    return static_cast<float>((stored < 50 ? 50 : (stored > 75 ? 75 : stored)) - 50) * 2.0f;
}
constexpr int swingStoredFromShown(float shown) noexcept
{
    const float s = shown < 0 ? 0 : (shown > 50 ? 50 : shown);
    return 50 + static_cast<int>(s * 0.5f + 0.5f);
}
} // namespace ui::input::steps
