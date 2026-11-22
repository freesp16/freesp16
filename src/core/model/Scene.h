// One scene: 16 tracks, 16 patterns, Send and Master FX, routing (data-model.md "Scene"; SCN-*, MIX-030, OQ-12).
#pragma once

#include "core/model/EffectSlot.h"
#include "core/model/Extras.h"
#include "core/model/Pattern.h"
#include "core/model/Track.h"

#include <array>
#include <optional>
#include <string>

namespace core::model
{
struct SceneRouting
{
    AssignMode assignMode = AssignMode::MasterPlus6;
    int headphoneSource = 0; // HP CUE: 0 = master, else a pair/channel
    FxMode fxMode = FxMode::Both;
    int afsAssignment = -1;  // track assigned to the analog filter slot, -1 = none
    Extras extras {};
    friend bool operator== (const SceneRouting&, const SceneRouting&) = default;
};

struct Scene
{
    std::string name {};
    int colour = 0;
    std::optional<double> bpm {}; // absent = follow the project tempo (AFS-007 / SceneOverrideElseProject)
    std::array<Track, kTracks> tracks;       // no {}: GCC 11 expands it element-wise
    std::array<Pattern, kPatterns> patterns; // no {}: see tracks
    EffectSlot sendFx {};
    EffectSlot masterFx {};
    SceneRouting routing {};
    Extras extras {};
    friend bool operator== (const Scene&, const Scene&) = default;
};
} // namespace core::model
