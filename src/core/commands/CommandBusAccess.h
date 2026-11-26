// What the owner of a CommandBus (the Session / composition root, tests) needs beyond the public header
// (core/commands/Command.h): the model it edits, the EngineBridge hook, the clock, the once-per-message-loop flush of
// observer notifications, and recording-pass groups (EDT-U3).
//
// Free functions over CommandBus::impl(); the semantics are documented here.
#pragma once

#include "core/commands/Command.h"
#include "core/util/Clock.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace core
{
class UndoStack;
}

namespace core::commands
{
// The model the bus edits (≈ 2 MB, on the heap). A new bus has none: the owner hands it one with replaceProject()
// (from makeDefaultProject or the store). Until then execute() ignores commands and undo()/redo() return
// false. The bus does not construct a Project itself because GCC 11 (the armel toolchain) does not finish compiling
// Project's implicit default constructor (> 2 GB, never ends) — hosts compile it fine.
bool hasProject(const CommandBus&) noexcept;
model::Project& project(CommandBus&) noexcept;             // precondition: hasProject()
const model::Project& project(const CommandBus&) noexcept; // precondition: hasProject()
// Project open / new (ProjectLifecycle): the bus takes `next` (nullptr = no project) and returns the previous model;
// the history is cleared and the state is clean (EDT-U1, EDT-007), an open group is dropped, pending notifications
// are discarded. Observers and the bridge are not told — the owner rebuilds screens and engine objects for a new
// project anyway.
std::unique_ptr<model::Project> replaceProject(CommandBus&, std::unique_ptr<model::Project> next);

// EngineBridge::apply (engine/EngineApi.h), called synchronously after every execute/undo/redo with the paths that
// command touched and the model after it — so an undo is audible at the next block (EDT-U4). core may not include
// engine/, hence a function. Empty = no engine (tests, headless tools).
using BridgeHook = std::function<void(const ChangeSet&, const model::Project&)>;
void setBridge(CommandBus&, BridgeHook);
// Called before track swaps, including history replay. Return true to refuse a swap involving this slot.
void setTrackSwapGuard(CommandBus&, TrackSwapGuard);

// The coalescing clock (EDT-U2). Not owned; nullptr restores the bus's SteadyClock.
void setClock(CommandBus&, const Clock*);

// Observer notification is batched per message-loop iteration (state-and-undo.md "Observers": a 64-step paste is one
// repaint). Either the loop calls flushNotifications() once per iteration (the frame loop), or the owner installs a
// scheduler — e.g. juce::MessageManager::callAsync — that the bus calls once with a flush callback when the first
// change of a batch arrives. The callback is safe to run after the bus is gone (it does nothing then).
using NotifyScheduler = std::function<void(std::function<void()> flush)>;
void setNotifyScheduler(CommandBus&, NotifyScheduler);
void flushNotifications(CommandBus&);
bool notificationsPending(const CommandBus&) noexcept;

// Recording-pass groups (EDT-U3): between beginGroup() and endGroup(), every executed command is applied, bridged and
// notified as usual, but its inverse joins one CommandGroup that becomes a single undo step when the group ends
// (transport stop or record off). endGroup() with nothing recorded pushes nothing. undo()/redo() end an open group
// first; beginGroup() while one is open ends that one.
void beginGroup(CommandBus&, std::string label = "RECORD");
void endGroup(CommandBus&);
bool groupOpen(const CommandBus&) noexcept;

// The history, for the undo/redo labels of the UI and for tests (read-only; the bus is the only writer).
const UndoStack& history(const CommandBus&) noexcept;
void setHistoryCapBytes(CommandBus&, std::size_t capBytes); // tests: exercise eviction without 256 MB

// RAII recording pass.
class ScopedGroup
{
public:
    ScopedGroup(CommandBus& bus, std::string label = "RECORD") : bus_(bus) { beginGroup(bus_, std::move(label)); }
    ~ScopedGroup() { endGroup(bus_); }
    ScopedGroup(const ScopedGroup&) = delete;
    ScopedGroup& operator=(const ScopedGroup&) = delete;

private:
    CommandBus& bus_;
};
} // namespace core::commands
