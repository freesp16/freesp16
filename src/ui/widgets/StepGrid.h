// StepGrid (components/visuals.md "StepGrid"; screens/sequence.md "Layout", UID-750…761): the SEQUENCE screen's 16
// step cells of one bar of one track (the micro-timing ruler under them is StepRuler.h, on the same model).
//   StepGrid: panel 16, 152, 1248 × 336 (surface-1, radius 8); cells x = 72 + 72·(n−1), y 200, 56 × 88 (radius 6):
//     empty surface-3, FULL track-colour fill, HALF the bottom half (OQ-09), PARAM a 2 lpx outline + 8 lpx mod dot,
//     `×n` (caption, on-accent) when RETRIGGER CNT > 1; numbers above at y 184 (1, 5, 9, 13 boxed 28 × 20); targets on
//     an 8 lpx accent-weak halo; the playing step outlined 2 lpx playhead; the held step gets the focus ring; a muted
//     track draws its cells at 40 %.
// Plain models only (UID-201); no allocation in build.
#pragma once

#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>

namespace ui
{
enum class StepKind : std::uint8_t
{
    Empty,
    Full,
    Half,
    Param
};

// One step of the shown bar.
struct StepCellView
{
    StepKind kind = StepKind::Empty;
    int offset24 = 0;    // −24 … +24 = ±1 step in 1/24 (SEQ-005)
    int retrigCount = 1; // 1–16 (SEQ-030)
    int retrigSpeed = 2; // index into StepRuler::kRetrigSteps (default 1/4 step, SEQ-030 M p.40)
    bool locked = false; // any lock on the cell
};

struct StepGridModel
{
    int track = 1, bar = 1, length = 16; // bar 1–4 shown; pattern length in steps
    std::array<StepCellView, 16> cells{};
    std::uint16_t targets = 0;  // bit n−1: step n is a target (UID-755)
    std::uint16_t flashing = 0; // bit n−1: step n's ruler mark is boxed this frame (targets at 2 Hz)
    int playStep = 0;           // 1–16 in the shown bar while playing, 0 = none (UID-761)
    float playFraction = 0;     // position inside the playing step 0–1 (the ruler's playhead line)
    int heldStep = 0;           // 1–16 while a step key is held (focus ring), 0 = none
    std::uint32_t colour = 0;   // track colour
    bool muted = false;
};

class StepGrid final : public Widget
{
public:
    static constexpr RectF kPanel{16, 152, 1248, 336};
    static constexpr float kCellX0 = 72, kPitch = 72, kCellW = 56, kCellY = 200, kCellH = 88;
    static constexpr RectF cell(int n1) noexcept
    {
        return {kCellX0 + kPitch * static_cast<float>(n1 - 1), kCellY, kCellW, kCellH};
    }
    // Step 1–16 under a canvas point (the cell plus its 8 lpx gutters), 0 = none.
    static int stepAt(float x, float y) noexcept;

    explicit StepGrid(const StepGridModel* model = nullptr) noexcept : model_(model) {}
    RectF bounds() const noexcept override { return kPanel; }
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
