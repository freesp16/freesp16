#include "core/commands/CommandGroup.h"

#include <algorithm>
#include <utility>

namespace core
{
void CommandGroup::add(std::unique_ptr<Command> child)
{
    if (child)
        children_.push_back(std::move(child));
}

bool CommandGroup::addOrMerge(std::unique_ptr<Command> child, double nowMs)
{
    if (! child)
        return false;
    if (! children_.empty() && children_.back()->mergeWith(*child, nowMs))
        return true;
    children_.push_back(std::move(child));
    return false;
}

void CommandGroup::reverse() noexcept
{
    std::reverse(children_.begin(), children_.end());
}

void CommandGroup::apply(model::Project& project)
{
    for (auto& child : children_)
        child->apply(project);
}

std::unique_ptr<Command> CommandGroup::inverse(const model::Project& before) const
{
    auto inv = std::make_unique<CommandGroup>(label_);
    inv->children_.reserve(children_.size());
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
        if ((*it)->undoable())
            inv->add((*it)->inverse(before));
    return inv;
}

bool CommandGroup::undoable() const
{
    for (const auto& child : children_)
        if (child->undoable())
            return true;
    return false;
}

bool CommandGroup::blockedTrackSwap(const TrackSwapGuard& blocked) const
{
    for (const auto& child : children_)
        if (child->blockedTrackSwap(blocked))
            return true;
    return false;
}

void CommandGroup::collectChanges(ChangeSet& changes) const
{
    for (const auto& child : children_)
        child->collectChanges(changes);
}

std::size_t CommandGroup::sizeBytes() const
{
    std::size_t bytes = sizeof(*this) + label_.capacity() + children_.capacity() * sizeof(children_[0]);
    for (const auto& child : children_)
        bytes += child->sizeBytes();
    return bytes;
}

std::unique_ptr<CommandGroup> CommandGroup::applyWithInverse(model::Project& project)
{
    std::vector<std::unique_ptr<Command>> inverses;
    inverses.reserve(children_.size());
    for (auto& child : children_)
        if (auto inv = applyAndInvert(*child, project))
            inverses.push_back(std::move(inv));
    if (inverses.empty())
        return nullptr;
    auto group = std::make_unique<CommandGroup>(label_);
    group->children_.reserve(inverses.size());
    for (auto it = inverses.rbegin(); it != inverses.rend(); ++it)
        group->children_.push_back(std::move(*it));
    return group;
}

std::unique_ptr<Command> applyAndInvert(Command& cmd, model::Project& project)
{
    if (auto* group = dynamic_cast<CommandGroup*>(&cmd))
        return group->applyWithInverse(project);
    std::unique_ptr<Command> inv = cmd.undoable() ? cmd.inverse(project) : nullptr;
    cmd.apply(project);
    return inv;
}
} // namespace core
