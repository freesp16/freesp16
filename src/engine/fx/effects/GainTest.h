// GainTest: the trivial engine::fx::EffectBase that proves the golden harness and the framework end to end
// (moved here from tests/golden/harness/GainTest.h, which now aliases it). Its golden
// reference is the own Python model tools/golden-gen/goldenlib/models/gain_test.py; both must stay operation for
// operation identical:
//
//   parameter 0 = gain index: index = int(value01 · 127 + 0.5) (float32), clamped 0…127 → core::GainLaw::gain(index);
//   default index 100 (unity). A new target is reached by a linear ramp over one 64-frame block:
//     step = (target − cur) · (1/64); per frame: cur += step; L *= cur; R *= cur; after the block cur = target.
//   Otherwise L *= cur; R *= cur.
//   reset() jumps to the target (no ramp); setOption(kClearState, 1) returns to the freshly prepared state.
//   Own option 0 (kBrokenGain, a test hook) scales every gain by 1 + 2^-10: the golden harness's deliberately broken
//   gain (tests/golden/harness/Subject.h kFaultGainScale).
//
// Registered as "gainTest" behind the feature flag "fx.test" (never listed in SELECT FX), every slot kind, shell
// policy {} (gate open, no wet HPF, wet only). No mul-add pattern exists here, so contraction cannot change a result.
#pragma once

#include "core/util/GainLaw.h"
#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"

namespace engine::fx::effects
{
class GainTest final : public EffectBase
{
public:
    enum Option : int
    {
        kBrokenGain = 0
    };
    static constexpr int kDefaultIndex = 100;
    static constexpr float kBrokenScale = 1.0f + 1.0f / 1024.0f;

    void prepare(double, int) override { clear(); }
    void reset() noexcept override { cur_ = target_; }
    void setParam(int index, float value01) noexcept override
    {
        if (index != 0)
            return;
        int i = static_cast<int>(value01 * 127.0f + 0.5f);
        index_ = i < 0 ? 0 : (i > 127 ? 127 : i);
        target_ = gainFor(index_);
    }
    void setOption(int which, int value) noexcept override
    {
        if (which == kBrokenGain)
        {
            broken_ = value != 0;
            target_ = gainFor(index_);
        }
        else if (which == kClearState && value != 0)
            clear();
    }
    void process(float* left, float* right, const ProcessContext&) noexcept override
    {
        constexpr int n = kBlockFrames;
        if (cur_ != target_)
        {
            const float step = (target_ - cur_) * (1.0f / 64.0f);
            float g = cur_;
            for (int i = 0; i < n; ++i)
            {
                g += step;
                left[i] *= g;
                right[i] *= g;
            }
            cur_ = target_;
            return;
        }
        const float g = cur_;
        for (int i = 0; i < n; ++i)
        {
            left[i] *= g;
            right[i] *= g;
        }
    }
    float telemetry(int which) const noexcept override { return which == 0 ? cur_ : 0.0f; }

private:
    float gainFor(int i) const noexcept
    {
        const float g = core::GainLaw::gain(i);
        return broken_ ? g * kBrokenScale : g;
    }
    void clear() noexcept
    {
        index_ = kDefaultIndex;
        target_ = cur_ = gainFor(index_);
    }
    int index_ = kDefaultIndex;
    float cur_ = 1.0f, target_ = 1.0f;
    bool broken_ = false;
};
} // namespace engine::fx::effects
