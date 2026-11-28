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

float chevronW(const TextMeasure& m, TextStyleId style)
{
    return m ? m(style, kChevron) : 5.0f;
}

float widthOf(std::span<const std::string_view> crumbs, const int* idx, int n, const TextMeasure& m, TextStyleId style)
{
    float w = 0;
    for (int i = 0; i < n; ++i)
        w += m(style, idx[i] < 0 ? kEllipsis : crumbs[static_cast<std::size_t>(idx[i])]);
    return w + static_cast<float>(n - 1) * (kGap + chevronW(m, style) + kGap);
}
} // namespace

RectF StatusRow::bounds() const noexcept
{
    return layout::statusRow;
}

int StatusRow::layout(std::span<const std::string_view> crumbs, const TextMeasure& m,
                      std::array<int, kMaxSegments>& out, TextStyleId style)
{
    const int n = static_cast<int>(std::min(crumbs.size(), kMaxSegments));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] = i;
    if (n <= 2 || ! m || widthOf(crumbs, out.data(), n, m, style) <= layout::breadcrumbMaxW)
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
        if (keep < n - 1 && widthOf(crumbs, t.data(), keep + 2, m, style) <= layout::breadcrumbMaxW)
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
    const auto style = p.classic ? TextStyleId::ValueS : TextStyleId::Label;
    const int n = layout(model_.breadcrumb, ctx.measure, idx, style);
    if (p.classic)
        dl.fill(layout::statusRow, p.surface1);
    float x = p.classic ? 29.0f : layout::breadcrumbX;
    for (int i = 0; i < n; ++i)
    {
        const bool last = i == n - 1;
        const auto text = idx[static_cast<std::size_t>(i)] < 0
                              ? kEllipsis
                              : model_.breadcrumb[static_cast<std::size_t>(idx[static_cast<std::size_t>(i)])];
        dl.text(x, cy, Align::Left, style, last ? p.text1 : p.text2, text);
        if (last)
            break;
        x += (ctx.measure ? ctx.measure(style, text) : 0) + kGap;
        dl.text(x, cy, Align::Left, style, p.text3, kChevron);
        x += chevronW(ctx.measure, style) + kGap;
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
    // CPU: the value left-aligned in a fixed "100%" field ending at the slot, so the label does not move as the digits
    // change.
    const auto width = [&ctx](std::string_view s)
    { return ctx.measure ? ctx.measure(TextStyleId::Caption, s) : 8.0f * static_cast<float>(s.size()); };
    if (! model_.cpu.empty())
    {
        const float field = width("100%");
        dl.text(ix - field, cy, Align::Left, TextStyleId::Caption, model_.cpuHigh ? p.warn : p.text2, model_.cpu);
        dl.text(ix - field - kCpuGap, cy, Align::Right, TextStyleId::Caption, p.text3, "CPU");
        ix -= field + kCpuGap + width("CPU") + tk::space[5];
    }
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
