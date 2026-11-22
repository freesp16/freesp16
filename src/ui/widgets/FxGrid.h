// FxGrid — SELECT FX (screens/effects.md "SELECT FX (grid popup)", components/controls.md "GridPopup"; UID-831…833,
// UI-262, FX-011, OQ-08): the slot's effect list in reference order from the registry (ui/screens/effects/
// FxCatalogue.h listFor), five cells per row, three rows per page, opening on the current effect; one tap or turn +
// push selects and closes, BACK cancels. P3 extensions appear only with their flag and carry an EXT badge. A newly
// registered effect takes the next free cell with no change here.
//
// The popup itself is GridPopup: the panel shows it with ShellApi::showGrid, and ShellApi::open("fx.select")
// builds it through the popup factory registered in EffectPanel.cpp. FxGrid builds its GridRequest and draws, through
// the request's cell hook (GridRequest::drawCell, with the frame's palette), what GridPopup does not know: the atlas
// icons, the EXT badge, the slot note after the title ("FX1 · TRACK 5", "SEND FX") and the ‹ › n / N pager.
#pragma once

#include "engine/fx/EffectDescriptor.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/shell/Popup.h"

#include <functional>
#include <string_view>

namespace ui
{
class FxGrid
{
public:
    using OnChoose = std::function<void(const engine::fx::EffectDescriptor&)>;
    // The SELECT FX request for `slot` (insert: the note names `track`), focused on `currentId`.
    static GridRequest request(fx::Slot slot, int track, std::string_view currentId, OnChoose onChoose);
    // The cell label: PanelHints::gridLabel when given, else the descriptor's name.
    static std::string_view labelOf(const engine::fx::EffectDescriptor&) noexcept;
};
} // namespace ui
