// The computer keyboard → logical controls (03-ui/control-surface.md §2.3, UI-120…122): the default map of UI-120
// (US key positions), fixed pad velocity 100 with `ext.accent` held = 127 (UI-121), auto-repeat ignored for pads and
// buttons (UI-121), the text-popup mode where only Esc and Return keep their meaning (UI-122). Device-independent and
// toolkit-independent (keys are KeyCode values); KeyboardSource feeds it from JUCE. The remappable map is KeyRemap.
//
// Details the spec leaves open (our choices):
//  - Option + ↑/↓ (fine) is sent as a Turn wrapped in Shift Press/Release (SHIFT + knob = fine, UI-111), because
//    ControlEvent has no fine flag; nothing is wrapped while Shift is physically held.
//  - The focused column (←/→, Control + 1…6) starts at 1 and stays within 1…6.
//  - ⌘S/⌘O/⌘N are menu-bar commands, not controls: unmapped here (⌘4 = PROJECT reaches SAVE / OPEN / CREATE NEW).
//  - `;` / `'` = ext.nudgeDown / ext.nudgeUp, held (SYN-005; UI-120 gives NUDGE no key).
//  - Typed text in the text mode is the window's (ui::Shell::typeText); ControlEvent has no Text kind.
#pragma once

#include "io/controllers/ControlSurface.h"
#include "io/controllers/profiles/KeyRemap.h"

#include <array>
#include <cstdint>
#include <vector>

namespace platform::host
{
// Printable keys are their lower-case ASCII code; the others:
enum KeyCode : int
{
    kKeyF1 = 0x10001, // … kKeyF1 + 11 = F12
    kKeyLeft = 0x10020, kKeyRight, kKeyUp, kKeyDown, kKeyReturn, kKeyEscape, kKeyHome, kKeyDelete, kKeyBackspace,
};
enum KeyModifier : unsigned { kModShift = 1, kModCtrl = 2, kModAlt = 4, kModCmd = 8 };

class KeyMap
{
public:
    // A key went down (repeat = auto-repeat) or up. Appends the logical events; pad hits also go to `triggers`
    // (UI-118; may be null). Returns true when the key is mapped.
    bool key(int code, unsigned mods, bool down, bool repeat, double timeMs, engine::TriggerSink* triggers,
             std::vector<io::ControlEvent>& out);
    // The modifier state changed (Shift → `shift` Press/Release).
    void modifiers(unsigned mods, double timeMs, std::vector<io::ControlEvent>& out);

    void setFixedVelocity(int v) noexcept { velocity_ = v < 1 ? 1 : (v > 127 ? 127 : v); }
    void setTextMode(bool on) noexcept { textMode_ = on; }
    int focusedColumn() const noexcept { return column_; }
    // The remap layer (UI-120 "all keys remappable"; io/controllers/profiles/KeyRemap.h). Default = the
    // app's keyRemap(); null = the plain default map.
    void setRemap(io::controllers::KeyRemap* remap) noexcept { remap_ = remap; }

private:
    struct Held
    {
        int code;
        io::Control control;
    };
    void press(int code, io::Control c, std::int16_t value, double t, std::vector<io::ControlEvent>& out);
    void turn(io::Control c, int detents, bool fine, double t, std::vector<io::ControlEvent>& out);

    std::array<Held, 32> held_ {};
    int heldCount_ = 0;
    bool shift_ = false, accent_ = false, textMode_ = false;
    int velocity_ = 100, column_ = 1;
    io::controllers::KeyRemap* remap_ = &io::controllers::keyRemap();
};
} // namespace platform::host
