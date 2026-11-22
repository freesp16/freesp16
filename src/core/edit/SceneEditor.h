// Copy, paste, swap and reset of tracks, sequences, patterns, scenes, steps and pages (spec
// 02-features/editing-and-undo.md EDT-001…006, 009…012; 01-architecture/state-and-undo.md).
//
//   copy(src)          message thread: a value snapshot of src becomes the one clipboard (a second COPY overwrites
//                      it, EDT-010); O(size of the object): a pattern ≈ 8 KB + locks, a scene ≈ 160 KB + locks.
//   paste(dst, done)   EDT-004: a worker copies the clipboard into the new sub-tree (a full scene ≈ 2 MB worst case:
//                      the slow part), then the message thread installs it with ONE undoable command that exchanges it
//                      with the model's slot (no copy; merges into existing patterns run then) — the bus's bridge hook
//                      posts the engine messages, so the paste is audible at the next engine block while the transport
//                      keeps running (EDT-U4). Pastes install in the order they were made. `done` reports the result.
//   swapTracks / swap  EDT-002: synchronous, one undo step, no copies for whole objects.
//   reset(target)      EDT-003 / EDT-012: synchronous, one undo step (delete = reset, the 16 slots always exist).
//   busy()             a paste is being prepared; busyIndicator(now) once that has taken > 100 ms (EDT-004).
//
// The editor never touches the model off the message thread: the worker sees only the immutable clipboard payload.
// A paste whose project was replaced meanwhile (open / new) is dropped with a failure result. Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "core/edit/EditCommands.h"
#include "core/edit/EditRef.h"
#include "core/edit/WorkerPool.h"
#include "core/util/Clock.h"
#include "core/util/Result.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

namespace core::edit
{
using Result = core::Result<void>;

// The single clipboard (EDT-010): the payload of the last COPY and its source (the "copied" indicator).
class Clipboard
{
public:
    bool empty() const noexcept { return payload_ == nullptr; }
    const EditRef* source() const noexcept { return payload_ ? &payload_->source : nullptr; }
    std::shared_ptr<const ClipPayload> payload() const noexcept { return payload_; }
    std::uint32_t revision() const noexcept { return revision_; } // bumped by every set / clear
    void set(std::shared_ptr<const ClipPayload> p) noexcept
    {
        payload_ = std::move(p);
        ++revision_;
    }
    void clear() noexcept { set(nullptr); }

private:
    std::shared_ptr<const ClipPayload> payload_;
    std::uint32_t revision_ = 0;
};

// SCENE MANAGER button state (SCN-044, UI-284…288): white frame, top indicator (flashing while pending), lit in the
// scene colour (palette index 0–15) when it has content, the copy mark.
struct SceneManagerButton
{
    bool focus = false, current = false, pending = false, hasContent = false, copied = false;
    std::uint32_t colour = 0;
};

class SceneEditor
{
public:
    using Done = std::function<void(Result)>;
    static constexpr double kBusyAfterMs = 100.0; // EDT-004

    SceneEditor(CommandBus& bus, WorkerPool& workers, const Clock* clock = nullptr);
    ~SceneEditor();
    SceneEditor(const SceneEditor&) = delete;
    SceneEditor& operator= (const SceneEditor&) = delete;

    void copy(const EditRef& src);                       // EDT-001/010: one clipboard, overwritten
    bool canPaste(const EditRef& dst) const noexcept;    // EDT-011: clipboard not empty, the kind matches
    void paste(const EditRef& dst, Done done = {});      // async (EDT-004), one undo step
    void swapTracks(int scene, int a, int b, bool withSequences); // EDT-002
    bool swap(const EditRef& a, const EditRef& b);       // same family (Scene / Pattern / Track / Sequence)
    void reset(const EditRef& target);                   // EDT-003 / EDT-012

    bool busy() const noexcept { return ! jobs_.empty(); }
    bool busyIndicator(double nowMs) const noexcept;     // busy for more than kBusyAfterMs
    bool busyIndicator() const noexcept { return busyIndicator(clock_->nowMs()); }
    int pending() const noexcept { return static_cast<int>(jobs_.size()); }

    Clipboard& clipboard() noexcept { return clip_; }
    const Clipboard& clipboard() const noexcept { return clip_; }
    // Whether `ref` is the clipboard's source (the "copied" mark, EDT-010; Track / TrackWithSequences alike).
    bool isCopied(const EditRef& ref) const noexcept;
    CommandBus& bus() noexcept { return bus_; }
    // Later pastes are prepared on `workers` (ones already submitted complete through their own pool).
    void setWorkers(WorkerPool& workers) noexcept { workers_ = &workers; }

private:
    struct Job;
    void finished(std::uint64_t id);
    void installReady();
    const void* projectId() const noexcept;

    CommandBus& bus_;
    WorkerPool* workers_;
    SteadyClock steady_;
    const Clock* clock_;
    Clipboard clip_;
    std::deque<std::shared_ptr<Job>> jobs_;
    std::uint64_t nextJob_ = 1;
    std::shared_ptr<SceneEditor*> self_; // completions check it: a destroyed editor ignores them
};
} // namespace core::edit
