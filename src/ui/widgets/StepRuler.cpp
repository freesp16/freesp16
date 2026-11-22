#include "ui/widgets/StepRuler.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr const char* kBeatLabels[4][4] = {{"1.1", "1.2", "1.3", "1.4"},
                                           {"2.1", "2.2", "2.3", "2.4"},
                                           {"3.1", "3.2", "3.3", "3.4"},
                                           {"4.1", "4.2", "4.3", "4.4"}};
} // namespace

void StepRuler::draw(DrawList& dl, const UiContext& ctx, const StepGridModel& m)
{
    const auto& p = ctx.palette;
    constexpr float sub = kW / 384.0f; // 16 steps × 24
    for (int k = 1; k < 384; ++k)
    {
        if (k % 24 == 0)
            continue;
        const float h = k % 12 == 0 ? 10.0f : 5.0f;
        dl.fill({kX0 + sub * static_cast<float>(k) - 0.5f, kBase - h, 1, h}, p.border);
    }
    for (int k = 0; k <= 16; ++k)
    {
        const bool beat = k % 4 == 0;
        const float h = beat ? 40.0f : 22.0f;
        dl.fill({kX0 + StepGrid::kPitch * static_cast<float>(k) - 0.5f, kBase - h, 1, h},
                beat ? p.borderStrong : p.text3);
    }
    dl.fill({kX0, kBase - 0.5f, kW, 1}, p.borderStrong);

    for (int n = 1; n <= 16; ++n)
    {
        const auto& c = m.cells[static_cast<std::size_t>(n - 1)];
        if (c.kind == StepKind::Empty)
            continue;
        const float x = markX(n, c.offset24);
        const RectF mark{x - 1.5f, kBase - 64, 3, 60};
        if (c.kind == StepKind::Param)
            dl.stroke(mark, m.colour, 1);
        else
            dl.fill(mark, c.kind == StepKind::Full ? m.colour : theme::withOpacity(m.colour, 0.5f));
        const float spacing = StepGrid::kPitch *
                              kRetrigSteps[std::clamp(c.retrigSpeed, 0, static_cast<int>(std::size(kRetrigSteps)) - 1)];
        const int repeats = std::clamp(c.retrigCount, 1, 16);
        for (int r = 1; r < repeats; ++r)
        {
            const float rx = x + spacing * static_cast<float>(r);
            if (rx > kX0 + kW + StepGrid::kPitch)
                break;
            dl.fill({rx - 1, kBase - 40, 2, 36}, m.colour);
        }
        if ((m.flashing >> (n - 1) & 1u) != 0)
            dl.stroke({x - 6, kBase - 70, 12 + spacing * static_cast<float>(repeats - 1), 72}, p.accent, 1.5f,
                      tk::radiusXs);
    }
    if (m.playStep > 0) // playing: a 2 lpx playhead line at the current step (sequence.md "States")
        dl.fill(
            {kX0 + StepGrid::kPitch * (static_cast<float>(m.playStep - 1) + m.playFraction) - 1, kTop, 2, kBase - kTop},
            p.playhead);
    const auto& labels = kBeatLabels[std::clamp(m.bar, 1, 4) - 1];
    for (int k = 0; k < 4; ++k)
        dl.text(kX0 + StepGrid::kPitch * 4 * static_cast<float>(k), 466, Align::Left, TextStyleId::Caption, p.text3,
                labels[k]);
}
} // namespace ui
