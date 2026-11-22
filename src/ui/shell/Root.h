// The UI root the composition root drives: once per frame the app hands it the
// logical control events of every ControlSource, then asks it for the frame's DrawList. createRoot() returns the Shell
// (ui/shell/Shell.h: router, popups, frame widgets, HOME as the root screen) without touching app/.
#pragma once

#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "io/controllers/LampRules.h"
#include "ui/render/DrawList.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/Widgets.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace core
{
class ParamRegistry;
}

namespace ui
{
struct RootContext
{
    const core::ParamRegistry& params;
    TextMeasure measure;                           // glyph-atlas text metrics in canvas lpx
    engine::EngineBridge* bridge = nullptr;        // parameter changes out, meters/playhead in
    std::function<void(const std::string&)> log{}; // one app log line (input tracing)
    // Added for the model-backed document (members may be added):
    ParamAccess* values = nullptr;                  // the model-backed values (ModelParamAccess); null = local values
    std::function<void(UiState&, double nowMs)> beforeFrame{}; // fills the state from the model before each frame
};

class Root
{
public:
    virtual ~Root() = default;
    virtual std::string_view id() const noexcept = 0;               // "home", the root screen (a test pins it)
    virtual void handle(const io::ControlEvent&, double nowMs) = 0; // UI thread, before frame()
    virtual void frame(DrawList&, double nowMs) = 0;                // reads the engine feeds, appends the frame
    // UI-150: the lamp frame of the last frame() — what every lamp shows, computed from the screen's
    // state; null = no lamp model. The composition root resolves it into the platform's sink after frame():
    //   if (lamps) if (const auto* f = root->lamps()) io::apply(*f, nowMs, io::LampBrightness::Normal, *lamps);
    virtual const io::LampFrame* lamps() const noexcept { return nullptr; }
    // The shell behind the root (selection, router, values), null for a root without one.
    virtual ShellApi* shell() noexcept { return nullptr; }
};

std::unique_ptr<Root> createRoot(const RootContext&);
} // namespace ui
