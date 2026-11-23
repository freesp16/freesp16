// Popup runtime (screens/popups-and-feedback.md, UID-900…916): the base every popup widget derives from and the
// requests screens use to open one (Shell / ShellApi). A popup is modal over the dimmed screen (the shell draws the
// scrim), keeps the four regions' geometry (UID-110), opens with focus on its default (the table "Focus defaults"),
// and closes itself by setting `closed`. The shell routes input to the top popup first; what it does not consume goes
// to the screen behind (pads, step keys, transport and encoders keep working, UI-289 / UID-913).
#pragma once

#include "core/params/ParamSpec.h"
#include "ui/descriptors/ListSpec.h"
#include "ui/descriptors/PopupSpec.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/Widget.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
struct UiState;
class ShellApi;

// Touch events on Control::Screen (the frozen ControlEvent has one Touch kind): value 1 = down, 2 = drag,
// 0 = up; x, y = the logical 800 × 480 point, mapped to canvas lpx by ×1.6 (UID-100).
namespace touch
{
inline constexpr std::int16_t kUp = 0, kDown = 1, kDrag = 2;
inline constexpr float kToCanvas = 1280.0f / 800.0f;
inline bool isTouch(const io::ControlEvent& e) noexcept
{
    return e.kind == io::ControlEvent::Kind::Touch && e.control == io::Control::Screen;
}
inline float x(const io::ControlEvent& e) noexcept
{
    return static_cast<float>(e.x) * kToCanvas;
}
inline float y(const io::ControlEvent& e) noexcept
{
    return static_cast<float>(e.y) * kToCanvas;
}
} // namespace touch

// Keys of a physical keyboard that a text popup interprets itself (UI-122): everything else arrives as text.
enum class TextKey : std::uint8_t
{
    Backspace,
    Left,
    Right,
    WordLeft,
    WordRight,
    Return,
    Escape,
    Home,
    End,
    Up,
    Down,
    PageUp,
    PageDown,
    Tab
};

class PopupWidget : public Widget
{
public:
    explicit PopupWidget(PopupSpec::Kind k) noexcept : kind_(k) {}
    PopupSpec::Kind kind() const noexcept { return kind_; }

    // The knob-strip bindings while this popup is open (UID-125); empty = the strip is dimmed under the scrim and the
    // encoders keep editing the screen behind.
    virtual std::span<const PopupBinding> bindings() const { return {}; }
    virtual bool swallowsEncoders() const { return false; } // a Modal binds the encoders itself
    virtual bool coversStrip() const { return false; }      // Dialog: the docked keyboard hides the strip
    virtual bool blocksHome() const { return false; }       // Progress: HOME ignored until done
    virtual bool modal() const { return true; }             // Toast: false (never blocks, never takes focus)
    virtual bool visibleAt(double nowMs) const
    {
        (void)nowMs;
        return true;
    }                                        // Progress: nothing for 300 ms
    virtual void cancel() { closed = true; } // BACK / HOME / click outside (when allowed)
    virtual bool text(std::string_view utf8)
    {
        (void)utf8;
        return false;
    } // a physical keyboard typed text
    virtual bool textKey(TextKey) { return false; }

    bool closed = false;
    double openedMs = 0; // for the base open transition (fade + 8 lpx rise, UID-065: input jumps it to the end)
    float dy = 0;        // set by the shell each frame: the rise offset and the nesting offset
    float dx = 0;
    const UiState* state = nullptr;
    ShellApi* shell = nullptr; // set by the shell when pushed: a popup may open a nested one (UID-914)
    double closedMs = -1;      // set by the shell when it closed: the close fade (motion.base) runs from here

protected:
    RectF place(RectF r) const noexcept { return {r.x + dx, r.y + dy, r.w, r.h}; }

private:
    PopupSpec::Kind kind_;
};

