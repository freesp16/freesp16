// Meter (components/visuals.md "Meter"; home.md, mixer.md; UI-216, UID-503): the stereo level column of HOME, TRACK
// SETTING and MIXER — two 8 lpx bars on a dB-linear scale (+3 dB at the top, −50 dB 12 lpx above the bottom, −∞ at
// the bottom), meter-low / -mid / -high zones, a 2 lpx peak line held 500 ms (motion.peak-hold), a 2 s clip latch and
// the left-pointing fader marker. The same scale draws the small ChannelStrip meters (ChannelStrip.h).
//
// The engine feeds (levels with UI-side decay, peak hold, clip latch, play positions) are the Shell's EngineFeeds
// (ui/shell/Feeds.h), polled once per frame and reached through WidgetArgs::feeds / ShellApi::feeds().
#pragma once

#include "engine/EngineApi.h"
#include "ui/shell/Feeds.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ui
{
struct UiState;

// dB ↔ y on a meter column whose bars span [top, bottom]: +3 dB at top, −50 dB at bottom − 12, −∞ at bottom.
float meterY(float db, float top, float bottom) noexcept;

struct MeterModel
{
    std::string_view caption = "MASTER";
    float db[2] = {-120.0f, -120.0f}, peakDb[2] = {-120.0f, -120.0f};
    bool clip[2] = {false, false};
    float faderDb = 0.0f; // fader marker position; < −119 = no marker
    bool scale = true;    // dB labels right of the bars
    bool dim = false;     // 40 % (muted strip, non-soloed strip while a solo is on)
    bool markerOver = false; // fader marker as a cap across the bars instead of a triangle right of them
};

class Meter final : public Widget
{
public:
    Meter(RectF column, std::string_view binds, const UiState* state, const EngineFeeds* feeds) noexcept
        : column_(column), binds_(binds), state_(state), feeds_(feeds)
    {
    }
    RectF bounds() const noexcept override { return column_; }
    void build(DrawList&, const UiContext&) const override;
    bool dirty(double nowMs) const override;

    // A full 88-wide meter column at x, y 152–608: caption and bars centred at x + kColCentre (two 8 lpx bars on a
    // 10 lpx pitch, the fader marker a cap across them), the dB scale right-aligned at x + kColScaleR — the same
    // look as the MIXER's bus meters (drawBusPair).
    static constexpr float kColCentre = 28, kColScaleR = 86;
    static void drawColumn(DrawList&, const UiContext&, float x, const MeterModel&);
    // Two bars of width 8 at x and x + pitch over [top, bottom] (+ peak lines, clip caps); the fader marker right of
    // the last bar.
    static void drawBars(DrawList&, const UiContext&, float x, float top, float bottom, float pitch, const MeterModel&);
    // The compact bus pair right of the MIXER / OUTPUT SETTING strips (template rail+content+meters2, its meter and
    // meter2 columns): two 48-wide columns at kBusX and kBusX + kBusPitch, captions at y 160 and the bars centred under
    // them over y 184–600 with the fader marker across the bars — bars only, no dB scale (hardware feedback: the
    // room goes to the strips).
    static constexpr float kBusX = 1168, kBusPitch = 48, kBusW = 48;
    static constexpr float kBusTop = 184, kBusBottom = 600;
    static void drawBusPair(DrawList&, const UiContext&, std::string_view leftCaption, const MeterModel& left,
                            std::string_view rightCaption, const MeterModel& right);
    // The model of the MASTER (binds "master") or SEND (binds "send") meter at nowMs (feeds null = silent).
    static MeterModel bus(std::string_view binds, const EngineFeeds* feeds, const UiState* state,
                          double nowMs) noexcept;

private:
    RectF column_;
    std::string_view binds_;
    const UiState* state_;
    const EngineFeeds* feeds_;
};
} // namespace ui
