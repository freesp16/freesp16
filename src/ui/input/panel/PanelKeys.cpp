#include "ui/input/panel/PanelKeys.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/UndoStack.h"
#include "ui/input/pads/PadController.h"
#include "ui/input/steps/StepController.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/ShellApi.h"

#include <cstdarg>
#include <cstdio>

namespace ui::input::panel
{
namespace
{
constexpr std::string_view kSceneManager = "sceneManager", kPlayback = "track.playback",
                           kSetScale = "track.playback.setScale", kTrackMenu = "track.menu",
                           kSetScaleMidi = "track.setScaleMidi", kLiveSampling = "sampling.live",
                           kSamplingEdit = "sampling.edit";
}

PanelKeys::PanelKeys(core::CommandBus& bus, pads::PadController& pads, steps::StepController& steps)
    : bus_(bus), pads_(pads), steps_(steps)
{
}

void PanelKeys::trace(const char* fmt, ...) const
{
    if (! log)
        return;
    char line[160];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    log(line);
}

bool PanelKeys::handle(const io::ControlEvent& e, bool shift)
{
    if (e.kind != io::ControlEvent::Kind::Press)
        return false;
    namespace cmd = core::commands;
    switch (e.control)
    {
    case io::Control::Scene:
        if (! shift || shell_ == nullptr)
            return false;
        if (shell_->current().id == kSceneManager)
        {
            trace("INPUT key shift+scene -> back");
            shell_->back(); // the screen closes as with a tap on the lit SCENE | PATTERN item (UI-408)
        }
        else
        {
            trace("INPUT key shift+scene -> %.*s", static_cast<int>(kSceneManager.size()), kSceneManager.data());
            shell_->open(kSceneManager);
        }
        return true;
    case io::Control::PadModeScale:
        if (! shift || shell_ == nullptr || pads_.mode() != pads::PadMode::Scale)
            return false; // SHIFT + an unlit SCALE key: the pad controller (SHIFT never selects a mode by itself)
        {
            // MIDI / AS-1 tracks have SET SCALE (MIDI) in their TRACK MENU (UI-403, playback.md "Entry").
            const int mode = m1::trackMode(*shell_, shell_->state().selectedTrack);
            const bool midi = mode == 1 || mode == 3;
            const auto target = midi ? kSetScaleMidi : kSetScale;
            trace("INPUT key shift+padmode.scale -> %.*s", static_cast<int>(target.size()), target.data());
            if (shell_->current().id != target)
            {
                shell_->open(midi ? kTrackMenu : kPlayback); // the breadcrumb's parent first: BACK returns there
                shell_->open(target);
            }
        }
        return true; // the SCALE mode stays on (without SHIFT the lit key ends it, UI-104)
    case io::Control::Rec:
        if (! shift || shell_ == nullptr || shell_->current().id == kLiveSampling || shell_->current().id == kSamplingEdit)
            return false; // plain REC: the transport; on the recorders: their own REC
        trace("INPUT key shift+rec -> %.*s", static_cast<int>(kLiveSampling.size()), kLiveSampling.data());
        shell_->open(kTrackMenu); // the breadcrumb's parent first: BACK returns there
        shell_->open(kLiveSampling);
        return true;
    case io::Control::ExtUndo:
    case io::Control::ExtRedo:
    {
        const bool undo = e.control == io::Control::ExtUndo;
        if (! cmd::hasProject(bus_))
            return true;
        if (cmd::groupOpen(bus_))
        {
            trace("EDIT %s refused: a recording pass is open", undo ? "undo" : "redo");
            return true;
        }
        const auto& history = cmd::history(bus_);
        const std::string label(undo ? history.undoLabel() : history.redoLabel());
        const char* action = undo ? "undo" : "redo";
        if (undo ? bus_.undo() : bus_.redo())
            trace("EDIT %s ok%s%s", action, label.empty() ? "" : " ", label.c_str());
        else if (! label.empty())
            trace("EDIT %s refused %s", action, label.c_str());
        else
            trace("EDIT %s nothing to %s", action, action);
        return true;
    }
    case io::Control::ExtClear:
        if (steps_.heldStep() == 0 || ! cmd::hasProject(bus_))
            return false; // no step held: the screen's DELETE (SCENE MANAGER), else nothing
        {
            const int step = steps_.heldStep();
            const bool ok = steps_.clearHeldStep();
            trace("EDIT clear step=%d track=%d %s", step, steps_.track() + 1, ok ? "ok" : "refused");
            return ok;
        }
    default:
        return false;
    }
}
} // namespace ui::input::panel
