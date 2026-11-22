// eqCurve and filterCurve (screens/effects.md "Visual kinds"): log-frequency grid (100, 1K, 10K) and the magnitude
// response, 2 lpx with a 12 % fill. 256 points, evaluated only when the model changes (≤ 0.3 ms on the A9).
//   eqCurve (2 BAND EQ): 0 dB line, the combined response of both bands, numbered handles ① ② (Ø 24) at each band's
//     frequency / gain.
//   filterCurve (FILTER): TYPE / SLOPE / RESONANCE / GAIN response, a 1 lpx text-2 cut-off line.
// The responses are display approximations (analogue prototypes), not the effect's exact transfer function.
#include "ui/screens/effects/visuals/VisualDraw.h"

#include <array>
#include <cmath>

namespace ui::fxvis
{
namespace
{
constexpr int kPoints = 256;

float frequencyAt(int i) noexcept
{
    return 20.0f * std::pow(10.0f, 3.0f * static_cast<float>(i) / (kPoints - 1));
}

// Second-order magnitude (dB) of a low/high/band-pass prototype at ratio r = f / fc.
float poleDb(int type, float r, float q) noexcept
{
    const float qq = std::max(q, 0.2f), a = 1 - r * r, b = r / qq;
    const float den = std::sqrt(a * a + b * b);
    float h = 1;
    if (type == 0)
        h = 1 / den; // low-pass
    else if (type == 1)
        h = r * r / den; // high-pass
    else
        h = b / den; // band-pass
    return 20 * std::log10(std::max(h, 1e-5f));
}

float eqBandDb(const VisualModel::EqBand& b, float f) noexcept
{
    const float r = f / std::max(b.freqHz, 1.0f);
    switch (b.type)
    {
    case 0: // low shelf
        return b.gainDb / (1 + r * r);
    case 1: // high shelf
        return b.gainDb * r * r / (1 + r * r);
    case 2: // peaking: a bell over log frequency, narrower with Q
    {
        const float o = std::log2(r) * std::max(b.q, 0.1f) * 1.4f;
        return b.gainDb / (1 + o * o);
    }
    case 3: // low-pass
        return poleDb(0, r, b.q);
    default: // high-pass
        return poleDb(1, r, b.q);
    }
}

class ResponseBase : public FxVisual
{
public:
    explicit ResponseBase(RectF zone) : FxVisual(zone), curve_(kPoints) {}

protected:
    float x0() const noexcept { return zone_.x + 24; }
    float x1() const noexcept { return zone_.right() - 24; }
    mutable Polyline curve_;
    std::array<float, kPoints> xs_{}, ys_{};
};

class EqCurve final : public ResponseBase
{
public:
    using ResponseBase::ResponseBase;
    std::string_view kind() const noexcept override { return "eqCurve"; }
    float zeroY() const noexcept { return zone_.y + zone_.h / 2; }
    static float clampDb(float db) noexcept { return std::clamp(db, -20.0f, 20.0f); }

    bool setModel(const VisualModel& m) override
    {
        bool changed = ! built_;
        for (int b = 0; b < 2; ++b)
        {
            const auto &n = m.band[static_cast<std::size_t>(b)], &o = model_.band[static_cast<std::size_t>(b)];
            changed |= n.type != o.type || n.freqHz != o.freqHz || n.gainDb != o.gainDb || n.q != o.q;
        }
        model_ = m;
        if (! changed)
            return false;
        built_ = true;
        for (int i = 0; i < kPoints; ++i)
        {
            const float f = frequencyAt(i);
            const float db = eqBandDb(model_.band[0], f) + eqBandDb(model_.band[1], f);
            xs_[static_cast<std::size_t>(i)] = logX(f, x0(), x1());
            ys_[static_cast<std::size_t>(i)] = zeroY() - clampDb(db) * 3.2f;
        }
        curve_.build(xs_.data(), ys_.data(), kPoints, 2, zeroY());
        return true;
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        zoneBox(dl, p, zone_);
        freqGrid(dl, p, x0(), x1(), zone_.y, zone_.bottom());
        hline(dl, x0(), x1(), zeroY(), p.borderStrong);
        dl.text(x0() - 4, zeroY(), Align::Right, TextStyleId::Caption, p.text3, "0");
        curve_.emit(dl, ink(model_, p), ink(model_, p, 0.12f));
        static constexpr std::string_view numbers[] = {"1", "2"};
        for (int b = 0; b < 2; ++b)
        {
            const auto& band = model_.band[static_cast<std::size_t>(b)];
            const float hx = logX(band.freqHz, x0(), x1()), hy = zeroY() - clampDb(band.gainDb * 0.5f) * 3.2f;
            dl.fill({hx - 12, hy - 12, 24, 24}, theme::withOpacity(p.surface3, model_.opacity), 12);
            dl.stroke({hx - 12, hy - 12, 24, 24}, ink(model_, p), 2, 12);
            dl.text(hx, hy + 1, Align::Centre, TextStyleId::Caption, p.text1, numbers[b]);
        }
    }

private:
    bool built_ = false;
};

class FilterCurve final : public ResponseBase
{
public:
    using ResponseBase::ResponseBase;
    std::string_view kind() const noexcept override { return "filterCurve"; }
    float topY() const noexcept { return zone_.y + 40; } // 0 dB
    float floorY() const noexcept { return zone_.bottom() - 8; }

    bool setModel(const VisualModel& m) override
    {
        const bool changed = ! built_ || m.filterType != model_.filterType || m.slopeDb != model_.slopeDb ||
                             m.cutoffHz != model_.cutoffHz || m.filterGainDb != model_.filterGainDb ||
                             m.resonance != model_.resonance;
        model_ = m;
        if (! changed)
            return false;
        built_ = true;
        const float fc = std::max(model_.cutoffHz, 1.0f);
        for (int i = 0; i < kPoints; ++i)
        {
            const float f = frequencyAt(i);
            float db = 0;
            if (model_.filterType < 3)
            {
                db = poleDb(model_.filterType, f / fc, model_.resonance);
                if (model_.slopeDb >= 24)
                    db *= 2;
            }
            db = std::max(-30.0f, db + model_.filterGainDb);
            xs_[static_cast<std::size_t>(i)] = logX(f, x0(), x1());
            ys_[static_cast<std::size_t>(i)] = std::clamp(topY() - db * 3.2f, zone_.y + 4, floorY());
        }
        curve_.build(xs_.data(), ys_.data(), kPoints, 2, floorY());
        return true;
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        zoneBox(dl, p, zone_);
        freqGrid(dl, p, x0(), x1(), zone_.y, zone_.bottom());
        curve_.emit(dl, ink(model_, p), ink(model_, p, 0.12f));
        if (model_.filterType < 3)
            vline(dl, logX(model_.cutoffHz, x0(), x1()), zone_.y + 8, zone_.bottom() - 8,
                  theme::withOpacity(p.text2, model_.opacity));
    }

private:
    bool built_ = false;
};

std::unique_ptr<Widget> makeEq(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<EqCurve>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
std::unique_ptr<Widget> makeFilter(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<FilterCurve>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
} // namespace
} // namespace ui::fxvis

APP_REGISTER_WIDGET("fxvisual.eqCurve", &ui::fxvis::makeEq)
APP_REGISTER_WIDGET("fxvisual.filterCurve", &ui::fxvis::makeFilter)
