// The on-screen virtual panel's behaviour (control-surface.md §2.5 UI-140…144, 07-ui-design/screens/
// virtual-panel.md UID-925…937): the 1280 × 480 lpx layout of every UI-101 / UI-102 control in the reference grouping,
// hit areas grown to ≥ 44 × 44 lpx without overlapping a neighbour (UID-935), the pointer gestures → the ControlEvents a
// hardware controller sends, and the lamp mirror (a LampSink fed by the same LampModel frame as every controller, so
// it shows exactly what a lamp-capable controller shows, UID-929). Drawing is ui/screens/virtualpanel/ (DrawList).
//
// Gestures (virtual-panel.md "Interactions"):
//   key        press = Press, release = Release; right-click / Control-click / a 500 ms hold without moving latches it
//              (stays down until clicked again, UID-931); Shift-click = SHIFT + the key without latching SHIFT
//   pad        Press with velocity from the vertical position (top 127 … bottom 20, UID-930), Option = 127; vertical
//              drag after the hit = Pressure (up = more, 2 per lpx); release = Release
//   encoder    vertical drag 8 lpx per detent (up = +), wheel 1 per notch, Option = fine (wrapped in SHIFT as the
//              keyboard does); rotary: circular drag 15° per detent (clockwise = +), its centre (Ø 52) = sel.push
//   knob       afs.* / main.volume: vertical drag 200 lpx = 0…1023, double-click = default (512, MAIN OUT 819)
//   strip      press / drag / release inside it = Touch / Value / Release at bottom 0 … top 127
//   drawer bar click toggles collapsed / expanded; KEYS toggles the keycaps (UID-934); HIDE hides; FLOAT is only
//              offered where the host has windows (never on the device or the linux window)
// Every pointer is independent (multi-touch: chords, pad + strip). Pad hits are not pushed into the TriggerSink:
// PadController posts them for kSourceVirtual (UI thread, the panel's own latency is a frame anyway).
// Message thread only; allocation-free after construction.
#pragma once

#include "io/controllers/ControlNames.h"
#include "io/controllers/ControlSurface.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace io::controllers
{
struct PanelRect
{
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const noexcept { return px >= x && px < x + w && py >= y && py < y + h; }
    float cx() const noexcept { return x + w * 0.5f; }
    float cy() const noexcept { return y + h * 0.5f; }
};

enum class PanelItemKind : std::uint8_t { Key, Pad, Step, Encoder, Rotary, Knob, Strip, Lamp, PanelButton };
enum class LampStyle : std::uint8_t { None, Fill, Bar, Dot };
enum class PanelButton : std::uint8_t { None, Keys, Float, Hide };

struct PanelItem
{
    PanelItemKind kind = PanelItemKind::Key;
    Control control = Control::Count; // lamp id for Lamp items (strip LEDs, indicators, bar position lamps)
    PanelRect rect {};                // drawn
    PanelRect hit {};                 // input (≥ 44 × 44, no overlap); empty for Lamp items
    std::string_view label {}, shiftLegend {};
    LampStyle lamp = LampStyle::None;
    PanelButton button = PanelButton::None;
};

enum class PanelState : std::uint8_t { Hidden, Collapsed, Expanded };

class VirtualPanel final : public ControlSource, public LampSink
{
public:
    static constexpr float kWidth = 1280, kHeight = 480, kBarHeight = 32;
    static constexpr int kPointers = 10;
    static constexpr double kLatchMs = 500, kRingMs = 600;
    struct Mods
    {
        bool option = false, control = false, shift = false, secondary = false; // secondary = right button
    };

    VirtualPanel();
    std::string_view id() const override { return "virtual-panel"; }
    void poll(std::vector<ControlEvent>& out) override;
    void setLamp(Control, std::uint32_t rgb, std::uint8_t level) override;
    void flush() override {}

    // Pointer input in panel lpx (0…1280 × 0…480). True = the panel used it.
    bool pointerDown(int id, float x, float y, Mods mods, double tMs);
    bool pointerMove(int id, float x, float y, double tMs);
    bool pointerUp(int id, float x, float y, double tMs);
    bool wheel(float x, float y, int notches, Mods mods, double tMs);
    void tick(double tMs); // long-press latches (once per frame)
    // Another source's event (keyboard, controllers): the panel shows the control's pressed state.
    void observe(const ControlEvent&);

    PanelState state() const noexcept { return state_; }
    void setState(PanelState s) noexcept { state_ = s; }
    void toggle() noexcept; // Hidden → Expanded → Hidden (Ctrl + \ / ⌘\)
    bool keycaps() const noexcept { return keycaps_; }
    void setKeycaps(bool on) noexcept { keycaps_ = on; }
    float height() const noexcept { return state_ == PanelState::Expanded ? kHeight : state_ == PanelState::Collapsed ? kBarHeight : 0; }

    std::span<const PanelItem> items() const noexcept { return items_; }
    const PanelItem* itemAt(float x, float y) const noexcept; // by hit area
    const PanelItem* item(Control c) const noexcept;
    struct Lamp
    {
        std::uint32_t rgb = 0;
        std::uint8_t level = 0;
    };
    Lamp lamp(Control c) const noexcept;
    bool pressed(Control c) const noexcept; // held by a pointer, latched, or down on another source
    bool latched(Control c) const noexcept;
    bool shiftShown() const noexcept { return pressed(Control::Shift); }
    int knobValue(Control c) const noexcept;
    double lastTurnMs(Control c) const noexcept;
    int encoderAngle(Control c) const noexcept; // detents turned (the indicator dot rotates 15° per detent)
    int stripPosition() const noexcept { return stripPos_; } // −1 = not touched

private:
    struct Pointer
    {
        bool active = false;
        const PanelItem* item = nullptr;
        float x0 = 0, y0 = 0, x = 0, y = 0, acc = 0;
        double t0 = 0;
        bool latchedNow = false, moved = false, shiftWrap = false, fine = false;
        int velocity = 0;
    };
    void emit(ControlEvent::Kind k, Control c, int v, double t);
    void press(Pointer&, const PanelItem&, Mods, double t);
    void release(Pointer&, double t);
    void turn(Control c, int detents, bool fine, double t);
    static std::size_t slotOf(Control c) noexcept { return static_cast<std::size_t>(lampSlot(c)); }

    std::vector<PanelItem> items_;
    std::vector<ControlEvent> pending_;
    std::array<Pointer, kPointers> pointers_ {};
    std::array<Lamp, kLampSlots> lamps_ {};
    std::array<std::uint8_t, kLampSlots> held_ {};    // pointers holding the control
    std::array<bool, kLampSlots> latched_ {}, external_ {};
    std::array<std::int16_t, kLampSlots> knob_ {};
    std::array<double, kLampSlots> turned_ {};
    std::array<int, kLampSlots> angle_ {};
    std::array<double, kLampSlots> lastClick_ {};
    PanelState state_ = PanelState::Hidden;
    bool keycaps_ = false;
    int stripPos_ = -1;
};

int knobDefault(Control c) noexcept; // double-click value (512, main.volume 819 ≈ 0 dB)

// The app's panel (process lifetime), registered as the control source "virtual-panel" (a proxy: polled and lamped
// like every source); the composition root draws it and routes the screen's pointer to it (ui/screens/virtualpanel/
// PanelHost.h). Its start state comes from the setting `virtual_panel=hidden|collapsed|expanded` (default
// hidden: the device has its physical panel, and the 800 × 480 linux window has no room for the docked drawer —
// virtual-panel.md collapses it below a 0.75 scale; Ctrl + \ shows and hides it).
VirtualPanel& virtualPanel();
} // namespace io::controllers
