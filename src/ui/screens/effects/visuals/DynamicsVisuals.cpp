// meters and duckEnvelope (screens/effects.md "Visual kinds", UID-834). Both read the slot's live telemetry
// (FxMeterView) once per frame through the model; the static parts are cached, the live parts (bars,
// the envelope dot, the chip flash) are a few rectangles.
//   meters (COMPRESSOR): TRANSFER curve 128 × 128 over columns 1–2 (THRESHOLD, RATIO, knee); IN (stereo), GR (from the
//     top down, text-2) and OUT (stereo) bars 8 lpx wide at x 760 / 848 / 936, y 364–476, a text-1 threshold marker
//     on IN.
//   duckEnvelope (DUCKER): SOURCE TRIGGER caption + 40 × 40 chip of the source track at (100, 384) whose frame flashes
//     on every source trigger; graph x 248–1232 with the gain-reduction axis 0 / 50 / 100, the duck shape (ATTACK,
//     CURVE, RELEASE, CURVE, DEPTH) 2 lpx in the slot ink and a `playhead` dot riding the live envelope.
#include "ui/screens/effects/visuals/VisualDraw.h"
#include "ui/shell/UiState.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace ui::fxvis
{
namespace
{
constexpr double kFlashMs = 150;

class Meters final : public FxVisual
{
public:
    static constexpr int kPoints = 65;
    explicit Meters(RectF zone) : FxVisual(zone), transfer_(kPoints), diagonal_(kPoints) {}
    std::string_view kind() const noexcept override { return "meters"; }
    float gx0() const noexcept { return zone_.x + 32; }
    float gy0() const noexcept { return zone_.y + 12; }
    static constexpr float kGraph = 128;

    bool setModel(const VisualModel& m) override
    {
        const bool changed =
            ! built_ || m.threshold != model_.threshold || m.ratio != model_.ratio || m.softKnee != model_.softKnee;
        model_ = m;
        if (! changed)
            return false;
        built_ = true;
        const float thr = std::clamp(model_.threshold, 0.0f, 1.0f), ratio = std::max(model_.ratio, 1.0f);
        const float knee = model_.softKnee ? 0.18f : 0.0f;
        for (int i = 0; i < kPoints; ++i)
        {
            const float in = static_cast<float>(i) / (kPoints - 1);
            float out = in;
            if (in > thr + knee / 2)
                out = thr + (in - thr) / ratio;
            else if (knee > 0 && in > thr - knee / 2) // quadratic soft knee
            {
                const float d = in - thr + knee / 2;
                out = in + (1 / ratio - 1) * d * d / (2 * knee);
            }
            xs_[static_cast<std::size_t>(i)] = gx0() + in * kGraph;
            ys_[static_cast<std::size_t>(i)] = gy0() + kGraph - out * kGraph;
            dx_[static_cast<std::size_t>(i)] = xs_[static_cast<std::size_t>(i)];
            dy_[static_cast<std::size_t>(i)] = gy0() + kGraph - in * kGraph;
        }
        transfer_.build(xs_.data(), ys_.data(), kPoints, 2, NAN);
        diagonal_.build(dx_.data(), dy_.data(), kPoints, 1, NAN);
        return true;
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        zoneBox(dl, p, zone_);
        dl.fill({gx0(), gy0(), kGraph, kGraph}, p.surface1, 4);
        diagonal_.emit(dl, p.border, 0);
        transfer_.emit(dl, ink(model_, p), 0);
        dl.text(gx0() + kGraph + 12, gy0() + 8, Align::Left, TextStyleId::Caption, p.text3, "TRANSFER");
        const float top = zone_.y + 28, bot = zone_.bottom() - 12, h = bot - top;
        const std::string_view labels[] = {"IN", "GR", "OUT"};
        for (int i = 0; i < 3; ++i)
        {
            const float mx = 760 + 88 * static_cast<float>(i);
            dl.text(mx + 9, zone_.y + 14, Align::Centre, TextStyleId::Caption, p.text2, labels[i]);
            if (i == 1)
            {
                dl.fill({mx + 4, top, 8, h}, p.surface3, 2);
                const float gr = std::clamp(model_.gainReduction, 0.0f, 1.0f) * h;
                if (gr > 0.5f)
                    dl.fill({mx + 4, top, 8, gr}, p.text2, 2);
                continue;
            }
            const auto& lv = i == 0 ? model_.in : model_.out;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float bx = mx + 12 * static_cast<float>(ch),
                            v = std::clamp(lv[static_cast<std::size_t>(ch)], 0.0f, 1.0f) * h;
                dl.fill({bx, top, 8, h}, p.surface3, 2);
                if (v > 0.5f)
                    dl.fill({bx, bot - v, 8, v}, p.meterLow, 2);
            }
            if (i == 0) // threshold marker: a small left-pointing triangle
            {
                const float ty = bot - h * std::clamp(model_.threshold, 0.0f, 1.0f);
                for (int k = 0; k < 5; ++k)
                    dl.fill({mx + 24 + 2 * static_cast<float>(k), ty - static_cast<float>(k) - 1, 2,
                             2 * static_cast<float>(k) + 2},
                            p.text1);
            }
        }
    }
    bool dirty(double) const override { return model_.playing; }

private:
    bool built_ = false;
    Polyline transfer_, diagonal_;
    std::array<float, kPoints> xs_{}, ys_{}, dx_{}, dy_{};
};

class DuckEnvelope final : public FxVisual
{
public:
    static constexpr int kPoints = 161;
    explicit DuckEnvelope(RectF zone) : FxVisual(zone), shape_(kPoints) {}
    std::string_view kind() const noexcept override { return "duckEnvelope"; }
    float gx0() const noexcept { return 248; }
    float gx1() const noexcept { return 1232; }
    float gy0() const noexcept { return zone_.y + 12; }
    float gy1() const noexcept { return zone_.bottom() - 12; }

    bool setModel(const VisualModel& m) override
    {
        const bool changed = ! built_ || m.attackFraction != model_.attackFraction ||
                             m.releaseFraction != model_.releaseFraction || m.attackCurve != model_.attackCurve ||
                             m.releaseCurve != model_.releaseCurve || m.duckDepth != model_.duckDepth;
        model_ = m;
        number_.format("%d", model_.sourceTrack);
        if (! changed)
            return false;
        built_ = true;
        const float w = gx1() - gx0(), depth = std::clamp(model_.duckDepth, 0.0f, 1.0f);
        const float floor = gy0() + (gy1() - gy0()) * depth;
        ax_ = gx0() + 8 + w * std::clamp(model_.attackFraction, 0.0f, 0.5f);
        rx_ = ax_ + w * std::clamp(model_.releaseFraction, 0.01f, 0.9f);
        floor_ = floor;
        // Curves −1…+1 bend the segments (0 = the reference's linear attack / exponential-looking release).
        auto bend = [](float t, float c) { return c >= 0 ? std::pow(t, 1 + 2 * c) : 1 - std::pow(1 - t, 1 - 2 * c); };
        int n = 0;
        auto add = [&](float x, float y)
        {
            xs_[static_cast<std::size_t>(n)] = x;
            ys_[static_cast<std::size_t>(n)] = y;
            ++n;
        };
        add(gx0(), gy0());
        add(gx0() + 8, gy0());
        constexpr int kSeg = 70;
        for (int i = 1; i <= kSeg; ++i)
        {
            const float t = static_cast<float>(i) / kSeg;
            add(gx0() + 8 + (ax_ - gx0() - 8) * t, gy0() + (floor - gy0()) * bend(t, model_.attackCurve));
        }
        for (int i = 1; i <= kSeg; ++i)
        {
            const float t = static_cast<float>(i) / kSeg;
            const float r = 1 - std::pow(1 - t, 2.2f); // recovery shape of the release
            add(ax_ + (rx_ - ax_) * t, floor + (gy0() - floor) * bend(r, model_.releaseCurve));
        }
        add(gx1(), gy0());
        shape_.build(xs_.data(), ys_.data(), n, 2, NAN);
        return true;
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        zoneBox(dl, p, zone_);
        const auto src = model_.sourceColour != 0 ? model_.sourceColour : ink(model_, p);
        dl.text(120, zone_.y + 22, Align::Centre, TextStyleId::Caption, p.text2, "SOURCE TRIGGER");
        dl.fill({100, zone_.y + 48, 40, 40}, src, 6);
        dl.text(120, zone_.y + 68, Align::Centre, TextStyleId::Label, p.bg, number_.view());
        const bool flash = ctx.nowMs - model_.lastTriggerMs < kFlashMs;
        dl.stroke({96, zone_.y + 44, 48, 48}, flash ? p.text1 : src, 2, 6);
        dl.text(120, zone_.y + 112, Align::Centre, TextStyleId::Caption, p.text3, "flashes on hit");
        static constexpr std::string_view marks[] = {"0", "50", "100"};
        for (int v = 0; v < 3; ++v)
        {
            const float y = gy0() + (gy1() - gy0()) * 0.5f * static_cast<float>(v);
            hline(dl, gx0(), gx1(), y, p.border);
            dl.text(gx0() - 8, y, Align::Right, TextStyleId::Caption, p.text3, marks[v]);
        }
        shape_.emit(dl, ink(model_, p), 0);
        // The dot rides the live envelope along the release (idle: halfway down the release, as drawn in the spec).
        const float depth = std::max(model_.duckDepth, 0.01f);
        const float env = model_.playing ? std::clamp(model_.envelope / depth, 0.0f, 1.0f) : 0.55f;
        const float x = ax_ + (rx_ - ax_) * (1 - env) * 0.6f, y = gy0() + (floor_ - gy0()) * env;
        dl.fill({x - 5, y - 5, 10, 10}, p.playhead, 5);
    }
    bool dirty(double) const override { return model_.playing; }

private:
    bool built_ = false;
    float ax_ = 0, rx_ = 0, floor_ = 0;
    Polyline shape_;
    std::array<float, kPoints> xs_{}, ys_{};
    FixedText<4> number_;
};

std::unique_ptr<Widget> makeMeters(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<Meters>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
std::unique_ptr<Widget> makeDuck(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<DuckEnvelope>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
} // namespace
} // namespace ui::fxvis

APP_REGISTER_WIDGET("fxvisual.meters", &ui::fxvis::makeMeters)
APP_REGISTER_WIDGET("fxvisual.duckEnvelope", &ui::fxvis::makeDuck)
