#include "ui/widgets/StatusRow.h"

#include "ui/shell/Layout.h"
#include "ui/shell/Timing.h"

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr std::string_view kChevron = "\xE2\x80\xBA";  // ›
constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // …
// A 12 lpx (glyph size) chevron with 8 lpx gaps on both sides (frame.md Breadcrumb), as the mockups place it.
constexpr float kGap = tk::space[2];
constexpr float kCpuGap = tk::space[1]; // between the CPU label and its value

float chevronW(const TextMeasure& m)
{
    return m ? m(TextStyleId::Label, kChevron) : 5.0f;
}

float widthOf(std::span<const std::string_view> crumbs, const int* idx, int n, const TextMeasure& m)
{
    float w = 0;
    for (int i = 0; i < n; ++i)
        w += m(TextStyleId::Label, idx[i] < 0 ? kEllipsis : crumbs[static_cast<std::size_t>(idx[i])]);
    return w + static_cast<float>(n - 1) * (kGap + chevronW(m) + kGap);
}
} // namespace

RectF StatusRow::bounds() const noexcept
{
    return layout::statusRow;
}

int StatusRow::layout(std::span<const std::string_view> crumbs, const TextMeasure& m,
                      std::array<int, kMaxSegments>& out)
{
    const int n = static_cast<int>(std::min(crumbs.size(), kMaxSegments));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] = i;
    if (n <= 2 || ! m || widthOf(crumbs, out.data(), n, m) <= layout::breadcrumbMaxW)
        return n;
    // HOME › … › tail segments: keep as many trailing segments as fit.
    int keep = n - 1;
    while (keep > 1)
    {
        std::array<int, kMaxSegments> t{};
        t[0] = 0;
        t[1] = -1;
        for (int k = 0; k < keep; ++k)
            t[static_cast<std::size_t>(2 + k)] = n - keep + k;
        if (keep < n - 1 && widthOf(crumbs, t.data(), keep + 2, m) <= layout::breadcrumbMaxW)
        {
            out = t;
            return keep + 2;
        }
        --keep;
    }
    out[0] = 0;
    out[1] = -1;
    out[2] = n - 1;
    return 3;
}

