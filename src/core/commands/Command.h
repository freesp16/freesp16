// Commands, the command bus and change notification (01-architecture/state-and-undo.md; EDT-U1…U5, EDT-007).
// Every user edit is a Command applied by the CommandBus on the message thread: apply → record the inverse on the
// UndoStack (EDT-U1, 256 MB cap) → notify observers (batched per message-loop iteration) → EngineBridge::apply (the
// engine hears the change at the next block, EDT-U4). Undo is just another execute of the inverse, so the bridge path
// is shared. CommandBus keeps its state behind a pimpl (CommandBus::Impl in core/commands/), next to UndoStack,
// SetParam<T> and CommandGroup (EDT-U3).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::model
{
struct Project;
}

namespace core
{
// Track moves carry their source and destination to the engine before forwarding the resulting model values.
struct TrackSwap
{
    std::uint8_t scene, first, second;
    bool withSequences;
};

// The model paths one command touched (data-model.md paths, e.g. "scene[0].track[3].volume"). Observers subscribe by
// prefix; the EngineBridge turns the set into engine messages. Message thread only.
class ChangeSet
{
public:
    void add(std::string path) { paths_.push_back(std::move(path)); }
    void merge(const ChangeSet& other) { paths_.insert(paths_.end(), other.paths_.begin(), other.paths_.end()); }
    void clear() noexcept
    {
        paths_.clear();
        trackSwaps_.clear();
    }
    void addTrackSwap(TrackSwap swap) { trackSwaps_.push_back(swap); }
    const std::vector<TrackSwap>& trackSwaps() const noexcept { return trackSwaps_; }
    bool empty() const noexcept { return paths_.empty(); }
    std::size_t size() const noexcept { return paths_.size(); }
    const std::vector<std::string>& paths() const noexcept { return paths_; }
    // True when any path starts with `prefix` (an empty prefix matches everything).
    bool touches(std::string_view prefix) const noexcept
    {
        return std::any_of(paths_.begin(), paths_.end(),
                           [prefix](const std::string& p) { return std::string_view(p).substr(0, prefix.size()) == prefix; });
    }

private:
    std::vector<std::string> paths_;
    std::vector<TrackSwap> trackSwaps_;
};

// RAII handle of an observer registration: destroying (or reset()) unsubscribes. Move-only.
class Subscription
{
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
    Subscription(Subscription&& other) noexcept : cancel_(std::exchange(other.cancel_, {})) {}
    Subscription& operator= (Subscription&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            cancel_ = std::exchange(other.cancel_, {});
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator= (const Subscription&) = delete;
    ~Subscription() { reset(); }
    void reset()
    {
        if (cancel_)
            std::exchange(cancel_, {})();
    }
    bool active() const noexcept { return static_cast<bool>(cancel_); }

private:
    std::function<void()> cancel_;
};

class Command
{
public:
    virtual ~Command() = default;
    virtual std::string_view label() const = 0;            // undo label, from ParamSpec.label where it applies
    virtual void apply(model::Project&) = 0;                // message thread only
    virtual std::unique_ptr<Command> inverse(const model::Project& before) const = 0;
    virtual bool undoable() const { return true; }
    // Coalescing (EDT-U2): a SetParam on the same path within 1 s merges into this command.
    virtual bool mergeWith(const Command&, double nowMs) { (void) nowMs; return false; }
    virtual void collectChanges(ChangeSet&) const = 0;      // paths touched → observers + EngineBridge
    // Bytes of inverse data this command holds, for the UndoStack's 256 MB cap (EDT-U1).
    virtual std::size_t sizeBytes() const { return sizeof(*this); }
};

class CommandBus
{
public:
    CommandBus();
    ~CommandBus();
    CommandBus(const CommandBus&) = delete;
    CommandBus& operator= (const CommandBus&) = delete;

    void execute(std::unique_ptr<Command>);                 // apply, record (UndoStack), notify, bridge
    bool undo();  bool redo();
    void checkpoint();                                      // on save (dirty tracking)
    Subscription observe(std::string_view pathPrefix, std::function<void(const ChangeSet&)>);

    bool dirty() const;                                     // changed since the last checkpoint
    void clearHistory();                                    // project open (EDT-U1: cleared on open)

    struct Impl; // model, UndoStack, observers, bridge hook
    // Owner-side set-up (model, bridge hook, clock, notification flush, recording groups) lives in
    // core/commands/CommandBusAccess.h as free functions over the Impl.
    Impl& impl() noexcept { return *impl_; }
    const Impl& impl() const noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};
} // namespace core
