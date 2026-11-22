// What a body widget may ask of the shell (WidgetArgs::shell): navigate, select a track (UI-205), rebind the knob strip
// (two-tap: the first tap on a card binds the strip to it, UID-155, UI-223), and open popups and feedback
// (popups-and-feedback.md). Screens never reach the router or the popup stack any other way.
//
// Later additions (defaults keep older fakes compiling): the engine feeds and the frame's engine events, custom
// popup widgets, strip-column drags started by a body (UID-163), the current screen.
#pragma once

#include "ui/descriptors/ScreenSpec.h"
#include "ui/shell/Feeds.h"
#include "ui/shell/Popup.h"

#include <memory>
#include <string_view>

namespace core
{
class ParamRegistry;
}

namespace ui
{
class ParamAccess;
struct UiState;
class Progress;
class Toast;

class ShellApi
{
public:
    virtual ~ShellApi() = default;
    virtual bool open(std::string_view screenOrPopupId) = 0;
    virtual void back() = 0;
    virtual void home() = 0;
    virtual void selectTrack(int track1Based) = 0;
    // Binds the strip to `strip` (static storage) instead of the screen's own StripSpec; null restores the screen's.
    virtual void bindStrip(const StripSpec* strip) = 0;

    virtual void showList(ListRequest) = 0;
    virtual void showSetting(SettingRequest) = 0;
    virtual void showGrid(GridRequest) = 0;
    virtual void showMessage(MessageRequest) = 0;
    virtual void showDialog(DialogRequest) = 0;
    virtual Progress* showProgress(ProgressRequest) = 0; // valid until it closes (done() / cancel)
    virtual Toast* showToast(ToastRequest) = 0;          // replaces the current toast

    virtual UiState& state() = 0;
    virtual ParamAccess& values() = 0;
    virtual const core::ParamRegistry& params() const = 0;

    // The engine's meters, playhead and this frame's events, polled by the shell once per frame (ui/shell/Feeds.h).
    virtual const EngineFeeds& feeds() const
    {
        static const EngineFeeds silent{};
        return silent;
    }
    // Pushes a popup widget the screen built itself (a grid with its own cells, a picker …) on the popup stack, with
    // the scrim, strip rebinding and input routing of the built-in ones. Returns it (valid until it closes) or null
    // when the nesting depth refuses it (UID-914).
    virtual PopupWidget* showPopup(std::unique_ptr<PopupWidget> popup)
    {
        (void)popup;
        return nullptr;
    }
    // Called by a body while it handles a touch down: the following vertical drag turns knob-strip column `column0`
    // (0–5) like the encoder, 8 px per detent (UID-163: drag on a knob glyph = turning; SHIFT = fine). The drag ends
    // with the touch.
    virtual void dragColumn(int column0) { (void)column0; }
    // The screen on top of the router (the body's own screen while it is shown).
    virtual const ScreenSpec& current() const
    {
        static const ScreenSpec none{};
        return none;
    }
};
} // namespace ui
