// The remappable keyboard map (control-surface.md UI-120 "all keys remappable, stored in settings";
// 07-ui-design/screens/utility.md KEYBOARD MAP). A layer around the host KeyMap's default map (platform/host/input/
// KeyMap.h, UI-120, which stays as it is): the user moves a logical control to another key; the control's default key
// is then free, and a key drives at most one control. The host KeyMap asks resolve() for every one-control key and
// offer() for every key that goes down (KEYBOARD MAP's "Press a key…" capture, and Ctrl + \ = show / hide the
// virtual panel, our stand-in for ⌘\ of virtual-panel.md on a keyboard where ⌘ is the window manager's).
//
// Key codes are the host KeyMap's (printable keys = lower-case ASCII, kKeyF1 = 0x10001 …, mirrored below; a test keeps
// them equal). Modifier class: 0 = plain, 1 = ⌘, 2 = ⇧⌘. Reserved, never remapped (they are focus and turning
// gestures, not one-control keys): the arrows, `,` and `.` (sel), Shift, Control + 1…6, Option + ↑/↓.
// Message thread only (the keyboard's events and KEYBOARD MAP both run there); the app's instance is keyRemap().
#pragma once

#include "io/controllers/ControlSurface.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace io::controllers
{
namespace keys // = platform::host::KeyCode / KeyModifier
{
inline constexpr int f1 = 0x10001, left = 0x10020, right = 0x10021, up = 0x10022, down = 0x10023, ret = 0x10024,
                     escape = 0x10025, home = 0x10026, del = 0x10027, backspace = 0x10028;
inline constexpr unsigned modShift = 1, modCtrl = 2, modAlt = 4, modCmd = 8;
} // namespace keys

struct KeyBinding
{
    int code = 0;
    std::uint8_t cls = 0; // 0 plain, 1 ⌘, 2 ⇧⌘
    Control control = Control::Count;
};

// UI-120's default map as data (what KEYBOARD MAP shows; KeyMap's switch is the behaviour, a test keeps them equal).
std::span<const KeyBinding> defaultKeyBindings() noexcept;
// "Z", "F1", "SPACE", "ESC", "RETURN", "DEL", "⌘C", "⇧⌘Z", "—" (no key): the keycap texts of virtual-panel-keys.svg.
std::string keyName(int code, int cls);
// The modifier class of a KeyMap modifier set (−1 = a Control / Option combination: never one control).
int keyClass(unsigned mods) noexcept;

class KeyRemap
{
public:
    // The control `code` drives with modifier class `cls`, given KeyMap's default `dflt` (Control::Count = none).
    Control resolve(int code, int cls, Control dflt) const noexcept;
    // The key that drives `c` now (code 0 = none).
    KeyBinding keyFor(Control c) const noexcept;

    bool remappable(int code, int cls) const noexcept;
    void assign(Control c, int code, int cls); // moves c to the key (the key's previous control loses it)
    void reset(Control c);                     // back to the default key
    void resetAll();
    std::span<const KeyBinding> overrides() const noexcept { return overrides_; }
    void setOverrides(std::vector<KeyBinding> o);
    std::uint32_t version() const noexcept { return version_; } // bumps on every change (save, redraw)

    // KEYBOARD MAP capture: arm(); the next key down is captured (Esc cancels) instead of reaching the map.
    void arm() noexcept { armed_ = true; captured_ = {}; cancelled_ = false; ready_ = false; }
    void disarm() noexcept { armed_ = false; }
    bool armed() const noexcept { return armed_; }
    // True when a capture finished: `out` = the key (code 0 when cancelled or reserved: `cancelled`).
    bool takeCapture(KeyBinding& out, bool& cancelled) noexcept;
    // KeyMap: a key went down (after normalising). True = consumed here (capture, the panel toggle).
    bool offer(int code, unsigned mods) noexcept;
    // Ctrl + \ presses since start (the virtual panel's host polls it).
    int panelToggles() const noexcept { return panelToggles_; }

private:
    std::vector<KeyBinding> overrides_;
    std::uint32_t version_ = 0;
    bool armed_ = false, ready_ = false, cancelled_ = false;
    KeyBinding captured_ {};
    int panelToggles_ = 0;
};

KeyRemap& keyRemap() noexcept; // the app's (message thread)
} // namespace io::controllers
