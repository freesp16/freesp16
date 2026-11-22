// Typed mapping of a decoded reference project tree to the document model (spec sp16-interop.md "Node mapping",
// data-model.md; research docs/project-format.md "The project tree (confirmed schema)").
//
// Evidence per property name (comments in Sp16Import.cpp): [D] named on the research page, [S] named by the spec's
// data model (Sixteen field = reference field unless the spec gives another), [G] a guess — every [G] name that matches
// is reported (`guessed`). Nothing is dropped silently: a property the importer does not know, a value outside the
// model's range, an enum string no table knows, a lock ordinal the map does not import, a node of an unknown type
// are kept in the entity's Extras ("sp16.<Node>.<prop>", step cells "sp16.step[s].track[t]", whole unknown subtrees
// "sp16.node.<type>" as base64 tree bytes) and listed in the ImportLog (FMT-042). Patterns are placed by their node
// suffix, never by position (OQ-03); absent patterns stay default. A step matrix that the model cannot reproduce
// byte for byte (encodePattern) is also kept verbatim ("sp16.PatternData.Steps") and reported.
//
// Pure: no file access (samples are resolved by the caller's resolver). Worker threads only.
#pragma once

#include "core/model/Project.h"
#include "services/interop/sp16/Sp16Mapping.h"
#include "services/interop/sp16/StepMatrix.h"
#include "services/interop/sp16/ValueTreeCodec.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace services::interop::sp16
{
// Resolves one `audioSourceUrl` to a pool entry (id, path, displayName, missing). Called once per distinct URL.
using SampleResolver = std::function<core::model::SampleRef(const std::string& url)>;

struct MapOptions
{
    const ParamIndexMap* map = nullptr; // null = ParamIndexMap::builtin()
    SampleResolver samples;             // null = every sample missing, id "sp16:<url>"
};

struct MapStats
{
    int scenes = 0, tracks = 0, patterns = 0;
    int triggers = 0, lockCells = 0, locks = 0;
    int verbatimPatterns = 0; // step matrices kept verbatim
    int value2Kept = 0;       // triggered steps whose value2 was not read as a velocity (kept per cell)
    int samples = 0, samplesMissing = 0;
    std::vector<std::string> patternList; // "s.p" per present pattern (1-based)
};

// `out` is reset to the default project first. Fails only when the root is not a ProjectData tree.
Result<void> mapProject(const Node& root, core::model::Project& out, const MapOptions&, ImportLog&,
                        MapStats* stats = nullptr);

// The model → step records encoder (canonical reference form + the cell overrides kept in Pattern::extras). The
// import's round-trip check and the exporter (FMT-044) use it.
std::vector<StepRecord> encodePattern(const core::model::Pattern&, const ParamIndexMap&);
} // namespace services::interop::sp16
