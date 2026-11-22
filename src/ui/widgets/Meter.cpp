#include "ui/widgets/Meter.h"

#include "ui/shell/UiState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr double kClipLatchMs = EngineFeeds::kClipLatchMs;
} // namespace

float meterY(float db, float top, float bottom) noexcept
{
    const float span = bottom - top - 12.0f;
    if (db <= -50.0f)
        return bottom - 12.0f + std::min(12.0f, (-50.0f - db) * 0.4f);
    return top + (3.0f - std::min(db, 3.0f)) * span / 53.0f;
}

// --- drawing ---------------------------------------------------------------------------------------------------------
void Meter::drawBars(DrawList& dl, const UiContext& ctx, float x, float top, float bottom, float pitch,
                     const MeterModel& m)
{
    const auto& p = ctx.palette;
    const float op = m.dim ? 0.4f : 1.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float bx = x + pitch * static_cast<float>(ch);
        dl.fill({bx, top, 8, bottom - top}, p.surface3, 2);
        const float db = std::clamp(m.db[ch], -120.0f, 3.0f);
        if (db > -119.0f)
        {
            const float y = meterY(db, top, bottom), yMid = meterY(-6.0f, top, bottom),
                        yHigh = meterY(-1.0f, top, bottom);
            const float lowTop = std::max(y, yMid);
            dl.fill({bx, lowTop, 8, bottom - lowTop}, theme::withOpacity(p.meterLow, op), 2);
            if (y < yMid)
                dl.fill({bx, std::max(y, yHigh), 8, yMid - std::max(y, yHigh)}, theme::withOpacity(p.meterMid, op));
            if (y < yHigh)
                dl.fill({bx, y, 8, yHigh - y}, theme::withOpacity(p.meterHigh, op));
        }
        if (m.peakDb[ch] > -50.0f)
            dl.fill({bx, meterY(m.peakDb[ch], top, bottom) - 1, 8, 2}, theme::withOpacity(p.text1, op));
        if (m.clip[ch])
            dl.fill({bx, top, 8, 4}, p.meterHigh, 2);
    }
    if (m.faderDb > -119.0f) // left-pointing triangle 10 × 12 right of the bars
    {
        const float fy = meterY(m.faderDb, top, bottom), fx = x + pitch + 8 + (pitch - 8);
        for (int k = 0; k < 6; ++k)
        {
            const float w = 10.0f * static_cast<float>(k + 1) / 6.0f;
            dl.fill({fx + 10 - w, fy - 6 + static_cast<float>(k), w, 1}, p.text1);
            dl.fill({fx + 10 - w, fy + 5 - static_cast<float>(k), w, 1}, p.text1);
        }
    }
}

void Meter::drawColumn(DrawList& dl, const UiContext& ctx, float x, const MeterModel& m)
{
    const auto& p = ctx.palette;
    constexpr float top = 184, bottom = 600;
    dl.text(x + 44, 160, Align::Centre, TextStyleId::Caption, p.text2, m.caption);
    if (m.scale)
    {
        static constexpr struct
        {
            float db;
            const char* text;
        } labels[] = {{3, "3"}, {0, "0"}, {-10, "-10"}, {-20, "-20"}, {-30, "-30"}, {-40, "-40"}, {-50, "-50"}};
        for (const auto& l : labels)
            dl.text(x + 84, meterY(l.db, top, bottom), Align::Right, TextStyleId::Caption, p.text3, l.text);
    }
    drawBars(dl, ctx, x + 12, top, bottom, 12, m);
}

MeterModel Meter::bus(std::string_view binds, const EngineFeeds* feeds, const UiState* state, double nowMs) noexcept
{
    static const EngineFeeds silent{};
    const auto& f = feeds != nullptr ? *feeds : silent;
    const bool send = binds == "send";
    const auto& l = send ? f.send() : f.master();
    MeterModel m;
    m.caption = send ? std::string_view("SEND") : std::string_view("MASTER");
    for (int ch = 0; ch < 2; ++ch)
    {
        m.db[ch] = l.db[ch];
        m.peakDb[ch] = l.peakDb[ch];
        m.clip[ch] = nowMs - l.clipAtMs[ch] < kClipLatchMs;
    }
    m.faderDb = 0;
    if (! send && state != nullptr)
        m.faderDb = state->masterGain > 0 ? 20.0f * std::log10(state->masterGain) : -120.0f;
    return m;
}

void Meter::build(DrawList& dl, const UiContext& ctx) const
{
    drawColumn(dl, ctx, column_.x, bus(binds_, feeds_, state_, ctx.nowMs));
}

bool Meter::dirty(double nowMs) const
{
    if (feeds_ == nullptr)
        return false;
    const auto& l = binds_ == "send" ? feeds_->send() : feeds_->master();
    return l.db[0] > -120.0f || l.db[1] > -120.0f || nowMs - l.peakAtMs[0] < tk::motionPeakHoldMs + 20 ||
           nowMs - l.clipAtMs[0] < kClipLatchMs + 20 || nowMs - l.clipAtMs[1] < kClipLatchMs + 20;
}

namespace
{
std::unique_ptr<Widget> makeMeter(const ScreenSpec&, const WidgetArgs& a)
{
    const RectF column = a.bounds.w > 0 ? a.bounds : RectF{1176, 152, 88, 456};
    return std::make_unique<Meter>(column, a.binds.empty() ? std::string_view("master") : a.binds, a.state, a.feeds);
}
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("Meter", &ui::makeMeter)
