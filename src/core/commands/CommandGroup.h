// A sequence of commands that is one undo step (state-and-undo.md): the `RecordPass` family (EDT-U3, one step per
// real-time recording pass), and multi-cell edits such as a 64-step paste (`ScenePatternEdit`, EDT-001…003) that must
// undo as one step and repaint once.
//
// The CommandBus executes a group with applyAndInvert(): every child's inverse is taken against the state just before
// that child, so a group is exact for any children (also ones that build on an earlier child's result). The plain
// Command::inverse(before) of a group takes every child's inverse against `before` and reverses them: exact for
// value-assignment children (SetParam, AssignSample, …), which is all a group is used for outside the bus.
#pragma once

#include "core/commands/Command.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core
{
class CommandGroup final : public Command
{
public:
    explicit CommandGroup(std::string label = "RECORD") : label_(std::move(label)) {}

    void add(std::unique_ptr<Command> child);
    // add(), unless the last child absorbs `child` (Command::mergeWith): the bus's open recording group keeps one
    // inverse per touched path run (a knob turned during a pass), without a time window — inside one undo step
    // merging consecutive same-path inverses is always exact.
    bool addOrMerge(std::unique_ptr<Command> child, double nowMs);

    // Reverses the children: the bus records a pass's inverses in execution order and reverses them when the pass
    // ends, so the newest edit is undone first.
    void reverse() noexcept;

    bool empty() const noexcept { return children_.empty(); }
    std::size_t size() const noexcept { return children_.size(); }
    const std::vector<std::unique_ptr<Command>>& children() const noexcept { return children_; }

    std::string_view label() const override { return label_; }
    void apply(model::Project&) override;                                          // children in order
    std::unique_ptr<Command> inverse(const model::Project& before) const override; // see the header comment
    bool undoable() const override;
    bool blockedTrackSwap(const TrackSwapGuard&) const override;                   // any child undoable
    void collectChanges(ChangeSet&) const override;                                // union of the children's
    std::size_t sizeBytes() const override;

    // Applies the children in order, taking each child's inverse just before it runs; returns the group that undoes
    // all of it (children reversed), or nullptr when no child was undoable.
    std::unique_ptr<CommandGroup> applyWithInverse(model::Project&);

private:
    std::string label_;
    std::vector<std::unique_ptr<Command>> children_;
};

// What the bus does for every command: the inverse against the state before it (nullptr when not undoable), then
// apply. A CommandGroup goes through applyWithInverse(), so nested groups are exact too.
std::unique_ptr<Command> applyAndInvert(Command&, model::Project&);
} // namespace core
