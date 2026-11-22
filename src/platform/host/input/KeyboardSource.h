// The computer keyboard as the host platform's "keyboard" io::ControlSource (control-surface.md §2.3,
// UI-120…122). It is also a juce::KeyListener: a host window (the preview) attaches it with
//   if (auto* keys = dynamic_cast<juce::KeyListener*>(source)) window.addKeyListener(keys);
// so the window needs only platform/Platform.h and JUCE. Key events arrive on the message thread; key() may be called
// from any thread (tests, other front ends). Pad hits go to the engine's TriggerSink at once (UI-118); poll() hands
// the queued events to the UI thread.
#pragma once

#include "platform/host/input/KeyMap.h"

#include "io/controllers/ControlSurface.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <mutex>
#include <string>
#include <vector>

namespace platform::host
{
class KeyboardSource final : public io::ControlSource, public juce::KeyListener
{
public:
    explicit KeyboardSource(engine::TriggerSink* triggers) : triggers_(triggers) {}
    std::string_view id() const override { return "keyboard"; }
    bool start(std::string& info) override;
    void poll(std::vector<io::ControlEvent>& out) override;

    // Toolkit-independent entry (KeyMap codes and modifiers).
    bool key(int code, unsigned mods, bool down, bool repeat, double timeMs);
    void setModifiers(unsigned mods, double timeMs);
    void setTextMode(bool on);       // UI-122
    void setFixedVelocity(int v);    // UI-121, default 100

    // juce::KeyListener: keyPressed = a key went down (or repeated); keyStateChanged = check which held keys let go.
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged(bool isKeyDown, juce::Component*) override;

    // JUCE key code / modifiers → KeyMap's (exposed for tests).
    static int codeFor(const juce::KeyPress&) noexcept;
    static unsigned modsFor(const juce::ModifierKeys&) noexcept;

private:
    struct Down
    {
        int juceCode, code;
    };
    engine::TriggerSink* triggers_;
    std::mutex mutex_;
    KeyMap map_;
    std::vector<io::ControlEvent> pending_;
    std::vector<Down> down_; // JUCE key codes held, for keyStateChanged
};
} // namespace platform::host
