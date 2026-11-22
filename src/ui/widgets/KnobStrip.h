// KnobStrip + StripTab (components/frame.md "KnobStrip", "StripTab"; UID-003, UID-120, UID-125, UID-166, UID-310,
// UID-311): y 624–768 on surface-1 with a 1 lpx border top edge; the trapezoid tab (x 0–248, y 624–648) names the
// owner of the bindings ("TRACK 1", "PLAYBACK") or reads "STEP n" on a mod fill during a step hold; six ParamColumns,
// column n = encoder n; 1 lpx border-strong separators between column groups (y 656–752). Never hidden.
#pragma once

#include "ui/widgets/ParamColumn.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ui
{
struct KnobStripModel
{
    std::string_view tab{};
    int stepHeld = 0;           // > 0: tab "STEP n" on mod (UID-166)
    std::uint8_t groupMask = 0; // bit n set = separator before column n (1…5), e.g. MIXER 1–3 | 4–6 = bit 3
    std::array<ParamColumnModel, 6> columns{};
};

class KnobStrip final : public Widget
{
public:
    KnobStrip() noexcept;
    KnobStripModel& model() noexcept { return model_; }
    const KnobStripModel& model() const noexcept { return model_; }
    RectF bounds() const noexcept override;
    void build(DrawList&, const UiContext&) const override;
    bool dirty(double nowMs) const override;
    int columnAt(float x, float y) const noexcept; // 0–5, -1 outside the columns
    double lastTouchMs = -1e9;                     // for dirty(): the underline fades out after 600 ms

private:
    KnobStripModel model_;
};
} // namespace ui
