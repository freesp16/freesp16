// StatusRow (components/frame.md "StatusRow", "Breadcrumb", "StatusIndicators"; UID-300, UI-200/201): y 0–40 on `bg`.
// Breadcrumb at x 16 in the label style, ancestors text-2, current text-1, `›` separators in text-3 with 8 lpx gaps,
// middle segments collapse to `…` beyond 560 lpx. Indicators right-aligned to x 1264, 40 apart: PLAY ▶, REC ●, OUT,
// IN (+ the SYNC badge when not internal, then "CPU 37%" while UTILITY › CPU USAGE is ON). The notification slot
// (x 320–880) belongs to Toast / caution chips.
#pragma once

#include "ui/shell/UiState.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <span>
#include <string_view>

namespace ui
{
struct StatusRowModel
{
    std::span<const std::string_view> breadcrumb{}; // titles from HOME to the current screen
    bool playing = false, recArmed = false;
    double midiInAtMs = -1e9, midiOutAtMs = -1e9;
    std::string_view syncBadge{};
    bool syncLost = false;
    std::string_view cpu{}; // CPU USAGE ON: "37%" (or "--"); empty = hidden
    bool cpuHigh = false;   // drawn in `warn`
};

class StatusRow final : public Widget
{
public:
    static constexpr std::size_t kMaxSegments = 8;
    void set(const StatusRowModel& m) noexcept { model_ = m; }
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool dirty(double nowMs) const override; // REC pulse, MIDI activity

    // Which segments are drawn (index into the breadcrumb, -1 = the `…` placeholder) within 560 lpx (UID-300).
    static int layout(std::span<const std::string_view> crumbs, const TextMeasure&, std::array<int, kMaxSegments>& out);

private:
    StatusRowModel model_;
};
} // namespace ui
