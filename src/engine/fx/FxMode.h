// FX mode (spec 02-features/effects/README.md FX-008…010, OQ-12: per scene): Both (default) runs the Send and the
// Master FX; Send disables the Master FX; Master disables the Send FX. A disabled slot uses the stop fade
// (FxProcessor::setDisabled, FX-047); re-enabling is a full re-activation. FX-012 (assignable outputs never pass the
// bus slots) is the router's.
#pragma once

#include "core/model/enums/Enums.h"

#include <cstdint>

namespace engine::fx
{
enum class FxMode : std::uint8_t
{
    Both = 0,
    Send = 1,
    Master = 2
};
static_assert(static_cast<int>(FxMode::Send) == static_cast<int>(core::model::FxMode::Send) &&
                  static_cast<int>(FxMode::Master) == static_cast<int>(core::model::FxMode::Master),
              "engine FX mode values follow the model's");

struct FxModeFlags
{
    bool sendDisabled = false, masterDisabled = false;
};

constexpr FxModeFlags flagsFor(FxMode m) noexcept
{
    switch (m)
    {
    case FxMode::Send:
        return {false, true};
    case FxMode::Master:
        return {true, false};
    case FxMode::Both:
        break;
    }
    return {false, false};
}

constexpr FxMode fxModeFromValue(int v) noexcept // ParamChange values; anything else = Both
{
    return v == 1 ? FxMode::Send : (v == 2 ? FxMode::Master : FxMode::Both);
}
} // namespace engine::fx
