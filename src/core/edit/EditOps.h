// The model surgery behind the SceneEditor (editing-and-undo.md EDT-001…006; state-and-undo.md
// "ScenePatternEdit"). Message thread unless noted; the CellSet / copy helpers are pure functions on values and also
// run on a worker (they touch only their arguments).
//
// Undo by exchange (EDT-U1): an edit is an EditCommand whose first apply() builds the new
// sub-trees (Scene / Pattern / Track slots) — moved in from a value prepared on a worker, or merged from the model —
// and *exchanges* them with the model's slots. The old sub-trees end up in a shared holder that the inverse
// (ExchangeBack) owns; undo exchanges again, so undo / redo are exact (bit-identical) and never copy the sub-tree on
// the message thread. The holder's deep size is what the UndoStack charges against its 256 MB cap.
//
// Lock records (Pattern::locks, referenced by Step::lockIndex): moving cells between patterns appends their records
// to the target and compacts it (records ordered by first use, unreferenced ones dropped).
#pragma once

#include "core/commands/Command.h"
#include "core/edit/EditRef.h"
#include "core/model/Project.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace core::edit
{
// ---- cells ----
// A set of cells of one pattern with their lock records (Cell::value.lockIndex indexes `locks`), plus the pattern
// mute/solo flags of the tracks in `flagTracks` (Sequence, TrackWithSequences: the track's state in that pattern).
struct CellSet
{
    struct Cell
    {
        std::uint8_t step, track;
        model::Step value;
    };
    std::vector<Cell> cells;
    std::vector<model::StepLocks> locks;
    std::uint16_t flagTracks = 0;
    std::array<bool, model::kTracks> mutes {}, solos {};
    std::uint64_t stepMask = 0; // the source steps (Steps / Page: placement)
    friend bool operator== (const CellSet&, const CellSet&) = default;
};

// The cells of the tracks in `trackMask` at the steps in `stepMask` (every cell, empty ones too: a paste overwrites).
CellSet extractCells(const model::Pattern&, std::uint16_t trackMask, std::uint64_t stepMask, bool withFlags);
// Writes `cells` into `dst`: cell (step s, track t) goes to (placeStep(s), trackTo < 0 ? t : trackTo); flags likewise.
// placeStep returning < 0 skips the cell. Compacts dst's lock records.
void writeCells(model::Pattern& dst, const CellSet& cells, const std::function<int(int)>& placeStep, int trackTo = -1);
// Empties the cells (and drops the flags when `flags`) of the tracks/steps given; compacts the lock records.
void clearCells(model::Pattern&, std::uint16_t trackMask, std::uint64_t stepMask, bool flags);
// Exchanges the columns (cells, flags) of tracks a and b inside one pattern (lock indices stay valid).
void swapColumns(model::Pattern&, int a, int b) noexcept;
// Drops unreferenced lock records, orders them by first use (step-major), clears out-of-range lockIndex values.
void compactLocks(model::Pattern&);

// EDT-005/006 placement of a Steps / Page paste: source step i goes to dstFirst + (i − srcFirst), wrapping at the
// target pattern length when the copied span fits in it (EDT-005), else kept at that matrix position (mod 64): steps
// beyond the length stay in the matrix and are not played (EDT-006).
int placeStep(int step, std::uint64_t srcMask, int dstFirst, int dstLength) noexcept;
int lowestStep(std::uint64_t mask) noexcept;  // 0 for an empty mask
int highestStep(std::uint64_t mask) noexcept; // 0 for an empty mask

// ---- sizes (UndoStack cap, EDT-U1) ----
std::size_t deepBytes(const model::Pattern&) noexcept;
std::size_t deepBytes(const model::Track&) noexcept;
std::size_t deepBytes(const model::Scene&) noexcept;

// ---- change paths ----
// The paths an edit reports (observers + EngineBridge): a pattern slot is "scene[s].pattern[p]"; a track slot expands
// into the per-field paths of every registered model parameter of that scope ("scene[s].track[t].volume",
// "scene[s].track[t].insertFx[0]", …), so the existing forwarders re-post the new values and the engine hears them at
// the next block (EDT-004); a scene slot = its scene-level fields ("scene[s].bpm", ".sendFx", ".masterFx",
// ".routing.fxMode", …) + its patterns + its tracks. No bare "scene[s].track[t]" (the sequencer's sync reads it
// as "16 patterns changed"). Subtrees reports only the patterns / tracks whose value the exchange changed, so a scene
// pasted onto a similar one re-posts and rebuilds only what differs.
void addTrackPaths(ChangeSet&, int scene, int track);
void addPatternPath(ChangeSet&, int scene, int pattern);
void addSceneFieldPaths(ChangeSet&, int scene); // the scene-level fields only
void addScenePaths(ChangeSet&, int scene, std::uint16_t patterns = 0xFFFF, std::uint16_t tracks = 0xFFFF);

// ---- undo by exchange ----
class Subtrees
{
public:
    void addScene(int scene, std::unique_ptr<model::Scene>);
    void addPattern(int scene, int pattern, std::unique_ptr<model::Pattern>);
    void addTrack(int scene, int track, std::unique_ptr<model::Track>);
    // Exchanges every held value with its model slot (the model gets the held value, the holder the old one).
    void exchange(model::Project&) noexcept;
    std::size_t bytes() const noexcept;
    void collect(ChangeSet&) const;
    bool empty() const noexcept { return scenes_.empty() && patterns_.empty() && tracks_.empty(); }

private:
    // `changed…`: what the last exchange changed (the paths collect() reports), computed by exchange().
    struct SceneSlot
    {
        int scene;
        std::unique_ptr<model::Scene> value;
        std::uint16_t changedPatterns = 0xFFFF, changedTracks = 0xFFFF;
    };
    struct PatternSlot { int scene, pattern; std::unique_ptr<model::Pattern> value; bool changed = true; };
    struct TrackSlot { int scene, track; std::unique_ptr<model::Track> value; bool changed = true; };
    std::vector<SceneSlot> scenes_;
    std::vector<PatternSlot> patterns_;
    std::vector<TrackSlot> tracks_;
};

// An undoable sub-tree edit. `build(model, next)` runs once, at the first apply, and fills `next` with the new values
// (from a prepared value, or computed from the model); apply() then exchanges them in. inverse() (taken just before
// apply by the bus) shares the holder that apply() fills with the old values.
class EditCommand final : public Command
{
public:
    using Build = std::function<void(const model::Project&, Subtrees& next)>;
    EditCommand(std::string_view label, Build build);

    std::string_view label() const override { return label_; }
    void apply(model::Project&) override;
    std::unique_ptr<Command> inverse(const model::Project&) const override;
    void collectChanges(ChangeSet& c) const override { holder_->collect(c); }
    std::size_t sizeBytes() const override { return sizeof(*this) + holder_->bytes(); }

private:
    std::string_view label_;
    Build build_;
    std::shared_ptr<Subtrees> holder_;
};

// Swaps two tracks of one scene, with or without their columns in the 16 patterns (EDT-002); its own inverse.
class SwapTracksCommand final : public Command
{
public:
    SwapTracksCommand(int scene, int a, int b, bool withSequences) noexcept
        : scene_(scene & 15), a_(a & 15), b_(b & 15), withSequences_(withSequences)
    {
    }
    std::string_view label() const override { return "SWAP TRACK"; }
    void apply(model::Project&) override;
    std::unique_ptr<Command> inverse(const model::Project&) const override
    {
        return std::make_unique<SwapTracksCommand>(scene_, a_, b_, withSequences_);
    }
    void collectChanges(ChangeSet&) const override;

private:
    int scene_, a_, b_;
    bool withSequences_;
};
} // namespace core::edit
