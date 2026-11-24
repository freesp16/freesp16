// pitchSplice (VINTAGE PITCH; ours): how the harmonizer makes a pitch, animated in three panels of the visual zone.
//
//   RAM       the input as written into the RAM, newest at the write head W (right edge), older to the left. The read
//             tap A sits DELAY + d behind W inside its 1K-word window (bracket, 30.6 ms); it drifts toward W (pitch
//             up) or away (pitch down) and jumps a window at the edge — a SPLICE (a dotted arc while it happens).
//   READ      the read delay d of tap A over time: a sawtooth whose slope is (ratio − 1) and whose teeth are the
//             splices; tap B (what is heard between crossfades) is A replayed 1024 reads later (ghost). Under it the
//             REFA crossfade: it starts half a window after A's jump and peaks when B reaches the same jump, so both
//             jumps happen where their tap is silent. AGE misaligns that trim: the peak drifts off B's jump.
//   D/A       the ratio and both clocks; a scope of the input (thin) against the D/A's staircase (the stored words
//             replayed at FIN: ratio × the frequency, coarser with AGE); the A/D LIMIT LED (live telemetry); the
//             FEEDBACK cascade — each pass through the loop climbs another PITCH, × |FEEDBACK| (inverted phase
//             alternates the bars, as the bus delay's L/R taps do).
//
// Animation: the splice rate |ratio − 1| · 33.5 kHz / 1024 is 1–33 per second, too fast to follow, so the clock runs
// in slow motion that puts one tooth in ~1.6 s (never faster than real time; the factor is printed). It runs while
// the transport plays or the slot hears signal (IN meter, LIMIT telemetry); idle, the frame stands still.
// Everything is rectangles and text (DrawList); the curves reuse preallocated Polylines: no allocation per frame.
#include "ui/screens/effects/visuals/VisualDraw.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace ui::fxvis
{
namespace
{
constexpr float kFref = 33488.0f;     // the A/D frame clock
constexpr float kWinWords = 1024;     // the pointer window
constexpr float kToothSeconds = 1.6f; // on-screen tooth period in slow motion

float frac(float x) noexcept
{
    return x - std::floor(x);
}

class PitchSplice final : public FxVisual
{
public:
    static constexpr int kSawPoints = 64;
    explicit PitchSplice(RectF zone) : FxVisual(zone), sawA_(kSawPoints), sawB_(kSawPoints), jump_(24), scopeIn_(97) {}
    std::string_view kind() const noexcept override { return "pitchSplice"; }
    bool setModel(const VisualModel& m) override
    {
        model_ = m;
        return true; // live: rebuilt per frame while animating (≈ 500 rects)
    }
    bool dirty(double) const override { return animating(); }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        advance(ctx.nowMs);
        const auto& p = ctx.palette;
        zoneBox(dl, p, zone_);
        const Clock c = clock();
        buildRam(dl, p, c);
        buildRead(dl, p, c);
        buildDac(dl, p, c);
    }

private:
    struct Clock
    {
        float ratio, rate; // rate = windows per real second the read pointer moves (+: toward W)
        float slow;        // real seconds per displayed second (≤ 1)
        float t;           // real time now (s)
        float fin;         // read clock (Hz)
        bool still;        // no shift: the pointer stands
    };

    bool animating() const noexcept
    {
        return model_.playing || model_.limit > 0.01f || std::max(model_.in[0], model_.in[1]) > 0.002f;
    }
    void advance(double nowMs) const
    {
        if (lastMs_ >= 0 && animating())
            displaySeconds_ += std::min(0.1, (nowMs - lastMs_) / 1000.0); // a stalled frame never jumps
        lastMs_ = nowMs;
    }
    Clock clock() const noexcept
    {
        Clock c{};
        c.ratio = std::clamp(model_.pitchRatio, 0.5f, 2.0f);
        c.rate = (c.ratio - 1.0f) * kFref / kWinWords;
        c.still = std::fabs(c.rate) < 1e-3f;
        const float tooth = c.still ? 1.0f : 1.0f / std::fabs(c.rate);
        c.slow = std::min(1.0f, tooth / kToothSeconds);
        c.t = static_cast<float>(std::fmod(displaySeconds_, 3600.0)) * c.slow;
        c.fin = c.ratio * kFref;
        return c;
    }
    // Tap A's position in its window at real time t: 0 = at the write side (just written), 1 = a window behind.
    static float pointer(const Clock& c, float t) noexcept { return c.still ? 0.5f : frac(0.5f - c.rate * t); }
    // Real time of the latest splice of tap A at or before t.
    static float lastSplice(const Clock& c, float t) noexcept
    {
        if (c.still)
            return -1e9f;
        const float k = std::floor(0.5f - c.rate * t); // the window index now
        const float edge = c.rate > 0 ? k + 1.0f : k;  // the integer last crossed
        return (0.5f - edge) / c.rate;
    }
    float xfadeStart() const noexcept { return 512.0f - std::round(160.0f * model_.pitchAge); } // the AGE trim
    // REFA at real time t (triangle of 1024 reads, starting xfadeStart() reads after the latest splice).
    float refA(const Clock& c, float t) const noexcept
    {
        const float s = lastSplice(c, t);
        const float reads = (t - s) * c.fin - xfadeStart();
        if (reads < 0 || reads >= 1024)
            return 0.0f;
        return (reads < 512 ? reads : 1024 - reads) / 512.0f;
    }

    // ------------------------------------------------------------------------------------------------ RAM panel
    void buildRam(DrawList& dl, const tk::Palette& p, const Clock& c) const
    {
        const float x0 = zone_.x + 20, x1 = zone_.x + 372, mid = zone_.y + 70, h = 34;
        const float presetWords = model_.pitchDelayMs * 0.001f * kFref;
        const float span = (presetWords + kWinWords) * 1.12f; // words shown, W at x1
        const auto wordX = [&](float age) { return x1 - age / span * (x1 - x0); };
        dl.text(x0, zone_.y + 14, Align::Left, TextStyleId::Caption, p.text2, "RAM");
        dl.text(x0 + 36, zone_.y + 14, Align::Left, TextStyleId::Caption, p.text3, "NEWEST \xe2\x86\x92 W");

        // the stored input: 1 bar per 4 lpx, the waveform written `age` words ago (scrolls left with the write clock)
        const float written = c.t * kFref;
        const float level = 0.75f;
        const float steps = 4.0f + 28.0f * (1.0f - model_.pitchAge); // coarser bars on a worn unit
        for (float x = x0; x < x1 - 2; x += 4)
        {
            const float age = (x1 - x) / (x1 - x0) * span;
            const float ph = (written - age) / kWinWords;
            float v = 0.65f * std::sin(6.2831853f * 5.0f * ph) + 0.35f * std::sin(6.2831853f * 11.0f * ph + 1.3f);
            v = std::round(v * steps) / steps;
            const float bh = std::max(1.0f, std::fabs(v) * level * h);
            dl.fill({x, mid - bh, 3, 2 * bh}, theme::withOpacity(p.text3, 0.55f * model_.opacity), 1);
        }
        hline(dl, x0, x1, mid, p.border);

        // window bracket (30.6 ms) and the DELAY gap
        const float wa = wordX(presetWords + kWinWords), wb = wordX(presetWords);
        const float by = mid + h + 12;
        hline(dl, wa, wb, by, ink(model_, p, 0.6f));
        vline(dl, wa, by - 5, by + 1, ink(model_, p, 0.6f));
        vline(dl, wb, by - 5, by + 1, ink(model_, p, 0.6f));
        dl.text((wa + wb) / 2, by + 12, Align::Centre, TextStyleId::Caption, p.text3, "WINDOW 30.6 ms");
        if (presetWords > 1)
        {
            hline(dl, wb + 3, x1, by, theme::withOpacity(p.text3, 0.6f * model_.opacity));
            char buf[24];
            std::snprintf(buf, sizeof buf, "+%d ms", static_cast<int>(std::lround(model_.pitchDelayMs)));
            dl.text((wb + x1) / 2, by - 10, Align::Centre, TextStyleId::Caption, p.text3, buf);
        }

        // W, tap A and the splice arc
        vline(dl, x1, mid - h - 8, mid + h + 4, p.text1, 2);
        dl.text(x1, mid - h - 16, Align::Centre, TextStyleId::Caption, p.text1, "W");
        const float wobble = 2.0f * model_.pitchAge * std::sin(c.t * 37.0f) * (c.still ? 0.0f : 1.0f);
        const float u = pointer(c, c.t);
        const float ax = wordX(presetWords + u * kWinWords) + wobble;
        const float since = c.t - lastSplice(c, c.t);
        const float arcLife = 0.25f * c.slow; // a quarter of a displayed second
        if (since < arcLife)
        {
            const float from = wordX(presetWords + (c.rate > 0 ? 0.0f : 1.0f) * kWinWords);
            const float to = wordX(presetWords + (c.rate > 0 ? 1.0f : 0.0f) * kWinWords);
            std::array<float, 24> xs{}, ys{};
            for (int i = 0; i < 24; ++i)
            {
                const float s = static_cast<float>(i) / 23.0f;
                xs[static_cast<std::size_t>(i)] = from + (to - from) * s;
                ys[static_cast<std::size_t>(i)] = mid - h - 6 - 22 * 4 * s * (1 - s);
            }
            jump_.build(xs.data(), ys.data(), 24, 2, NAN);
            jump_.emit(dl, ink(model_, p, 1.0f - since / arcLife), 0);
            dl.text((from + to) / 2, mid - h - 34, Align::Centre, TextStyleId::Caption,
                    theme::withOpacity(p.text1, (1.0f - since / arcLife) * model_.opacity), "SPLICE");
        }
        dl.fill({ax - 2, mid - h - 6, 4, 2 * h + 10}, ink(model_, p), 2);
        dl.text(ax, mid + h + 1, Align::Centre, TextStyleId::Caption, ink(model_, p), "A");
    }

    // ------------------------------------------------------------------------------------------------ READ panel
    void buildRead(DrawList& dl, const tk::Palette& p, const Clock& c) const
    {
        const float x0 = zone_.x + 440, x1 = zone_.x + 892;
        const float top = zone_.y + 30, bottom = zone_.y + 106, lane0 = zone_.y + 114, lane1 = zone_.y + 136;
        dl.text(zone_.x + 400, zone_.y + 14, Align::Left, TextStyleId::Caption, p.text2, "READ DELAY");
        char buf[40];
        if (c.still)
            std::snprintf(buf, sizeof buf, "NO SHIFT");
        else if (c.slow < 0.999f)
            std::snprintf(buf, sizeof buf,
                          "SLOW-MO \xc3\x97"
                          "1/%d",
                          static_cast<int>(std::lround(1.0f / c.slow)));
        else
            std::snprintf(buf, sizeof buf, "REAL TIME");
        dl.text(x1, zone_.y + 14, Align::Right, TextStyleId::Caption, p.text3, buf);
        const int d0 = static_cast<int>(std::lround(model_.pitchDelayMs));
        std::snprintf(buf, sizeof buf, "%d", d0 + 31);
        dl.text(x0 - 8, top + 4, Align::Right, TextStyleId::Caption, p.text3, buf);
        std::snprintf(buf, sizeof buf, "%d ms", d0);
        dl.text(x0 - 8, bottom - 4, Align::Right, TextStyleId::Caption, p.text3, buf);
        hline(dl, x0, x1, bottom, p.border);
        hline(dl, x0, x1, top, theme::withOpacity(p.border, 0.5f));
        dl.text(zone_.x + 400, (lane0 + lane1) / 2, Align::Left, TextStyleId::Caption, p.text3, "XFADE");

        // time axis: 2.5 teeth (or 60 ms when nothing moves), now at x1
        const float span = c.still ? 0.06f : 2.5f / std::fabs(c.rate);
        const float t0 = c.t - span;
        const auto tx = [&](float t) { return x0 + (t - t0) / span * (x1 - x0); };
        const auto uy = [&](float u) { return bottom - u * (bottom - top); };
        const float delayB = kWinWords / c.fin; // B = A, 1024 reads later
        buildSaw(c, t0, c.t, 0.0f, tx, uy, sawA_);
        buildSaw(c, t0, c.t, delayB, tx, uy, sawB_);
        sawB_.emit(dl, ink(model_, p, 0.35f), 0);
        sawA_.emit(dl, ink(model_, p), 0);

        // splice ticks and the crossfade lane (REFA, the staircase triangle)
        if (! c.still)
            for (float s = lastSplice(c, c.t); s > t0; s -= 1.0f / std::fabs(c.rate))
            {
                vline(dl, tx(s), bottom + 1, bottom + 6, p.text2, 2);
                vline(dl, tx(s + delayB), bottom + 1, bottom + 4, theme::withOpacity(p.text2, 0.45f));
            }
        hline(dl, x0, x1, lane1, p.border);
        for (float x = x0; x < x1; x += 3)
        {
            const float a = refA(c, t0 + (x - x0) / (x1 - x0) * span);
            if (a > 0)
                dl.fill({x, lane1 - a * (lane1 - lane0), 3, a * (lane1 - lane0)}, ink(model_, p, 0.45f));
        }
        // now: both taps' dots
        dl.fill({x1 - 4, uy(pointer(c, c.t)) - 4, 8, 8}, ink(model_, p), 4);
        dl.fill({x1 - 3, uy(pointer(c, c.t - delayB)) - 3, 6, 6}, ink(model_, p, 0.45f), 3);
        dl.text(x1 + 8, uy(pointer(c, c.t)), Align::Left, TextStyleId::Caption, ink(model_, p), "A");
        dl.text(x1 + 8,
                uy(pointer(c, c.t - delayB)) +
                    (std::fabs(pointer(c, c.t) - pointer(c, c.t - delayB)) < 0.12f ? 12.0f : 0.0f),
                Align::Left, TextStyleId::Caption, ink(model_, p, 0.55f), "B");
    }
    template <typename TX, typename UY>
    void buildSaw(const Clock& c, float t0, float t1, float lag, TX tx, UY uy, Polyline& out) const
    {
        std::array<float, kSawPoints> xs{}, ys{};
        int n = 0;
        auto put = [&](float t, float u)
        {
            if (n < kSawPoints)
            {
                xs[static_cast<std::size_t>(n)] = tx(t);
                ys[static_cast<std::size_t>(n)] = uy(u);
                ++n;
            }
        };
        put(t0, pointer(c, t0 - lag));
        if (! c.still)
        {
            const float period = 1.0f / std::fabs(c.rate);
            float s = lastSplice(c, t0 - lag) + period + lag; // the first jump after t0, on this tap's time
            for (; s < t1 && n + 2 < kSawPoints; s += period)
            {
                put(s, c.rate > 0 ? 0.0f : 1.0f);
                put(s, c.rate > 0 ? 1.0f : 0.0f);
            }
        }
        put(t1, pointer(c, t1 - lag));
        out.build(xs.data(), ys.data(), n, 2, NAN);
    }

    // ------------------------------------------------------------------------------------------------ D/A panel
    void buildDac(DrawList& dl, const tk::Palette& p, const Clock& c) const
    {
        const float x0 = zone_.x + 936, x1 = zone_.right() - 20;
        char buf[48];
        std::snprintf(buf, sizeof buf, "\xc3\x97%.3f", static_cast<double>(c.ratio));
        dl.text(x0, zone_.y + 18, Align::Left, TextStyleId::BodyStrong, p.text1, buf);
        std::snprintf(buf, sizeof buf, "FIN %.1f kHz \xc2\xb7 FREF 33.5 kHz", static_cast<double>(c.fin) / 1000.0);
        dl.text(x0, zone_.y + 38, Align::Left, TextStyleId::Caption, p.text3, buf);
        // LIMIT LED (live)
        const RectF led{x1 - 56, zone_.y + 8, 56, 20};
        const float lim = std::clamp(model_.limit, 0.0f, 1.0f);
        dl.fill(led, lim > 0.02f ? theme::withOpacity(p.rec, (0.35f + 0.65f * lim) * model_.opacity) : p.surface2, 4);
        dl.text(led.x + led.w / 2, led.y + led.h / 2, Align::Centre, TextStyleId::Caption,
                lim > 0.02f ? p.text1 : p.text3, "LIMIT");

        // scope: two input cycles (thin) and the D/A staircase at ratio × the frequency
        const float sy = zone_.y + 82, sh = 24, sw = x1 - x0;
        hline(dl, x0, x1, sy, p.border);
        const float scroll = frac(c.t * 0.5f / c.slow * 0.25f); // a slow drift, the same in both
        std::array<float, 97> xs{}, ys{};
        for (int i = 0; i < 97; ++i)
        {
            const float s = static_cast<float>(i) / 96.0f;
            xs[static_cast<std::size_t>(i)] = x0 + s * sw;
            ys[static_cast<std::size_t>(i)] = sy - sh * std::sin(6.2831853f * (2.0f * s + scroll));
        }
        scopeIn_.build(xs.data(), ys.data(), 97, 1, NAN);
        scopeIn_.emit(dl, theme::withOpacity(p.text3, 0.8f * model_.opacity), 0);
        const int steps = static_cast<int>(std::lround(22.0f * c.ratio));
        const float levels = 3.0f + 9.0f * (1.0f - model_.pitchAge); // ±levels: worn units show the dead LSBs
        const float stepW = sw / static_cast<float>(steps);
        float prev = NAN;
        for (int k = 0; k < steps; ++k)
        {
            const float s = (static_cast<float>(k) + 0.5f) / static_cast<float>(steps);
            const float v = std::round(std::sin(6.2831853f * (2.0f * c.ratio * s + scroll)) * levels) / levels;
            const float y = sy - sh * v, x = x0 + static_cast<float>(k) * stepW;
            hline(dl, x, x + stepW, y, ink(model_, p), 2);
            if (! std::isnan(prev))
                vline(dl, x, std::min(prev, y), std::max(prev, y), ink(model_, p, 0.6f), 1);
            prev = y;
        }

        // feedback cascade: each pass through the loop climbs one more PITCH, × |FEEDBACK| (the FB LEV ceiling 0.92)
        const float fy = zone_.y + 136;
        const float fb = std::clamp(model_.pitchFeedback, -1.0f, 1.0f);
        if (std::fabs(fb) < 0.005f)
        {
            dl.text(x0, fy - 12, Align::Left, TextStyleId::Caption, p.text3, "FEEDBACK OFF");
            return;
        }
        // pass k (0 = the first wet output) is k + 1 PITCHes up; bars about a baseline, up or (inverted) down
        const float base = fy - 12;
        dl.text(x0, base, Align::Left, TextStyleId::Caption, p.text3, "FB");
        hline(dl, x0 + 26, x1, base, theme::withOpacity(p.border, 0.6f));
        float g = 1.0f;
        const float semis = static_cast<float>(model_.pitchSemis) + static_cast<float>(model_.pitchCents) / 100.0f;
        for (int k = 0; k < 7; ++k)
        {
            const float bx = x0 + 40 + static_cast<float>(k) * 32;
            if (bx > x1 - 10)
                break;
            const float hgt = std::max(12 * g, 1.0f);
            const bool down = fb < 0 && (k % 2 == 1);
            dl.fill({bx, down ? base : base - hgt, 6, hgt}, ink(model_, p, k == 0 ? 1.0f : 0.8f), 2);
            std::snprintf(buf, sizeof buf, "%+.0f", static_cast<double>(semis * static_cast<float>(k + 1)));
            dl.text(bx + 3, fy + 8, Align::Centre, TextStyleId::Caption, p.text3, buf);
            g *= std::fabs(fb) * 0.92f;
            if (g < 0.03f)
                break;
        }
    }

    mutable double displaySeconds_ = 0.62; // the idle frame: mid-tooth
    mutable double lastMs_ = -1;
    mutable Polyline sawA_, sawB_, jump_, scopeIn_;
};

std::unique_ptr<Widget> makePitch(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<PitchSplice>(a.bounds.w > 0 ? a.bounds : fxzone::zone);
}
} // namespace
} // namespace ui::fxvis

APP_REGISTER_WIDGET("fxvisual.pitchSplice", &ui::fxvis::makePitch)
