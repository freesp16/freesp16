// Model paths (data-model.md "Identity and paths"): every node is addressable as a stable string, e.g.
// "scene[3].track[7].insertFx[1].params[2]", used by commands, the undo stack, MIDI learn and p-locks. The engine
// sees only the ParamId + scope indices (engine::ParamChange). ParamPath.cpp implements parsePath/formatPath.
#pragma once

#include "core/params/ParamId.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace core
{
class ParamRegistry;
}

namespace core::model
{
enum class Scope : std::uint8_t { Project, Scene, Track, Pattern, Step, InsertFx, SendFx, MasterFx };
struct ParamPath { Scope scope; std::uint8_t scene = 0, track = 0, pattern = 0, step = 0, slot = 0; ParamId param; };
std::optional<ParamPath> parsePath(std::string_view text, const ParamRegistry&);   // "scene[3].track[7].volume"
std::string formatPath(const ParamPath&, const ParamRegistry&);

// engine::ParamChange::scope for a track/step parameter: (scene << 4) | track; for a scene parameter: the scene;
// 0 for a project parameter.
constexpr std::uint16_t engineScope(const ParamPath& p) noexcept
{
    switch (p.scope)
    {
        case Scope::Track:
        case Scope::Step:
        case Scope::InsertFx: return static_cast<std::uint16_t>((p.scene << 4) | p.track);
        case Scope::Scene:
        case Scope::Pattern:
        case Scope::SendFx:
        case Scope::MasterFx: return p.scene;
        case Scope::Project: break;
    }
    return 0;
}
} // namespace core::model
