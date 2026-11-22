// The importer's editable tables (spec sp16-interop.md "Step matrix" OQ-01, "Sample paths") and its
// report (FMT-042).
//
// ParamIndexMap: reference step-lock ordinal 0–22 → this app's lockable ParamSpec id, with the ordinal's value class.
// The classes are the research's confirmed code paths (docs/project-format.md "Value classes": ordinals 5–7 0–127 as
// k/127, ordinal 8 0–127 or a 12-step index, 0 / 4 / 9–22 raw integers, 1–3 frames split base 1,411,200 with the high
// part in slots 16 / 17 / 18); the parameter behind each ordinal is unknown (OQ-01): the built-in table is this app's
// own ParamIndex order (sequencer.md "Lockable parameters"), every entry flagged a guess. FX slots 23–28 / 29–34 are
// FX1 / FX2 internal parameters 0–5 as 0–1 fractions (value / 2097151).
// A user copy (JSON, same shape as services/interop/sp16/param-index-map.json, which `printParamIndexMap` reproduces)
// replaces the built-in table when setUserMapFile() names an existing file; a broken file is reported and ignored.
//
// Path aliases: `audioSourceUrl` = "$Int0/%Tmp/X.wav": "$Int0" is the chosen disk root, "%<alias>" a folder alias
// resolved against candidate sub-folders of that root (unknown in the research: our candidates, editable in the
// same JSON); a file not found there is looked up by name below the root (reported as matched by name).
#pragma once

#include "core/util/Result.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace juce
{
class File;
}

namespace services::interop::sp16
{
using ::core::Result;

enum class ValueClass : std::uint8_t
{
    Raw,         // the 22-bit signed integer is the parameter's value in its own units
    Norm,        // value / 2097151 = 0–1 over the parameter's range (k/127 for the 0–127 ordinals)
    Frames,      // low part here + high part × 1,411,200 in highSlot
    NormOrIndex, // ordinal 8: a multiple of 16513 is a 0–127 value, a smaller value a 12-step index
};
std::string_view className(ValueClass) noexcept;

struct OrdinalMap
{
    int ordinal = 0;
    std::string param;    // lockable ParamSpec id ("" = not imported: locks reported, kept raw)
    ValueClass cls = ValueClass::Raw;
    int highSlot = -1;    // Frames: the companion slot holding the high part
    double scale = 1.0;   // Raw: parameter value = stored integer × scale
    bool guess = true;
    std::string basis = "guess"; // "research" (class + parameter documented), "observed" (value ranges seen), "guess"
};

struct ParamIndexMap
{
    int version = 1;
    std::array<OrdinalMap, 23> ordinals {};
    // Folder alias → candidate sub-folders of the disk root, tried in order ("" = the root itself).
    std::map<std::string, std::vector<std::string>> pathAliases;
    // Volume token → sub-folder of the disk root ("" = the root itself). Unknown volumes resolve to the root, reported.
    std::map<std::string, std::string> volumes;
    // PatternData.Steps value2: "ignore" (default) — a real project holds only 0–63 there, low values most common,
    // which is no 0–127 velocity: the import plays every step at full velocity and keeps the stored pair per cell;
    // "velocity" reads it as the step velocity (1–127; 0 → full).
    bool stepValue2AsVelocity = false;
    std::string source = "built-in"; // or the user file's path

    static const ParamIndexMap& builtin();
    static Result<ParamIndexMap> parse(std::string_view json); // errors name the entry
    std::string toJson() const;
    int guesses() const noexcept;
    std::string basisSummary() const; // "4 observed, 3 research, 1 not imported, …"
};

// The table an import uses: the user file when set and valid, else the built-in one (`note` says which / why).
void setUserMapFile(const juce::File&);
ParamIndexMap activeParamIndexMap(std::string& note);

// FMT-042: summary lines in order plus aggregated issues ("<kind> <key> x<count> (first: <where>)").
class ImportLog
{
public:
    enum class Kind : std::uint8_t
    {
        EnumUnmapped,    // a stored enum string no table knows (kept in Extras, UI-501)
        OrdinalUnmapped, // locks of an ordinal the map does not import (kept raw)
        Opaque,          // a blob / array kept verbatim (grids, arrangement, non-canonical step matrix)
        SampleMissing,   // a sample that was not found
        ModelLacks,      // a documented field the model has no place for (kept in Extras)
        UnknownNode,     // a node the importer does not know (kept as a subtree)
        UnknownProp,     // a property the importer does not know (kept in Extras)
        TypeMismatch,    // a property of an unexpected var type (kept in Extras)
        Clamped,         // a value outside the model's range (clamped; the original kept)
        Guessed,         // a mapping that rests on a guess (field name or value meaning)
        Structure,       // missing / duplicate / misplaced nodes
    };
    static std::string_view kindName(Kind) noexcept;

    void line(std::string text) { lines_.push_back(std::move(text)); }
    void issue(Kind, std::string key, std::string_view where);
    std::size_t count(Kind) const noexcept;
    std::size_t distinct(Kind) const noexcept;
    bool has(Kind k, std::string_view keyPrefix) const noexcept;
    std::vector<std::string> lines() const; // summary lines, then one per issue key
    const std::vector<std::string>& summary() const noexcept { return lines_; }

private:
    struct Issue
    {
        Kind kind;
        std::string key, first;
        std::size_t count = 0;
    };
    std::vector<std::string> lines_;
    std::vector<Issue> issues_;
    std::map<std::pair<int, std::string>, std::size_t> index_;
};
} // namespace services::interop::sp16
