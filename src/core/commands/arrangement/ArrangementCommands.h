// Arrangement row commands (spec 02-features/arrangement.md ARR-001…004, ARR-011/012; 03-ui/screens.md
// ARRANGER UI-300…306; state-and-undo.md EDT-U1/U2, EDT-007). The project's one arrangement
// (core::model::Project::arrangement, ≤ 999 rows {scene, pattern, repeats 1–99, fixed BPM or "--"}) is edited only
// through these commands on the CommandBus: each is one undo step whose inverse is the row-level opposite (insert ↔
// delete, move ↔ move back, edit ↔ the old row), so an undo never copies the 999 rows. Knob edits of one row field
// coalesce (EDT-U2). Message thread only.
//
// Rows are 0-based; the ARRANGER's END OF ARRANGEMENT marker is row index rowCount() (a valid target for "insert
// before" = append). A factory returns null for an edit that would change nothing or is invalid (index out of range,
// 999 rows already, nothing on the clipboard), so the screen can grey the control (UI-304).
//
// ChangeSet paths: "arrangement.sections" for every row edit (ArrangementPlanBuilder.h rebuilds the engine's plan) and
// "arrangement.play" for song mode (ProjectSettings::playArrangement, ARR-004).
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"

#include <memory>
#include <optional>
#include <string_view>

namespace core::commands::arrangement
{
inline constexpr int kMaxRows = model::Arrangement::kMaxSections; // ARR-001
inline constexpr int kMaxRepeats = 99;                             // ARR-002
inline constexpr std::string_view kSectionsPath = "arrangement.sections";
inline constexpr std::string_view kPlayPath = "arrangement.play";

// The knob columns of the ARRANGER (ARR-011): 1 SCENE · 2 PATTERN · 3 REPEAT · 4 BPM (arrangement.row.* ParamSpecs).
enum class RowField : std::uint8_t { Scene, Pattern, Repeat, Bpm };

int rowCount(const model::Project&) noexcept;
// A row as the commands store it: scene / pattern 0–15, repeats 1–99, a BPM below 20 = "--", else 20–300 on 0.1.
model::Section normalised(model::Section) noexcept;
// UI-302 / UI-303: the current scene / pattern, REPEAT 1, BPM "--".
model::Section currentRow(const model::Project&);
// UI-301: the value knob `field` shows for `row` (END OF ARRANGEMENT or out of range: the ParamSpec default — scene /
// pattern 0, repeat 1, bpm 0 = "--"). Units as the arrangement.row.* ParamSpecs.
float rowValue(const model::Project&, int row, RowField) noexcept;

std::unique_ptr<Command> insertRowBefore(const model::Project&, int row, model::Section);  // row ∈ [0, count]
std::unique_ptr<Command> insertCurrentBefore(const model::Project&, int row);              // selector push (UI-303)
std::unique_ptr<Command> appendRow(const model::Project&, model::Section);                 // knobs on END (UI-302)
std::unique_ptr<Command> appendCurrent(const model::Project&);
std::unique_ptr<Command> editRow(const model::Project&, int row, model::Section);
// A knob turned on the focused row: field = value (ParamSpec units, clamped); consecutive turns of one row's field
// merge into one undo step (EDT-U2).
std::unique_ptr<Command> editRowField(const model::Project&, int row, RowField, double value);
std::unique_ptr<Command> deleteRow(const model::Project&, int row);                         // DELETE (UI-304)
std::unique_ptr<Command> moveRow(const model::Project&, int from, int to);                  // SHIFT + turn
std::unique_ptr<Command> duplicateRow(const model::Project&, int row);                      // SHIFT + COPY
std::unique_ptr<Command> clearArrangement(const model::Project&);
std::unique_ptr<Command> setPlayArrangement(const model::Project&, bool on);                 // ARRANGEMENT (UI-300)

// ARR-012 / UI-304: COPY copies the focused row's settings; PASTE always inserts the copy before the focused row (never
// overwrites). The clipboard belongs to the screen (one per document); it survives project changes.
class RowClipboard
{
public:
    bool copy(const model::Project&, int row); // false (clipboard unchanged) for END OF ARRANGEMENT
    bool hasRow() const noexcept { return row_.has_value(); }
    const std::optional<model::Section>& row() const noexcept { return row_; }
    std::unique_ptr<Command> pasteBefore(const model::Project&, int row) const;
    void clear() noexcept { row_.reset(); }

private:
    std::optional<model::Section> row_;
};
} // namespace core::commands::arrangement
