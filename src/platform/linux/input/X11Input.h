// The linux window's input as one io::ControlSource ("x11-input"), polled on the UI thread: it drains the
// shared window's X connection without blocking (XPending / XNextEvent) — the renderer uses the same connection on
// the same thread — and produces:
//  - keyboard: X keysyms → the host KeyMap codes (UI-120, platform/linux/input/XKeyMap) through KeyboardSource's
//    toolkit-independent key(code, mods, down, repeat, t); pad hits go straight to the engine's TriggerSink (UI-118).
//    Auto-repeat is detected with XkbSetDetectableAutoRepeat (a repeat = a second press without a release); keys
//    held when the window loses focus are released.
//  - mouse: button 1 = LCD touch (Kind::Touch on Control::Screen: 1 down, 2 drag while held, 0 up) at the logical
//    800 × 480 point (window pixels / scale); the wheel turns the Selector (up = +1 detent); Ctrl + wheel plays
//    the touch strip, releasing Ctrl lifts it (StripWheel.h).
//  - window: the close button (WM_DELETE_WINDOW) asks the app to quit (io::requestQuit); an Expose makes the next
//    software present send the whole frame.
// Before a renderer opened the window there is nothing to read (poll() returns nothing).
#pragma once

#include "platform/host/input/KeyboardSource.h"
#include "platform/linux/display/LinuxDisplay.h"
#include "platform/linux/input/StripWheel.h"

#include "io/controllers/ControlSurface.h"

#include <array>
#include <memory>

namespace platform::linux_desktop
{
class X11InputSource final : public io::ControlSource
{
public:
    X11InputSource(engine::TriggerSink* triggers, std::shared_ptr<LinuxWindow> window);
    std::string_view id() const override { return "x11-input"; }
    bool start(std::string& info) override;
    void poll(std::vector<io::ControlEvent>& out) override;
    void stop() override;

private:
    void releaseAll(double t, std::vector<io::ControlEvent>& out);

    std::shared_ptr<LinuxWindow> window_;
    platform::host::KeyboardSource keyboard_;
    void* attached_ = nullptr;       // the Display* auto-repeat detection was set on
    std::array<int, 256> held_ {};   // X keycode → KeyMap code while the key is down (0 = up)
    unsigned mods_ = 0;
    bool touching_ = false;
    StripWheel strip_; // Ctrl + wheel = the touch strip
    std::int16_t lastX_ = -1, lastY_ = -1;
};
} // namespace platform::linux_desktop
