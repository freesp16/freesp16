// delayTaps and decayCurve (screens/effects.md "Visual kinds").
//   delayTaps (DELAY, insert and bus): a DRY bar (text-2), then one bar per repeat at TIME spacing, height × FEEDBACK
//     per repeat; window 2 s free, 1 bar synced with `play` beat lines (35 %, bars 70 %); the bus version alternates
//     L/R taps up and down around the centre when SPREAD > 0.
//   decayCurve (REVERB): DRY bar, the PRE DELAY gap, an exponential tail whose length = TIME and brightness = DAMPING,
//     16 % fill.
#include "ui/screens/effects/visuals/VisualDraw.h"

#include <array>
#include <cmath>

namespace ui::fxvis
{
namespace
{
class DelayTaps final : public FxVisual
{
public:
    using FxVisual::FxVisual;
    std::string_view kind() const noexcept override { return "delayTaps"; }
    bool setModel(const VisualModel& m) override
    {
        model_ = m;
        return true; // a handful of bars: no cache needed
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        zoneBox(dl, p, zone_);
        const float x0 = zone_.x + 24, x1 = zone_.right() - 24, w = x1 - x0;
        const bool split = model_.bus && model_.spread > 0;
        const float base = split ? zone_.y + zone_.h / 2 : zone_.bottom() - 16;
        const float full = split ? 56 : 112;
        if (model_.beatLines > 0)
            for (int i = 0; i <= model_.beatLines; ++i)
                vline(dl, x0 + w * static_cast<float>(i) / static_cast<float>(model_.beatLines), zone_.y + 8,
                      zone_.bottom() - 16, theme::withOpacity(p.play, (i % 4 ? 0.35f : 0.7f) * model_.opacity));
        hline(dl, x0, x1, base, p.borderStrong);
        dl.fill({x0, base - full, 6, full}, theme::withOpacity(p.text2, model_.opacity), 2);
        const float step = std::max(model_.tapFraction, 0.004f);
        float g = 1;
        for (int k = 1; k < 256; ++k)
        {
            const float t = step * static_cast<float>(k);
            if (k > 1)
                g *= std::clamp(model_.feedback, 0.0f, 0.99f);
            if (t > 1 || g < 0.03f)
                break;
            const float h = full * g * 0.9f, x = x0 + t * w - 3;
            const bool down = split && (k % 2 == 0);
            dl.fill({x, down ? base : base - h, 6, h}, ink(model_, p), 2);
        }
        dl.text(x0, zone_.y + 16, Align::Left, TextStyleId::Caption, p.text3, "DRY");
    }
};

class DecayCurve final : public FxVisual
{
public:
    static constexpr int kPoints = 122;
    explicit DecayCurve(RectF zone) : FxVisual(zone), tail_(kPoints) {}
    std::string_view kind() const noexcept override { return "decayCurve"; }
    bool setModel(const VisualModel& m) override
    {
        const bool changed =
            ! built_ || m.preDelayFraction != model_.preDelayFraction || m.lengthFraction != model_.lengthFraction;
        model_ = m;
        if (! changed)
            return false;
        built_ = true;
        const float x0 = zone_.x + 24, x1 = zone_.right() - 24, base = zone_.bottom() - 16;
        const float px = x0 + std::clamp(model_.preDelayFraction, 0.0f, 1.0f) * (x1 - x0) + 12;
        const float len = std::max(model_.lengthFraction, 0.02f);
        xs_[0] = px;
        ys_[0] = base;
        for (int i = 0; i + 1 < kPoints; ++i)
        {
            const float t = static_cast<float>(i) / (kPoints - 2);
            xs_[static_cast<std::size_t>(i + 1)] = px + t * (x1 - px);
            ys_[static_cast<std::size_t>(i + 1)] = base - 104 * std::exp(-5 * t / len);
        }
        tail_.build(xs_.data(), ys_.data(), kPoints, 2, base);
        preX_ = px;
        return true;
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        zoneBox(dl, p, zone_);
        const float x0 = zone_.x + 24, x1 = zone_.right() - 24, base = zone_.bottom() - 16;
        hline(dl, x0, x1, base, p.borderStrong);
        dl.fill({x0, base - 112, 6, 112}, theme::withOpacity(p.text2, model_.opacity), 2);
        const float bright = 1 - 0.5f * std::clamp(model_.damping, 0.0f, 1.0f); // damping darkens the tail
        tail_.emit(dl, ink(model_, p, bright), ink(model_, p, 0.16f));
        dl.text(x0, zone_.y + 16, Align::Left, TextStyleId::Caption, p.text3, "DRY");
        dl.text(preX_ + 8, zone_.y + 16, Align::Left, TextStyleId::Caption, p.text3, "PRE DELAY");
    }

private:
    bool built_ = false;
    float preX_ = 0;
    Polyline tail_;
    std::array<float, kPoints> xs_{}, ys_{};
};

std::unique_ptr<Widget> makeDelay(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<DelayTaps>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
std::unique_ptr<Widget> makeDecay(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<DecayCurve>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
} // namespace
} // namespace ui::fxvis

APP_REGISTER_WIDGET("fxvisual.delayTaps", &ui::fxvis::makeDelay)
APP_REGISTER_WIDGET("fxvisual.decayCurve", &ui::fxvis::makeDecay)
