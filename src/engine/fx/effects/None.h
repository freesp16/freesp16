// None: the identity effect (wet = input). It measures the shell alone (bench_fx_shell: no effect cost, the mix kernel
// at dry 1 − v / wet v) and is the minimal effect of the contract and processor tests. Registered as "none" behind
// the feature flag "fx.test" (never listed: the slot's Off entry is the processor's type −1, not this effect), every
// slot kind, shell policy {Open gate, CrossfadeK5 mix, no wet HPF}; parameter k5 = Mix ("fx.none.mix").
#pragma once

#include "engine/fx/EffectBase.h"

namespace engine::fx::effects
{
class None final : public EffectBase
{
public:
    void prepare(double, int) override {}
    void reset() noexcept override {}
    void setParam(int, float) noexcept override {}
    void process(float*, float*, const ProcessContext&) noexcept override {}
};
} // namespace engine::fx::effects
