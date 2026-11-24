// The UTILITY list (07-ui-design/screens/utility.md "Layout — UTILITY", UID-875…880, UI-326, SET-011/014): one widget,
// "SettingsList", for UTILITY and every sub-page (UID-880: CHANNEL MAPPING, MIDI PORTS, AUDIO DEVICE, OUTPUT CHANNEL
// MAP, LIBRARY LOCATIONS reuse the layout with the page name in the title band and the breadcrumb).
//
// Layout (template full): PanelTitle 16, 152, 1248, 48 (page name; right: "n sections" / the page's note); the list
// from y 200 to 608: section headers 36 tall (surface-3, caption, not focusable), rows 44 (zebra per section, name at
// x 56, value at x 664, a 1 lpx border divider at x 640 on value rows, › at x 1232 on rows that open a page), only
// whole rows drawn; Scrollbar 1256, 204, 4, 400. Editing (UID-877): the focused row's value cell becomes the inset
// box (ListRow editing: 648, y+4, 600 × 36, accent border, ‹ ›, value body-strong).
//
// Rows come from the SettingsRegistry (sections → rows of the page's settings id, UID-220: a preference is one
// registration) and, for the pages whose rows are data (CHANNEL MAPPING: 16 tracks; MIDI PORTS: one group per port;
// LIBRARY LOCATIONS: one row per location), from code. A Value row binds, in this order: a project field
// (services/settings/ProjectSettings.h: saved with the project, undoable, committed on push), the composite SYNC SOURCE
// ("settings.syncSource" → midi.syncSource + midi.syncPort: INTERNAL / DIN MIDI / USB MIDI, then the other ports by
// name), a value list of a ParamSpec ("settings.midiOutModeDin" / "…Usb" → midi.portMode#0 / #1 with the manual's
// labels, the USB port's two values only), a ParamSpec ("id" or "id#scope"; through the shell's
// ParamAccess, which saves preferences — PrefAccess.h), a PrefSpec (services::settings::Preferences directly). A row
// whose binding or target screen is not registered is shown disabled with "—" (e.g. MIDI or controller ids
// in a build without them).
//
// Input: turn = row focus (headers and disabled rows skipped); SHIFT + turn = first row of the next / previous
// section (UID-170); push on a value = edit (an invisible EditCapture popup takes push / BACK / the rotary while it
// lasts): turn = change, live for preferences (preview; BACK restores the value), shown only for project settings
// (one undo step on commit); push = commit; BACK = cancel. A list of more than eight values opens a ListPopup instead
// (UID-878). Push on a › row opens its page (the focus is remembered: BACK returns to that row, UID-879); on an action
// row runs it (CHANNEL MAPPING RESET asks first, SET-002); on an info row does nothing. Touch: tap a row = focus; tap
// the focused row = push; in editing, tap ‹ / › = step, elsewhere = commit.
#pragma once

#include "services/settings/ProjectSettings.h"
#include "ui/widgets/Widget.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui::utility
{
struct ValueChoice
{
    int value;              // the ParamSpec value
    std::string_view label; // what the row shows
};

struct Binding
{
    enum class Store : std::uint8_t
    {
        None,
        Param,
        Pref,
        Project,
        Sync,
        Choice
    } store = Store::None;
    const core::ParamSpec* spec = nullptr; // range, labels, format (Project: the field's)
    const services::settings::ProjectField* field = nullptr;
    std::uint16_t scope = 0; // ParamChange scope (Param) / field index (Project)
    std::string key;         // Pref: the preference key ("midi.portIn#2")
    std::span<const ValueChoice> choices{}; // Choice: the values offered, in order (option i = choices[i])
};

struct SettingsRow
{
    enum class Kind : std::uint8_t { Header, Value, Action, Info } kind = Kind::Info;
    std::string label;
    Binding bind {};
    std::string opens;  // Action: the screen it opens
    std::string action; // Action without a screen: "reset.midiIn" / "reset.midiOut" / "library.add" / "library.remove"
    std::string info;   // Info / Action value source: "version", "diskSpace", "audio.device", … or a fixed text "=…"
    std::string needs {};    // "sync.link": disabled unless SYNC SOURCE = LINK
    int track = 0;      // CHANNEL MAPPING: the track colour tick
};

// The rows of a page (UTILITY: "utility"), headers included. `params` resolves ParamSpec bindings.
std::vector<SettingsRow> buildRows(std::string_view page, const core::ParamRegistry& params);
std::string_view pageNote(std::string_view page); // the title band's right text ("" = none; UTILITY: computed)

// Test access (the body of a SettingsList screen; null / −1 / "" for another body).
int settingsFocus(const Widget* body) noexcept;       // index into the page's rows
bool settingsEditing(const Widget* body) noexcept;
const std::vector<SettingsRow>* settingsRows(const Widget* body) noexcept;
std::string settingsValueText(const Widget* body, int row); // what the value column shows
void forgetUtilityFocus() noexcept; // the remembered rows of every page (tests start from row 1)
} // namespace ui::utility
