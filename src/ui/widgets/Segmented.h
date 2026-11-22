// Segmented (components/controls.md "Segmented"): equal segments (min 96 wide, 48 tall) in one radius-8 outline, an
// optional 11 lpx caption centred on a hairline above; the selected segment has an accent-weak fill, a 3 lpx accent top
// bar and text-1, the others text-2, disabled text-3. QUANTIZE, FULL|HALF|PARAM, BAR1–4, PEAK|RMS …
#pragma once

#include "ui/widgets/Widget.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace ui
{
struct SegmentedModel
{
    std::span<const std::string_view> items{};
    int selected = 0;
    std::uint32_t disabledMask = 0;
    std::string_view caption{};
    std::uint32_t captionBackground = 0; // the surface the caption sits on (0 = bg)
    bool focused = false;
};

class Segmented final : public Widget
{
public:
    Segmented(RectF r = {}, SegmentedModel m = {}) noexcept : rect_(r), model_(m) {}
    SegmentedModel& model() noexcept { return model_; }
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList& dl, const UiContext& ctx) const override { draw(dl, ctx, rect_, model_); }
    bool handle(const io::ControlEvent&, UiContext&) override;
    int segmentAt(float x, float y) const noexcept;
    static void draw(DrawList&, const UiContext&, RectF, const SegmentedModel&);

private:
    RectF rect_;
    SegmentedModel model_;
};
} // namespace ui