// --- requests --------------------------------------------------------------------------------------------------------
// A TrackChip before a list row's label (components/visuals.md "TrackChip", inline 32 × 24; UID-842: the SOURCE list
// shows each track's chip in its colour). track 0 = no chip.
struct ListChip
{
    int track = 0;      // the number drawn (1–16)
    int colour = 0;     // track palette index 1–16 (pad colour); 0 = the track's own
    bool dim = false;   // empty track: fill = the colour's dim variant
    bool muted = false; // muted glyph in the corner
};

struct ListRequest
{
    std::string_view title;
    std::vector<ListItem> items; // group headers: items with an empty label and a group name are inserted
    int current = -1;            // index of the current value (accent + ✓, focused on open, UID-902)
    bool grouped = false;        // insert group header rows from ListItem::group
    std::function<void(int index, const ListItem&)> onChoose;
    std::function<void()> onCancel;
    std::vector<ListChip> chips; // per item (index-aligned with items; shorter = no chip on the rest)
};

struct SettingRow
{
    std::string_view label;
    std::vector<std::string_view> options; // enum values (push / turn steps them)
    int current = 0;
    const core::ParamSpec* spec = nullptr; // numeric rows: formatted and stepped by the ParamSpec instead
    float value = 0;
    bool disabled = false;
    bool action = false; // an action row (`›`, SAVE SELECTION): push commits at once, no value side
};
struct SettingRequest
{
    std::string_view title;
    std::vector<SettingRow> rows;
    std::function<void(int row, int option, float value)> onCommit; // push on the value side confirms (UID-903)
};

struct MessageButton
{
    std::string label;
    ButtonStyle style = ButtonStyle::Plain;
};
struct MessageRequest
{
    glyph::Kind icon = glyph::Kind::Info; // Info / Caution / Error
    std::string title;
    std::vector<std::string> lines;     // ≤ 6 body lines (text-2)
    std::vector<MessageButton> buttons; // left to right; CANCEL first
    int defaultButton = -1;             // -1 = the last (affirmative) button (UID-906)
    int cancelButton = 0;               // BACK / Esc (the only button when there is one)
    std::function<void(int button)> onResult;
};

struct DialogRequest
{
    std::string title, prompt, text, hint;
    std::vector<MessageButton> buttons; // ≤ 3 at the fixed slots: CANCEL, primary, secondary (project.md)
    int primary = 1, cancel = 0;
    std::function<std::string(std::string_view)> validate; // non-empty = error text (rec), primary disabled
    std::function<void(int button, std::string_view text)> onResult;
};

struct ProgressRequest
{
    std::string title, detail;
    bool cancellable = false;
    std::function<void()> onCancel;
};

struct GridItem
{
    std::string_view label, icon;
    bool available = true; // unavailable in this slot = hidden (cells close up)
};
// What GridRequest::drawCell gets for one visible cell (after the cell background and outline, before the label).
struct GridCell
{
    int item = -1;               // index into GridRequest::items
    std::string_view icon;       // the item's icon id
    RectF cell, icon48, popup;   // cell 208 × 152, its 48 lpx icon square, the popup panel (follows its animation)
    std::uint32_t ink = 0;       // accent for the current entry, else text-1
    const theme::tk::Palette* palette = nullptr; // the frame's palette (dark / light, faded while opening)
    int slotOnPage = 0, page = 0, pages = 1;
    bool focused = false, current = false;
};
struct GridRequest
{
    std::string_view title;
    std::vector<GridItem> items;
    int current = -1;
    std::function<void(DrawList&, std::string_view icon, RectF cell48, std::uint32_t ink)> drawIcon; // 48 lpx icons
    std::function<void(DrawList&, const GridCell&)> drawCell; // replaces drawIcon when set
    std::function<void(int index)> onChoose;
};

struct ToastRequest
{
    std::string text;
    bool hasGlyph = false;
    glyph::Kind glyph = glyph::Kind::Check;
    float progress = -1;           // ≥ 0: inline bar; the toast stays until the job ends
    std::function<void()> onClick; // a toast with a target opens it
};
} // namespace ui
