// The parts SAMPLING/EDIT and LIVE SAMPLING share (sampling.md "Local components"): the Rec Toggle (a rail
// Toggle whose LED is `rec` — off `border-strong`, standby blinking 1 Hz, recording solid — with a leading ● glyph),
// the left INPUT meter column (x 224, between the rail and the content) with the REC THRESHOLD line and the OVER
// caption, and the input level the capture hub publishes (post-limiter peaks, UI-side fall-off and clip latch).
// Header-only, message thread, no allocation.
#pragma once

#include "core/util/sampling/CaptureHub.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/RailItem.h"
#include "ui/widgets/Widget.h"

#include <algorithm>
#include <cmath>

namespace ui::m5
{
enum class RecLed : std::uint8_t
{
    Off,
    Standby,
    Recording
};

// The Rec Toggle at a rail slot (07-ui-design/_tools/kit_track.py rec_toggle): surface-2, LED 160 × 4 at y + 8,
// ● r 6 left of the label, label `REC` centred right of it. Standby blinks at 1 Hz (on: rec 50 %, off: border-strong).
inline void drawRecItem(DrawList& dl, const UiContext& ctx, RectF r, RecLed led, bool focused, double nowMs)
{
    namespace tk = ::theme::tokens;
    const auto& p = ctx.palette;
    dl.fill(r, p.surface2, tk::radiusS);
    const bool blinkOn = std::fmod(nowMs, 1000.0) < 500.0;
    const auto ledInk = led == RecLed::Recording                  ? p.rec
                        : led == RecLed::Standby && blinkOn       ? theme::withOpacity(p.rec, 0.5f)
                                                                  : p.borderStrong;
    dl.fill({r.x + 16, r.y + 8, r.w - 32, 4}, ledInk, 2);
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2 + 4;
    dl.fill({cx - 34, cy - 6, 12, 12}, led == RecLed::Off ? p.text3 : p.rec, 6);
    dl.text(cx + 6, cy, Align::Centre, TextStyleId::Label, p.text1, "REC");
    if (focused)
        draw::focusRing(dl, p, r, tk::radiusS);
}

// The input level of the capture hub with a UI-side fall-off (20 dB/s) and a 2 s clip latch.
struct InputLevel
{
    float db[2] = {-120.0f, -120.0f};
    double clipAtMs = -1e12, lastMs = 0;
    std::uint32_t clips = 0;

    void update(const core::sampling::CaptureHub* hub, double nowMs) noexcept
    {
        const double dt = std::clamp(nowMs - lastMs, 0.0, 200.0);
        lastMs = nowMs;
        const float peak[2] = {hub != nullptr ? hub->status.peakL.load(std::memory_order_relaxed) : 0.0f,
                               hub != nullptr ? hub->status.peakR.load(std::memory_order_relaxed) : 0.0f};
        for (int ch = 0; ch < 2; ++ch)
        {
            const float now = peak[ch] > 1e-6f ? 20.0f * std::log10(peak[ch]) : -120.0f;
            db[ch] = std::max(now, db[ch] - static_cast<float>(dt) * 0.02f);
            if (db[ch] < -119.0f)
                db[ch] = -120.0f;
        }
        if (hub != nullptr)
        {
            const auto c = hub->status.clips.load(std::memory_order_relaxed);
            if (c != clips)
                clipAtMs = nowMs;
            clips = c;
        }
    }
    bool clipped(double nowMs) const noexcept { return nowMs - clipAtMs < 2000.0; }
};

// The INPUT meter column at x 224 (bars centred at 252 over y 184–600; Meter::drawColumn), the REC THRESHOLD line (warn, 1 lpx, when the
// threshold is a level: −60 … −20 dB) and the OVER caption (warn) while the clip latch holds (sampling.md "States").
inline void drawInputMeter(DrawList& dl, const UiContext& ctx, const InputLevel& in, float thresholdDb)
{
    MeterModel m;
    m.caption = "INPUT";
    m.db[0] = in.db[0];
    m.db[1] = in.db[1];
    m.peakDb[0] = m.peakDb[1] = -120.0f;
    const bool clip = in.clipped(ctx.nowMs);
    m.clip[0] = m.clip[1] = clip;
    m.faderDb = -120.0f;
    Meter::drawColumn(dl, ctx, 224, m);
    if (thresholdDb > -120.0f)
    {
        const float y = meterY(std::max(thresholdDb, -50.0f), 184, 600);
        dl.fill({224 + Meter::kColCentre - 12, y - 0.5f, 24, 1}, ctx.palette.warn);
    }
    if (clip)
        dl.text(224 + Meter::kColCentre, 176, Align::Centre, TextStyleId::Caption, ctx.palette.warn, "OVER");
}

// REC THRESHOLD (sampling.threshold: 0 OFF, 1…41 = −60…−20 dB, 42 PLAY) as a level, −120 = no line.
inline float thresholdDb(float value) noexcept
{
    const int i = static_cast<int>(std::lround(value));
    return i >= 1 && i <= 41 ? static_cast<float>(i - 61) : -120.0f;
}
} // namespace ui::m5
