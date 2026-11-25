// The 4 × 4 TrackTile grid of HOME and TRACK SETTING (home.md, track-setting.md; UID-500…504, UID-525…531): tiles
// in pad order at the fixed grid (layout::trackTile), selection following taps and the rotary (wraps 1 → 16), trigger
// flashes and mini levels from the engine's meter feed (EngineFeeds, UID-503). HOME opens TRACK MENU on a second tap
// or a rotary push (UID-155, UI-401). In TRACK SETTING, choose a pair of pads to exchange their tracks.
#pragma once

#include "ui/widgets/TrackTile.h"
#include "ui/widgets/Widget.h"

#include <array>

namespace ui
{
class TrackGrid final : public Widget
{
public:
    TrackGrid(const WidgetArgs&, TrackTileModel::Variant) noexcept;
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool dirty(double nowMs) const override;
    // The tile model of track t at nowMs (also used by the tests).
    TrackTileModel model(int track, const UiContext&) const noexcept;

private:
    void bindStripForMode() const;
    void swapSelection(int picked);
    int sourceScene_ = -1;
    int sourceTrack_ = 0;
    ShellApi* shell_;
    UiState* state_;
    TrackTileModel::Variant variant_;
    mutable int boundMode_ = -1;
    mutable std::array<char[12], 16> vel_{}, choke_{};
};
} // namespace ui
