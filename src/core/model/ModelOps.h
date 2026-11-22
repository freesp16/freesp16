// Defaults and validation of the document model (data-model.md "Invariants").
//
// Defaults: a default-constructed entity carries the header defaults; the functions here apply the manual
// defaults the spec names on top (PRJ-002 new project, ENV-011 envelope, SEQ-030 retrigger, RIB-002 pitch range,
// TRK-031/SCN-003 colours, …). A Project is ≈ 2 MB: prefer newDefaultProject() (heap) or resetToDefaults() (in place)
// over makeDefaultProject() (returned by value; the caller's storage must be large enough, never a small thread stack).
//
// validate(): the invariants of data-model.md (fixed capacities hold by construction; every non-empty SampleKey
// resolves to a pool entry, possibly `missing`) plus the value ranges and cross-references a decoder or a command can
// break (enum values in range, lock indices, pool ids unique, …). Used by tests and after every load.
// Message thread or worker; allocates only to report problems.
#pragma once

#include "core/model/Project.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace core::model
{
// New Project (PRJ-002): every track Sample mode and empty, every FX slot Off, tempo 120.0, pattern length 16,
// scene 1 / pattern 1 selected, with the manual defaults applied.
Project makeDefaultProject();
std::unique_ptr<Project> newDefaultProject();
void resetToDefaults(Project&); // in place, no 2 MB temporary

// Defaults of one entity, for commands that clear or initialise part of a project (track/scene indices 0-based).
Track defaultTrack(int trackIndex);
Pattern defaultPattern();
void resetTrack(Track&, int trackIndex);
void resetPattern(Pattern&);
void resetScene(Scene&, int sceneIndex); // tracks, patterns, FX, routing, name, colour and BPM

// Checks `project`; appends one human-readable line per problem ("scene[3].track[7].volume: 200 not in 0…127") to
// `problems` and returns true when there were none. At most `maxProblems` lines are appended, then one summary line.
bool validate(const Project& project, std::vector<std::string>& problems, std::size_t maxProblems = 100);
} // namespace core::model
