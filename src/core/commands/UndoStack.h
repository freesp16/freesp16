// The undo history (01-architecture/state-and-undo.md): two stacks of small inverse commands — never model snapshots —
// with a memory cap (EDT-U1), coalescing (EDT-U2) and a save checkpoint for the dirty state
// (EDT-007, "Dirty state and checkpoints"). Message thread only; the CommandBus is its only writer.
//
// Undo is "execute the inverse" (EDT-U4): undo()/redo() hand the entry to a runner (the bus applies it, notifies,
// bridges, and returns the inverse of what it just did), and the runner's result becomes the opposite stack's entry.
//
// Dirty tracking uses state ids: every recorded edit produces a new id, an entry remembers the id it restores, and
// checkpoint() remembers the current id. dirty() is "current id != checkpoint id", so undoing past a save marks the
// document dirty again and redoing back to it marks it clean — eviction and redo truncation need no index arithmetic.
#pragma once

#include "core/commands/Command.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string_view>

namespace core
{
class UndoStack
{
public:
    static constexpr std::size_t kDefaultCapBytes = std::size_t{256} << 20; // EDT-U1: 256 MB of inverse data
    static constexpr double kCoalesceWindowMs = 1000.0;                     // EDT-U2: same path within 1 s

    // Applies `cmd` to the model (and whatever else the owner does on execute) and returns its inverse, or nullptr
    // when it has none (the entry is then consumed without a counterpart on the other stack).
    using Runner = std::function<std::unique_ptr<Command>(Command& cmd)>;

    explicit UndoStack(std::size_t capBytes = kDefaultCapBytes) : cap_(capBytes) {}

    // Records the inverse of a new edit: clears the redo stack, then evicts the oldest entries beyond the cap (the
    // newest entry is always kept, even when it alone exceeds the cap).
    void push(std::unique_ptr<Command> inverse, std::size_t bytes);
    // As push(), but first tries to coalesce (EDT-U2): when the top entry is not sealed, was recorded or last merged
    // at most kCoalesceWindowMs ago, and top->mergeWith(*inverse, nowMs) accepts, the new inverse is dropped (the
    // older one already restores the state before both edits). Returns true when it merged.
    bool pushOrMerge(std::unique_ptr<Command> inverse, std::size_t bytes, double nowMs);

    const Command* nextUndo() const noexcept { return undo_.empty() ? nullptr : undo_.back().cmd.get(); }
    const Command* nextRedo() const noexcept { return redo_.empty() ? nullptr : redo_.back().cmd.get(); }
    bool undo(const Runner& run); // false when there is nothing to undo
    bool redo(const Runner& run); // false when there is nothing to redo

    bool canUndo() const noexcept { return ! undo_.empty(); }
    bool canRedo() const noexcept { return ! redo_.empty(); }
    std::string_view undoLabel() const noexcept; // "" when empty
    std::string_view redoLabel() const noexcept;
    std::size_t undoDepth() const noexcept { return undo_.size(); }
    std::size_t redoDepth() const noexcept { return redo_.size(); }

    void clear();               // drops both stacks (project open, EDT-U1); dirty() unchanged
    void clearRedo();           // a new edit that bypasses push() (an open CommandGroup)
    void checkpoint() noexcept; // on save: the current state is clean; seals the top entry
    bool dirty() const noexcept { return state_ != clean_; }
    void seal() noexcept; // the top entry no longer coalesces with the next edit

    std::size_t bytes() const noexcept { return bytes_; }
    std::size_t capBytes() const noexcept { return cap_; }
    void setCapBytes(std::size_t cap); // evicts immediately when lowered
    std::uint64_t evicted() const noexcept { return evicted_; }

private:
    struct Entry
    {
        std::unique_ptr<Command> cmd;
        std::size_t bytes = 0;
        std::uint64_t restores = 0; // the state id the model is in after executing cmd
        double lastMs = 0;          // recorded or last merged (EDT-U2 window)
        bool sealed = false;        // never coalesces again (undo/redo/checkpoint boundary)
    };

    void pushEntry(std::deque<Entry>& stack, std::unique_ptr<Command> cmd, std::size_t bytes, std::uint64_t restores,
                   double nowMs, bool sealed);
    bool transfer(std::deque<Entry>& from, std::deque<Entry>& to, const Runner& run);
    void dropAll(std::deque<Entry>& stack) noexcept;
    void enforceCap(bool newestOnUndo) noexcept;

    std::deque<Entry> undo_, redo_; // back() = next to undo / redo
    std::size_t cap_;
    std::size_t bytes_ = 0;
    std::uint64_t state_ = 0, clean_ = 0, nextState_ = 1;
    std::uint64_t evicted_ = 0;
};
} // namespace core
