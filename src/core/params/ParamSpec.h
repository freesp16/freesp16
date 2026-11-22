// The parameter catalogue (07-ui-design/05-extensibility.md §1, UID-200/201): every user-editable value is one
// ParamSpec, registered once. Knob-strip captions, p-lock slots, LFO and touch-strip targets, MIDI CCs, undo labels and
// file serialisation keys are all generated from the registry; nothing else may define a caption or a range.
//
// Besides the value description a spec carries `scope` (where a value lives: track, scene, project,
// scene-override-else-project — AFS-007 scene BPM — or global preference, SET) and `storage` (the file key).
// Preferences are ordinary ParamSpecs with scope = Global (stored in the settings file, never in a project).
#pragma once

#include "core/params/ParamId.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace core
{
enum class ParamType : std::uint8_t
{
    Continuous,
    Integer,
    Enum,
    Bool,
    Time,
    Note
};

// Where a parameter's value lives (data-model.md entity tree; engine::ParamChange::scope encodes the indices).
enum class ParamScope : std::uint8_t
{
    Track,                    // per scene and track: scene[s].track[t].<field>; also Insert FX params of that track
    Scene,                    // per scene: scene[s].<field>; also Send/Master FX params of that scene
    Project,                  // project-wide: settings.<field>
    SceneOverrideElseProject, // a scene value that, when absent, follows the project value (AFS-007: scene BPM)
    Global                    // a preference (SET): settings file of the device, never saved in a project
};

// Serialisation key (FMT-013: native files use engineering units under this key). `key` empty = the ParamSpec id.
// `reference` is the field name in the reference project format, for the importer's mapping table;
// empty = no reference field (Sixteen-only parameter).
struct StorageKey
{
    std::string_view key {};
    std::string_view reference {};
    constexpr std::string_view resolve(std::string_view id) const noexcept { return key.empty() ? id : key; }
};

// Formats a value into buf (caller-owned, no allocation); returns the number of chars written.
using ParamFormatter = int (*)(float value, char* buf, int bufSize) noexcept;

struct ParamSpec
{
    std::string_view id;         // stable path, also the p-lock / LFO / MIDI / file key: "playback.pitch"
    std::string_view label;      // knob-strip caption, manual wording: "PITCH"
    std::string_view shortLabel; // <= 8 chars for target lists
    std::string_view group;      // list grouping: "PLAYBACK"
    ParamType type = ParamType::Continuous;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    float coarseStep = 1.0f; // per encoder detent
    float fineStep = 1.0f;   // per detent with SHIFT
    std::span<const std::string_view> enumLabels {}; // Enum: one label per integer value from minValue
    std::string_view unit {};                         // appended in caption style
    ParamFormatter format = nullptr;                  // null: default formatting by type
    bool lockable = false;
    bool lfoTarget = false;
    bool stripTarget = false;
    bool bipolar = false;          // value track fills from the centre
    std::int16_t midiCc = -1;      // fixed reference CC, -1 = none
    std::string_view featureFlag {}; // P3 extensions only
    ParamScope scope = ParamScope::Track; // where the value lives (Global = preference, SET)
    StorageKey storage {};                // file key (empty key = id)

    constexpr std::string_view storageKey() const noexcept { return storage.resolve(id); }
    constexpr bool isPreference() const noexcept { return scope == ParamScope::Global; }

    float clamp(float v) const noexcept { return v < minValue ? minValue : (v > maxValue ? maxValue : v); }
    float normalised(float v) const noexcept
    {
        return maxValue > minValue ? (clamp(v) - minValue) / (maxValue - minValue) : 0.0f;
    }
    // Applies `detents` encoder steps (coarse, or fine with SHIFT) and clamps. Enums and integers stay integral.
    float step(float v, int detents, bool fine) const noexcept;
    int formatValue(float v, char* buf, int bufSize) const noexcept;
};
} // namespace core
