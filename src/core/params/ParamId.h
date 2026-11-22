// The real-time handle of a parameter (UID-200): a dense index into the ParamRegistry, assigned at registration in a
// fixed order. The only part of core/params the engine may include: engine messages
// (engine::ParamChange) carry a ParamId, never a string. ParamId values are never written to files — the string id
// (ParamSpec::id, or its StorageKey) is the persistent key.
#pragma once

#include <cstdint>

namespace core
{
struct ParamId
{
    std::uint16_t value = 0xffff;
    constexpr bool valid() const noexcept { return value != 0xffff; }
    friend constexpr bool operator== (ParamId, ParamId) = default;
};
} // namespace core
