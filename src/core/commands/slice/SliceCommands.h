// Slice editor commands (spec 02-features/slicing.md SLC-001/002/003/006, 07-ui-design/screens/
// playback.md "SET SLICE"; state-and-undo.md). Each edit replaces the track's SliceSettings (scene s, track t) as one
// undo step (EDT-U1) whose inverse restores the previous settings (≤ 1 kB); POSITION detents on one slice and AREA
// turns coalesce (EDT-U2). The ChangeSet path is "scene[s].track[t].slices" (kSlicesPath); the app rebuilds and posts
// the engine's slice table from it (core/util/slice/SliceTable.h). No command ever touches the sample (SLC-010).
//
// `frames` is the track sample's frame count (the grid spans the whole sample, SLC-001). Slice indices are 0-based
// positions in the sorted point list (core/commands/slice/SlicePoints.h); a factory returns null for an edit that
// would change nothing or is not allowed (DELETE on slice 1 or on a grid point, ADD on a full grid or on an existing
// point, no sample).
//
//   setCount      GRID SLICE (SLC-001): N equal slices, N ∈ {2, 4, 8, 16, 32, 64}; discards moved / added points
//   movePoint     POSITION (SLC-002/006): the slice's start to `frame`, kept strictly between its neighbours
//   nudgePoint    POSITION by encoder detents: coarse = frames / 128 per detent (PLY-002), SHIFT (fine) = 1 frame
//   addPoint      ADD SLICE: a point at `frame` (the playhead or the current slice's mid-point, the UI chooses)
//   deletePoint   DELETE SLICE: removes an added point (slice > 1); grid points are moved, not deleted (UID-341)
//   setArea       AREA (SLC-003): which 16 slices the pads play (windowOffset), clamped to the grid
#pragma once

#include "core/commands/Command.h"
#include "core/commands/slice/SlicePoints.h"
#include "core/model/Project.h"

#include <memory>
#include <string>
#include <string_view>

namespace core::commands::slice
{
// "scene[s].track[t].slices"
std::string slicesPath(int scene, int track);
// The scene / track of a slices path, false for any other path.
bool parseSlicesPath(std::string_view path, int& scene, int& track) noexcept;

const model::SliceSettings& settings(const model::Project&, int scene, int track) noexcept;

std::unique_ptr<Command> setCount(const model::Project&, int scene, int track, int count);
std::unique_ptr<Command> movePoint(const model::Project&, int scene, int track, int slice, std::int64_t frame,
                                   std::int64_t frames);
std::unique_ptr<Command> nudgePoint(const model::Project&, int scene, int track, int slice, int detents, bool fine,
                                    std::int64_t frames);
std::unique_ptr<Command> addPoint(const model::Project&, int scene, int track, std::int64_t frame, std::int64_t frames);
std::unique_ptr<Command> deletePoint(const model::Project&, int scene, int track, int slice, std::int64_t frames);
std::unique_ptr<Command> setArea(const model::Project&, int scene, int track, int area, std::int64_t frames);
// Replaces the whole settings (the inverse of every edit above).
std::unique_ptr<Command> replaceSlices(int scene, int track, model::SliceSettings, std::string_view label = "SLICE");

// Rule checks the SET SLICE rail uses to draw ADD / DELETE disabled in place (UID-341).
bool canAdd(const model::SliceSettings&, std::int64_t frames) noexcept;
bool canDelete(const model::SliceSettings&, int slice, std::int64_t frames) noexcept;
// ADD SLICE's frame (ours): the playhead when it lies inside a slice away from its points, else the current slice's
// mid-point.
std::int64_t addFrame(const model::SliceSettings&, int slice, std::int64_t playhead, std::int64_t frames) noexcept;
} // namespace core::commands::slice
