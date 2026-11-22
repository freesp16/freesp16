#include "core/commands/UndoStack.h"

#include <limits>
#include <utility>

namespace core
{
void UndoStack::push(std::unique_ptr<Command> inverse, std::size_t bytes)
{
    if (! inverse)
        return;
    clearRedo();
    // A push without a time never coalesces with the next edit.
    pushEntry(undo_, std::move(inverse), bytes, state_, -std::numeric_limits<double>::infinity(), false);
    state_ = nextState_++;
    enforceCap(true);
}

bool UndoStack::pushOrMerge(std::unique_ptr<Command> inverse, std::size_t bytes, double nowMs)
{
    if (! inverse)
        return false;
    clearRedo();
    if (! undo_.empty())
    {
        Entry& top = undo_.back();
        if (! top.sealed && nowMs - top.lastMs <= kCoalesceWindowMs && top.cmd->mergeWith(*inverse, nowMs))
        {
            top.lastMs = nowMs;
            const std::size_t size = top.cmd->sizeBytes();
            bytes_ = bytes_ - top.bytes + size;
            top.bytes = size;
            state_ = nextState_++; // the model changed, so a checkpoint taken before this edit is left behind
            enforceCap(true);
            return true;
        }
    }
    pushEntry(undo_, std::move(inverse), bytes, state_, nowMs, false);
    state_ = nextState_++;
    enforceCap(true);
    return false;
}

bool UndoStack::undo(const Runner& run)
{
    return transfer(undo_, redo_, run);
}

bool UndoStack::redo(const Runner& run)
{
    return transfer(redo_, undo_, run);
}

bool UndoStack::transfer(std::deque<Entry>& from, std::deque<Entry>& to, const Runner& run)
{
    if (from.empty())
        return false;
    Entry e = std::move(from.back());
    from.pop_back();
    bytes_ -= e.bytes;
    if (! from.empty())
        from.back().sealed = true; // e.g. turn, undo, turn: the second turn is its own step
    const std::uint64_t before = state_;
    std::unique_ptr<Command> opposite = run(*e.cmd);
    state_ = e.restores;
    if (opposite)
    {
        const std::size_t size = opposite->sizeBytes();
        pushEntry(to, std::move(opposite), size, before, 0.0, true);
    }
    enforceCap(&to == &undo_);
    return true;
}

std::string_view UndoStack::undoLabel() const noexcept
{
    return undo_.empty() ? std::string_view{} : undo_.back().cmd->label();
}

std::string_view UndoStack::redoLabel() const noexcept
{
    return redo_.empty() ? std::string_view{} : redo_.back().cmd->label();
}

void UndoStack::clear()
{
    dropAll(undo_);
    dropAll(redo_);
}

void UndoStack::clearRedo()
{
    dropAll(redo_);
}

void UndoStack::checkpoint() noexcept
{
    clean_ = state_;
    seal();
}

void UndoStack::seal() noexcept
{
    if (! undo_.empty())
        undo_.back().sealed = true;
}

void UndoStack::setCapBytes(std::size_t cap)
{
    cap_ = cap;
    enforceCap(! undo_.empty());
}

void UndoStack::pushEntry(std::deque<Entry>& stack, std::unique_ptr<Command> cmd, std::size_t bytes,
                          std::uint64_t restores, double nowMs, bool sealed)
{
    bytes_ += bytes;
    stack.push_back(Entry{std::move(cmd), bytes, restores, nowMs, sealed});
}

void UndoStack::dropAll(std::deque<Entry>& stack) noexcept
{
    for (const Entry& e : stack)
        bytes_ -= e.bytes;
    stack.clear();
}

// Oldest history first (the bottom of the undo stack), then the farthest redo; the entry just recorded stays.
void UndoStack::enforceCap(bool newestOnUndo) noexcept
{
    const std::size_t keepUndo = newestOnUndo ? 1 : 0, keepRedo = newestOnUndo ? 0 : 1;
    while (bytes_ > cap_ && undo_.size() > keepUndo)
    {
        bytes_ -= undo_.front().bytes;
        undo_.pop_front();
        ++evicted_;
    }
    while (bytes_ > cap_ && redo_.size() > keepRedo)
    {
        bytes_ -= redo_.front().bytes;
        redo_.pop_front();
        ++evicted_;
    }
}
} // namespace core
