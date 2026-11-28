#pragma once

#include "core/audio/PluginNode.h"
#include "engine/fx/EffectBase.h"

namespace engine::fx::effects
{
class Plugin final : public EffectBase
{
public:
    void prepare(double, int) override {}
    void reset() noexcept override
    {
        if (node)
            node->resetEffect();
    }
    bool available() const noexcept override { return node != nullptr; }
    int latencySamples() const noexcept override { return node ? node->effectLatency() : 0; }
    void setParam(int, float) noexcept override {}
    void process(float* left, float* right, const ProcessContext& ctx) noexcept override;

    core::audio::PluginAudioNode* node = nullptr; // audio thread, owned by the engine swap table
    bool send = false;
};
} // namespace engine::fx::effects
