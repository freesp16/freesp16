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
    if (m.faderDb > -119.0f && m.markerOver) // a 3 lpx cap 4 lpx wider than the bars each side, outlined in bg
    {
        const float fy = meterY(m.faderDb, top, bottom), w = pitch + 8 + 8;
        dl.fill({x - 4, fy - 2.5f, w + 2, 5}, p.bg, 1.5f);
        dl.fill({x - 3, fy - 1.5f, w, 3}, theme::withOpacity(p.text1, m.dim ? 0.6f : 1.0f), 1.0f);
    }
    else if (m.faderDb > -119.0f) // left-pointing triangle 10 × 12 right of the bars
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

namespace
{
void drawScale(DrawList& dl, const UiContext& ctx, float right, float top, float bottom)
{
    static constexpr struct
    {
        float db;
        const char* text;
    } labels[] = {{3, "3"}, {0, "0"}, {-10, "-10"}, {-20, "-20"}, {-30, "-30"}, {-40, "-40"}, {-50, "-50"}};
    for (const auto& l : labels)
        dl.text(right, meterY(l.db, top, bottom), Align::Right, TextStyleId::Caption, ctx.palette.text3, l.text);
    dl.text(right, bottom + 6, Align::Right, TextStyleId::Caption, ctx.palette.text3, "\xE2\x88\x92\xE2\x88\x9E");
}
} // namespace

void Meter::drawColumn(DrawList& dl, const UiContext& ctx, float x, const MeterModel& m)
{
    const float cx = x + kColCentre;
    dl.text(cx, 160, Align::Centre, TextStyleId::Caption, ctx.palette.text2, m.caption);
    if (m.scale)
        drawScale(dl, ctx, x + kColScaleR, kBusTop, kBusBottom);
    auto mm = m;
    mm.markerOver = true;
    drawBars(dl, ctx, cx - 9, kBusTop, kBusBottom, 10, mm);
}

void Meter::drawBusPair(DrawList& dl, const UiContext& ctx, std::string_view leftCaption, const MeterModel& left,
                        std::string_view rightCaption, const MeterModel& right)
{
    const struct
    {
        std::string_view caption;
        const MeterModel& m;
    } cols[] = {{leftCaption, left}, {rightCaption, right}};
    for (int i = 0; i < 2; ++i)
    {
        const float cx = kBusX + kBusPitch * static_cast<float>(i) + kBusW / 2;
        dl.text(cx, 160, Align::Centre, TextStyleId::Caption, ctx.palette.text2, cols[i].caption);
        auto m = cols[i].m;
        m.markerOver = true;
        drawBars(dl, ctx, cx - 9, kBusTop, kBusBottom, 10, m);
    }
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
    const auto m = bus(binds_, feeds_, state_, ctx.nowMs);
    if (ctx.palette.classic && column_.x == 1176)
    {
        constexpr float top = 221, bottom = 606;
        const float x = column_.x;
        dl.text(x + 38, 183, Align::Centre, TextStyleId::ValueS, ctx.palette.text2, m.caption);
        for (const int db : {3, 0, -10, -20, -30, -40, -50})
        {
            char label[8];
            std::snprintf(label, sizeof label, "%d", db);
            const float y = meterY(static_cast<float>(db), top, bottom);
            dl.text(x + 25, y, Align::Right, TextStyleId::Body, ctx.palette.text2, label);
            dl.fill({x + 29, y, 5, 1}, ctx.palette.borderStrong);
        }
        drawBars(dl, ctx, x + 38, top, bottom, 16, m);
        return;
    }
    drawColumn(dl, ctx, column_.x, m);
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
