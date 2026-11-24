#include "engine/fx/effects/Plugin.h"

#include "engine/EngineApi.h"
#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"

#include <algorithm>

namespace engine::fx::effects
{
void Plugin::process(float* left, float* right, const ProcessContext& ctx) noexcept
{
    if (node)
        node->processEffect({.sampleRate = ctx.sampleRate,
                             .bpm = ctx.bpm,
                             .ppqStart = ctx.ppqStart,
                             .frames = kBlockFrames,
                             .playing = ctx.transportRunning},
                            left, right);
    else if (send)
    {
        std::fill_n(left, kBlockFrames, 0.0f);
        std::fill_n(right, kBlockFrames, 0.0f);
    }
}
} // namespace engine::fx::effects

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "plugin",
    .name = "PLUGIN",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"", "", "", "", "", "fx.plugin.mix"},
    .options = {},
    .visual = "slotLevels",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5),
              .wetHighPass = false,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::effects::Plugin>}))
