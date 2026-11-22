// lfoCurve (screens/effects.md "Visual kinds"; CHORUS, FLANGER, PHASER): a 2 lpx L waveform in the slot ink over a
// 1 lpx centre line, R at 45 % shifted by PHASE; cycles across the zone = RATE (free: 4 s window, sync: 4 bars),
// amplitude = WIDTH. While the transport plays the waveform scrolls by the window per 4 s / 4 bars.
#include "ui/screens/effects/visuals/VisualDraw.h"

#include <array>
#include <cmath>

namespace ui::fxvis
{
namespace
{
class LfoCurve final : public FxVisual
{
public:
    static constexpr int kPoints = 241;
    explicit LfoCurve(RectF zone) : FxVisual(zone), left_(kPoints), right_(kPoints) { model_.cycles = -1; }
    std::string_view kind() const noexcept override { return "lfoCurve"; }

    bool setModel(const VisualModel& m) override
    {
        const bool changed = m.cycles != model_.cycles || m.depth != model_.depth || m.phase != model_.phase ||
                             m.playing || model_.playing != m.playing;
        model_ = m;
        if (changed)
            rebuild(0);
        return changed;
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        if (model_.playing)
            rebuild(ctx.nowMs - model_.lfoStartMs);
        zoneBox(dl, p, zone_);
        const float mid = zone_.y + zone_.h / 2;
        hline(dl, zone_.x + 16, zone_.right() - 16, mid, p.border);
        right_.emit(dl, ink(model_, p, 0.45f), 0);
        left_.emit(dl, ink(model_, p), 0);
        dl.text(zone_.x + 16, zone_.y + 16, Align::Left, TextStyleId::Caption, p.text2, "L");
        dl.text(zone_.x + 32, zone_.y + 16, Align::Left, TextStyleId::Caption, p.text3, "R");
    }
    bool dirty(double) const override { return model_.playing; }

private:
    void rebuild(double elapsedMs) const
    {
        const float x0 = zone_.x + 16, w = zone_.w - 32, mid = zone_.y + zone_.h / 2;
        const float amp = 56 * std::clamp(model_.depth, 0.0f, 1.0f);
        const float scroll = static_cast<float>(std::fmod(elapsedMs / 4000.0, 1.0)) * model_.cycles;
        for (int side = 0; side < 2; ++side)
        {
            const float ph = side == 0 ? 0 : model_.phase;
            for (int i = 0; i < kPoints; ++i)
            {
                const float t = static_cast<float>(i) / (kPoints - 1);
                xs_[static_cast<std::size_t>(i)] = x0 + t * w;
                ys_[static_cast<std::size_t>(i)] = mid - amp * std::sin(6.2831853f * (model_.cycles * t + ph + scroll));
            }
            (side == 0 ? left_ : right_).build(xs_.data(), ys_.data(), kPoints, 2, NAN);
        }
    }
    mutable Polyline left_, right_;
    mutable std::array<float, kPoints> xs_{}, ys_{};
};

std::unique_ptr<Widget> make(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<LfoCurve>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
} // namespace
} // namespace ui::fxvis

APP_REGISTER_WIDGET("fxvisual.lfoCurve", &ui::fxvis::make)
