// How the shell reads and writes the values the knob strip shows (UID-160, UID-166, UID-201). The interface hides the
// document model: LocalParamValues keeps the values in memory (defaults from the ParamSpecs) and forwards every
// change to the engine bridge; the composition root plugs a model-backed implementation (commands, undo, coalescing,
// ModelParamAccess) into the same interface.
// scope = engine::ParamChange::scope: (scene << 4) | track for track parameters, the scene for scene parameters, 0
// else.
#pragma once

#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"

#include <cstdint>
#include <vector>

namespace ui
{
class ParamAccess
{
public:
    virtual ~ParamAccess() = default;
    virtual float get(const core::ParamSpec&, std::uint16_t scope) const = 0;
    virtual void set(const core::ParamSpec&, std::uint16_t scope, float value) = 0;
    // Step locks (UID-166, SEQ-022): the value locked on step 1–16 of the track in `scope`, if any.
    virtual bool lockOf(const core::ParamSpec&, std::uint16_t scope, int step, float& value) const = 0;
    virtual void setLock(const core::ParamSpec&, std::uint16_t scope, int step, float value) = 0;
};

class LocalParamValues final : public ParamAccess
{
public:
    // Pre-allocates every slot (256 track scopes, 16 scenes, 1 project/global value per parameter) and the lock table,
    // so get/set never allocate. `bridge` may be null.
    LocalParamValues(const core::ParamRegistry&, engine::EngineBridge* bridge = nullptr);
    float get(const core::ParamSpec&, std::uint16_t scope) const override;
    void set(const core::ParamSpec&, std::uint16_t scope, float value) override;
    bool lockOf(const core::ParamSpec&, std::uint16_t scope, int step, float& value) const override;
    void setLock(const core::ParamSpec&, std::uint16_t scope, int step, float value) override;

private:
    struct Lock
    {
        const core::ParamSpec* spec;
        std::uint16_t scope;
        std::uint8_t step;
        float value;
    };
    std::size_t indexOf(const core::ParamSpec&) const noexcept;
    static std::size_t slots(const core::ParamSpec&) noexcept;
    const core::ParamRegistry& params_;
    engine::EngineBridge* bridge_;
    std::vector<std::size_t> offset_;
    std::vector<float> values_;
    std::vector<Lock> locks_;
};
} // namespace ui
