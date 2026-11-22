// The undoable ScenePatternEdit commands (editing-and-undo.md EDT-001…006, EDT-009, EDT-012;
// state-and-undo.md "ScenePatternEdit"): copy snapshots, paste preparation, reset and swap. The SceneEditor
// (core/edit/SceneEditor.h) runs the paste preparation on a worker; everything here is also usable synchronously
// (tests, tools). Each command is one undo step and exact: undo restores the previous sub-trees bit-identically
// (EditOps.h "Undo by exchange").
#pragma once

#include "core/commands/Command.h"
#include "core/edit/EditOps.h"
#include "core/edit/EditRef.h"
#include "core/model/Project.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace core::edit
{
// An immutable copy of one edit object, made on the message thread at COPY (EDT-001/010) and shared read-only with the
// worker that prepares a paste.
struct ClipPayload
{
    EditRef source {};
    const void* project = nullptr;            // identity of the source project (sample references, EDT-008)
    std::unique_ptr<model::Scene> scene;      // Scene
    std::unique_ptr<model::Pattern> pattern;  // Pattern
    std::unique_ptr<model::Track> track;      // Track, TrackWithSequences
    std::vector<CellSet> cells;               // TrackWithSequences: 16 (pattern p); Sequence, Steps, Page: 1
};

// Message thread: the value copy of `src` in `project` (the source state at COPY time). Null for an invalid ref.
std::shared_ptr<const ClipPayload> snapshot(const model::Project& project, const EditRef& src);

// Whether `clip` can be pasted onto `dst` (EDT-011: same family; a Track or Scene clip only into the project it was
// copied from, since its sample references are that project's).
bool canPaste(const ClipPayload& clip, const EditRef& dst, const void* dstProject) noexcept;

// Worker-safe (touches only `clip` and `dst`): builds the paste command — the expensive value copy of a whole scene,
// pattern or track happens here; merges into existing patterns (TrackWithSequences, Sequence, Steps, Page) happen at
// apply, against the model as it is then. Null when !canPaste (the project check is the caller's).
std::unique_ptr<Command> preparePaste(std::shared_ptr<const ClipPayload> clip, const EditRef& dst);

// EDT-003 / EDT-012: back to the defaults (Track: defaultTrack; TrackWithSequences: + its column in the 16 patterns;
// Sequence: the column; Pattern: defaultPattern; Scene: resetScene — the slots always exist, so delete = reset;
// Steps / Page: the cells).
std::unique_ptr<Command> makeReset(const EditRef& target);

// EDT-002 (tracks) and the SCENE MANAGER's SHIFT + PASTE (ours): exchanges two objects of one family (Scene, Pattern,
// Track with or without sequences, Sequence). Null for Steps / Page or a family mismatch.
std::unique_ptr<Command> makeSwap(const EditRef& a, const EditRef& b);
} // namespace core::edit
