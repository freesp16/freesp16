// CommandBus (core/commands/Command.h) and its Impl: model, UndoStack, observers, bridge hook
// (state-and-undo.md; EDT-U1…U5, EDT-007). Message thread only.
//
// execute(cmd): inverse against the model before → apply → EngineBridge hook with the touched paths (EDT-U4) →
// queue the paths for the batched observer flush → record the inverse (coalescing, EDT-U2; or into the open
// recording group, EDT-U3). undo()/redo() run the stored inverse through exactly the same path.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/CommandGroup.h"
#include "core/commands/UndoStack.h"
#include "core/model/Project.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace core
{
struct CommandBus::Impl
{
    struct Observer
    {
        std::uint64_t id;
        std::string prefix;
        std::function<void(const ChangeSet&)> fn;
        bool live = true;
    };

    // No default Project here: GCC 11 (the armel toolchain) does not finish compiling Project's implicit default
    // constructor (see CommandBusAccess.h), so the owner hands the bus its model with replaceProject().
    Impl() : self(std::make_shared<Impl*>(this)) {}

    double now() const noexcept { return clock->nowMs(); }

    // Apply one command (or a stored inverse) and publish what it touched; returns its inverse.
    std::unique_ptr<Command> run(Command& cmd)
    {
        std::unique_ptr<Command> inv = applyAndInvert(cmd, *model);
        scratch.clear();
        cmd.collectChanges(scratch);
        publish(scratch);
        return inv;
    }

    void publish(const ChangeSet& changes)
    {
        if (changes.empty())
            return;
        if (bridge)
            bridge(changes, *model);
        pending.merge(changes);
        if (scheduler && ! flushScheduled)
        {
            flushScheduled = true;
            scheduler(
                [weak = std::weak_ptr<Impl*>(self)]
                {
                    if (auto alive = weak.lock())
                        (*alive)->flush();
                });
        }
    }

    void record(std::unique_ptr<Command> inv)
    {
        if (! inv)
            return;
        if (group)
        {
            history.clearRedo();
            group->addOrMerge(std::move(inv), now());
            return;
        }
        const std::size_t bytes = inv->sizeBytes();
        history.pushOrMerge(std::move(inv), bytes, now());
    }

    void closeGroup()
    {
        if (! group)
            return;
        std::unique_ptr<CommandGroup> done = std::move(group);
        if (! done->empty())
        {
            done->reverse(); // recorded in execution order; the newest edit is undone first
            const std::size_t bytes = done->sizeBytes();
            history.push(std::move(done), bytes);
        }
    }

    void flush()
    {
        flushScheduled = false;
        if (pending.empty())
            return;
        // One notification per batch, each path once.
        std::vector<std::string> paths = pending.paths();
        pending.clear();
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        ChangeSet batch;
        for (auto& p : paths)
            batch.add(std::move(p));

        struct Delivering
        {
            Impl& impl;
            explicit Delivering(Impl& i) : impl(i) { ++impl.delivering; }
            ~Delivering()
            {
                if (--impl.delivering == 0 && impl.needsCompact)
                {
                    impl.needsCompact = false;
                    // remove_if + erase, not std::erase_if: GCC 11 (the armel toolchain) never finishes this TU with it
                    auto& obs = impl.observers;
                    obs.erase(std::remove_if(obs.begin(), obs.end(),
                                             [](const std::unique_ptr<Observer>& o) { return ! o->live; }),
                              obs.end());
                }
            }
        } guard(*this);
        // Observers added during delivery see the next batch; removed ones are skipped at once.
        const std::size_t n = observers.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            Observer& o = *observers[i];
            if (o.live && batch.touches(o.prefix))
                o.fn(batch);
        }
    }

    Subscription subscribe(std::string_view prefix, std::function<void(const ChangeSet&)> fn)
    {
        const std::uint64_t id = nextObserver++;
        observers.push_back(std::make_unique<Observer>(Observer{id, std::string(prefix), std::move(fn)}));
        return Subscription(
            [weak = std::weak_ptr<Impl*>(self), id]
            {
                if (auto alive = weak.lock())
                    (*alive)->unsubscribe(id);
            });
    }

    void unsubscribe(std::uint64_t id)
    {
        auto it = std::find_if(observers.begin(), observers.end(), [id](const auto& o) { return o->id == id; });
        if (it == observers.end())
            return;
        if (delivering > 0)
        {
            (*it)->live = false; // its std::function may be running right now
            needsCompact = true;
        }
        else
            observers.erase(it);
    }

    std::unique_ptr<model::Project> model; // null until replaceProject(): every edit is then ignored
    UndoStack history;
    SteadyClock steady;
    const Clock* clock = &steady;
    commands::BridgeHook bridge;
    commands::NotifyScheduler scheduler;
    std::vector<std::unique_ptr<Observer>> observers; // stable addresses while a callback subscribes
    std::uint64_t nextObserver = 1;
    int delivering = 0;
    bool needsCompact = false;
    ChangeSet pending, scratch;
    bool flushScheduled = false;
    std::unique_ptr<CommandGroup> group; // open recording pass (EDT-U3)
    std::shared_ptr<Impl*> self;         // last member, destroyed first: weak handles see the bus gone
};

