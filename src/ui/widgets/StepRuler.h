// StepRuler (screens/sequence.md "Layout"; UID-752, UID-755): the micro-timing ruler of the SEQUENCE screen under the
// StepGrid, drawn from the same StepGridModel.
// 64, 376, 1152 × 64, base line y 440: 16 × 24 sub-ticks (half-step ticks 10 tall), step ticks 22 tall
//     text-3, beat ticks 40 tall border-strong; each trigger a 3 × 60 mark at its offset (FULL track colour, HALF 50 %,
//     PARAM outline) with its retrigger repeats (2 × 36) at RETRIGGER SPD spacing; target marks boxed 1.5 lpx accent,
//     flashing at 2 Hz; beat labels `b.1` … `b.4` at y 466.
// No allocation in build.
#pragma once

#include "ui/widgets/StepGrid.h"

namespace ui
{
class StepRuler final : public Widget
{
public:
    static constexpr float kX0 = 64, kW = 1152, kBase = 440, kTop = 376;
    // RETRIGGER SPD values as a fraction of a step (enumerations.md: 1/8, 1/6, 1/4, 1/3, 1/2, 1 step).
    static constexpr float kRetrigSteps[] = {1.0f / 8, 1.0f / 6, 1.0f / 4, 1.0f / 3, 1.0f / 2, 1.0f};
    // x of a trigger mark of step n (1–16) at `offset24`.
    static constexpr float markX(int n1, int offset24) noexcept
    {
        return kX0 + StepGrid::kPitch * (static_cast<float>(n1 - 1) + static_cast<float>(offset24) / 24.0f) + 4;
    }

    explicit StepRuler(const StepGridModel* model = nullptr) noexcept : model_(model) {}
    RectF bounds() const noexcept override { return {kX0, kTop, kW, 104}; }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (model_ != nullptr)
            draw(dl, ctx, *model_);
    }
    static void draw(DrawList&, const UiContext&, const StepGridModel&);

private:
    const StepGridModel* model_;
};
} // namespace ui
