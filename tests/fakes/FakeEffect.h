// FakeEffect: the simplest engine::fx::EffectBase — a gain (param 0: 0..1 → ×0..×2, default ×1) — for FxShell,
// registry and RT-contract tests without any real effect.
#pragma once

#include "engine/fx/EffectBase.h"

namespace fakes
{
class FakeEffect final : public engine::fx::EffectBase
{
public:
    void prepare(double sampleRate, int maxBlock) override
    {
        sampleRate_ = sampleRate;
        maxBlock_ = maxBlock;
        prepared = true;
    }
    void reset() noexcept override { ++resets; }
    void setParam(int index, float value01) noexcept override
    {
        if (index == 0)
            gain_ = 2.0f * value01;
    }
    void process(float* left, float* right, const engine::fx::ProcessContext&) noexcept override
    {
        for (int i = 0; i < engine::kBlockFrames; ++i)
        {
            left[i] *= gain_;
            right[i] *= gain_;
        }
    }
    float telemetry(int which) const noexcept override { return which == 0 ? gain_ : 0.0f; }

    bool prepared = false;
    int resets = 0;

private:
    double sampleRate_ = 0;
    int maxBlock_ = 0;
    float gain_ = 1.0f;
};
} // namespace fakes