// ---- the adapter (CommandBusAccess.h) ------------------------------------------------------------------------------
namespace
{
CommandBus::Impl& impl(CommandBus& bus) noexcept { return bus.impl(); }
const CommandBus::Impl& impl(const CommandBus& bus) noexcept { return bus.impl(); }
} // namespace

// ---- CommandBus ---------------------------------------------------------------------------------------------------

CommandBus::CommandBus() : impl_(std::make_unique<Impl>()) {}

CommandBus::~CommandBus() = default;

void CommandBus::execute(std::unique_ptr<Command> cmd)
{
    if (! cmd || ! impl_->model)
        return;
    impl_->record(impl_->run(*cmd));
}

bool CommandBus::undo()
{
    if (! impl_->model)
        return false;
    impl_->closeGroup(); // undo ends an open recording pass, then undoes it as one step
    return impl_->history.undo([this](Command& c) { return impl_->run(c); });
}

bool CommandBus::redo()
{
    if (! impl_->model)
        return false;
    impl_->closeGroup();
    return impl_->history.redo([this](Command& c) { return impl_->run(c); });
}

void CommandBus::checkpoint()
{
    impl_->closeGroup(); // saving ends the pass's undo step, so the saved state is a reachable undo state
    impl_->history.checkpoint();
}

Subscription CommandBus::observe(std::string_view pathPrefix, std::function<void(const ChangeSet&)> fn)
{
    return impl_->subscribe(pathPrefix, std::move(fn));
}

bool CommandBus::dirty() const
{
    return impl_->history.dirty() || (impl_->group && ! impl_->group->empty());
}

void CommandBus::clearHistory()
{
    impl_->group.reset();
    impl_->history.clear();
    impl_->history.checkpoint();
}

namespace commands
{
model::Project& project(CommandBus& bus) noexcept
{
    return *impl(bus).model;
}

const model::Project& project(const CommandBus& bus) noexcept
{
    return *impl(bus).model;
}

bool hasProject(const CommandBus& bus) noexcept
{
    return impl(bus).model != nullptr;
}

std::unique_ptr<model::Project> replaceProject(CommandBus& bus, std::unique_ptr<model::Project> next)
{
    auto& i = impl(bus);
    std::swap(i.model, next);
    i.group.reset();
    i.history.clear();
    i.history.checkpoint();
    i.pending.clear();
    return next;
}

void setBridge(CommandBus& bus, BridgeHook hook)
{
    impl(bus).bridge = std::move(hook);
}

void setClock(CommandBus& bus, const Clock* clock)
{
    auto& i = impl(bus);
    i.clock = clock != nullptr ? clock : &i.steady;
}

void setNotifyScheduler(CommandBus& bus, NotifyScheduler scheduler)
{
    auto& i = impl(bus);
    i.scheduler = std::move(scheduler);
    i.flushScheduled = false;
}

void flushNotifications(CommandBus& bus)
{
    impl(bus).flush();
}

bool notificationsPending(const CommandBus& bus) noexcept
{
    return ! impl(bus).pending.empty();
}

void beginGroup(CommandBus& bus, std::string label)
{
    auto& i = impl(bus);
    i.closeGroup();
    i.group = std::make_unique<CommandGroup>(std::move(label));
}

void endGroup(CommandBus& bus)
{
    impl(bus).closeGroup();
}

bool groupOpen(const CommandBus& bus) noexcept
{
    return impl(bus).group != nullptr;
}

const UndoStack& history(const CommandBus& bus) noexcept
{
    return impl(bus).history;
}

void setHistoryCapBytes(CommandBus& bus, std::size_t capBytes)
{
    impl(bus).history.setCapBytes(capBytes);
}
} // namespace commands
} // namespace core