void StatusRow::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const float cy = layout::statusCentreY;
    std::array<int, kMaxSegments> idx{};
    const int n = layout(model_.breadcrumb, ctx.measure, idx);
    float x = layout::breadcrumbX;
    for (int i = 0; i < n; ++i)
    {
        const bool last = i == n - 1;
        const auto text = idx[static_cast<std::size_t>(i)] < 0
                              ? kEllipsis
                              : model_.breadcrumb[static_cast<std::size_t>(idx[static_cast<std::size_t>(i)])];
        dl.text(x, cy, Align::Left, TextStyleId::Label, last ? p.text1 : p.text2, text);
        if (last)
            break;
        x += (ctx.measure ? ctx.measure(TextStyleId::Label, text) : 0) + kGap;
        dl.text(x, cy, Align::Left, TextStyleId::Label, p.text3, kChevron);
        x += chevronW(ctx.measure) + kGap;
    }

    // Indicators, right to left: PLAY, REC, OUT, IN, then SYNC.
    float ix = layout::indicatorsRight;
    const double now = ctx.nowMs;
    dl.text(ix, cy, Align::Right, TextStyleId::Caption, model_.playing ? p.play : p.text3, "\xE2\x96\xB6"); // ▶
    ix -= layout::indicatorPitch;
    // REC: rec while armed; pulsing 1 Hz while armed and stopped (blink-slow).
    const bool recLit = model_.recArmed && (model_.playing || theme::blinkOn(now, tk::motionBlinkSlowMs));
    dl.text(ix, cy, Align::Right, TextStyleId::Caption, recLit ? p.rec : p.text3, "\xE2\x97\x8F"); // ●
    ix -= layout::indicatorPitch;
    dl.text(ix, cy, Align::Right, TextStyleId::Caption,
            now - model_.midiOutAtMs < timing::midiActivityMs ? p.accent : p.text3, "OUT");
    ix -= layout::indicatorPitch;
    dl.text(ix, cy, Align::Right, TextStyleId::Caption,
            now - model_.midiInAtMs < timing::midiActivityMs ? p.accent : p.text3, "IN");
    ix -= layout::indicatorPitch;
    if (! model_.syncBadge.empty())
    {
        dl.text(ix, cy, Align::Right, TextStyleId::Caption, model_.syncLost ? p.warn : p.text2, model_.syncBadge);
        ix -= layout::indicatorPitch;
    }
    const auto width = [&ctx](std::string_view s)
    { return ctx.measure ? ctx.measure(TextStyleId::Caption, s) : 8.0f * static_cast<float>(s.size()); };
    const auto drawProcessor =
        [&](std::string_view label, std::string_view load, bool highLoad, bool showTemp, std::optional<int> degrees)
    {
        if (load.empty() && ! showTemp)
            return;

        // Reserve only the enabled fields. Keep the label steady as digits change, but space the
        // visible values by their actual text widths.
        const bool bothReadings = ! load.empty() && showTemp;
        const float valueWidth =
            (load.empty() ? 0.0f : width("100%")) + (showTemp ? width("100°C") : 0.0f) + (bothReadings ? kGap : 0.0f);
        float x = ix - valueWidth;
        const float labelRight = x - kCpuGap;
        const float labelInset = bothReadings ? kCpuGap / 2 : 0.0f;
        dl.text(labelRight + labelInset, cy, Align::Right, TextStyleId::Caption, p.text3, label);
        if (! load.empty())
        {
            dl.text(x, cy, Align::Left, TextStyleId::Caption, highLoad ? p.warn : p.text2, load);
            x += width(load) + kGap;
        }
        if (showTemp)
        {
            FixedText<16> temp;
            if (degrees)
                temp.format("%d°C", *degrees);
            else
                temp.set("N/A");
            dl.text(x, cy, Align::Left, TextStyleId::Caption, p.text2, temp.view());
        }
        ix = labelRight - width(label) - tk::space[4];
    };
    const auto show = model_.showTemp;
    const auto& t = model_.temperatures;
    drawProcessor("GPU", {}, false, show.gpu, t.gpu);
    drawProcessor("CPU", model_.cpu, model_.cpuHigh, show.cpu || show.soc, show.cpu ? t.cpu : t.soc);
    // MUTE / SOLO: a filled badge, right-aligned before the indicators, the MUTE MODE detail after it.
    if (model_.padMute != UiState::PadMuteView::None)
    {
        const bool solo = model_.padMute == UiState::PadMuteView::Solo;
        const std::string_view label = solo ? "SOLO" : "MUTE";
        const auto detail = solo ? std::string_view{} : model_.muteMode;
        // In the breadcrumb's Label style, so the badge reads as well as the screen's name.
        const auto labelW = [&ctx](std::string_view s)
        { return ctx.measure ? ctx.measure(TextStyleId::Label, s) : 9.0f * static_cast<float>(s.size()); };
        float right = ix;
        if (! detail.empty())
        {
            dl.text(right, cy, Align::Right, TextStyleId::Label, p.text1, detail);
            right -= labelW(detail) + static_cast<float>(tk::space[3]);
        }
        const float w = labelW(label) + 2 * static_cast<float>(tk::space[4]), h = 30;
        dl.fill({right - w, cy - h / 2, w, h}, solo ? p.warn : p.accent, tk::radiusXs);
        dl.text(right - w / 2, cy, Align::Centre, TextStyleId::Label, p.onAccent, label);
    }
}

bool StatusRow::dirty(double nowMs) const
{
    return (model_.recArmed && ! model_.playing) || nowMs - model_.midiInAtMs < timing::midiActivityMs + 20 ||
           nowMs - model_.midiOutAtMs < timing::midiActivityMs + 20;
}
} // namespace ui
