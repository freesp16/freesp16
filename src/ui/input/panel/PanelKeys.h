// The panel and keyboard paths of the document (NFR-040: every action reachable by panel or keyboard;
// 03-ui/control-surface.md UI-102/103/120, 03-ui/navigation-and-workflows.md UI-403). The composition root calls
// handle() for every control event before the SCENE / PATTERN keys and the pad controller see it:
//
//   SHIFT + SCENE           SCENE MANAGER (UI-403); pressed again while it is shown, SHIFT + SCENE closes it (as a tap
//                           on the lit SCENE | PATTERN item, UI-408). The SCENE mode does not change.
//   SHIFT + lit SCALE       SET SCALE of the selected track (UI-403: PLAYBACK › SET SCALE); the SCALE mode stays on.
//                           (SHIFT + the lit SLICE key = SET SLICE is the document's slice handling.)
//   SHIFT + REC             LIVE SAMPLING (our choice; sampling.md "Entry") over the TRACK MENU, so BACK
//                           returns there; the transport's REC is not armed. On SAMPLING/EDIT and LIVE SAMPLING the
//                           key stays the screen's (their REC).
//   ext.undo / ext.redo     the document's undo / redo (EDT-007; Cmd+Z / Shift+Cmd+Z). Refused while a recording
//                           pass is open (its CommandGroup closes on stop / REC off: the pass is one undo step).
//   ext.clear + held step   clears that step on the selected track (UI-120, SEQ-012). Without a held step ext.clear is
//                           left to the screen (SCENE MANAGER: DELETE).
//
// Each path logs one trace line ("EDIT undo ok", "INPUT key shift+scene -> sceneManager", …). Message thread only.
#pragma once

#include "io/controllers/ControlSurface.h"

#include <functional>
#include <string>

namespace core
{
class CommandBus;
}

namespace ui
{
class ShellApi;
}

namespace ui::input::pads
{
class PadController;
}

namespace ui::input::steps
{
class StepController;
}

namespace ui::input::panel
{
class PanelKeys
{
public:
    PanelKeys(core::CommandBus&, pads::PadController&, steps::StepController&);

    void attach(ShellApi* shell) noexcept { shell_ = shell; }
    std::function<void(const std::string&)> log;

    // True when the event was consumed (nothing else may act on it).
    bool handle(const io::ControlEvent&, bool shift);

private:
    void trace(const char* fmt, ...) const;

    core::CommandBus& bus_;
    pads::PadController& pads_;
    steps::StepController& steps_;
    ShellApi* shell_ = nullptr;
};
} // namespace ui::input::panel
