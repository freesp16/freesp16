// The session's one SceneEditor (EDT-010: a single clipboard, shared by the SCENE MANAGER, the panel's COPY / PASTE
// extension controls and the step-copy gesture). Message thread only.
//
// Composition root: setSharedWorkers(&pool) once the worker threads exist (WorkerPool.h shows the adapter); until then
// (tests, tools, the emulator without workers) pastes are prepared inline, i.e. synchronously. sharedEditor(bus)
// returns the editor bound to `bus`; asking with another bus (the M2Session installs the app's) rebinds it and keeps
// the clipboard (EDT-008: the clipboard survives within a session; Track / Scene clips paste only into their own
// project, EditCommands.h).
#pragma once

#include "core/commands/Command.h"
#include "core/edit/SceneEditor.h"
#include "core/edit/WorkerPool.h"

namespace core::edit
{
SceneEditor& sharedEditor(CommandBus& bus);
// nullptr = inline (synchronous). The pool must outlive the pastes submitted to it (drain it before destroying it).
void setSharedWorkers(WorkerPool* workers);
void resetSharedEditor();                   // tests: drop the editor and its clipboard
} // namespace core::edit
