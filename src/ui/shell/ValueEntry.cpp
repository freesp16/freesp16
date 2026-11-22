#include "ui/shell/ValueEntry.h"

#include "ui/shell/Layout.h"
#include "ui/shell/Timing.h"
#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
constexpr float kW = 240, kH = 96, kY = 520;
constexpr int kMaxCandidates = 4096;

// The leading number of a display text ("-6.0 dB" → −6, "L32" → −32, "R10" → 10, "C" → 0); false when none.
bool displayNumber(const char* s, double& out) noexcept
{
    while (*s == ' ')
        ++s;
    double sign = 1;
    if (*s == 'L' || *s == 'l')
        sign = -1, ++s;
    else if (*s == 'R' || *s == 'r')
        ++s;
    else if ((*s == 'C' || *s == 'c') && (s[1] == 0 || s[1] == ' '))
    {
        out = 0;
        return true;
    }
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s)
        return false;
    out = sign * v;
    return true;
}
} // namespace

ValueEntry::ValueEntry(int column0, const core::ParamSpec& spec, std::uint16_t scope, ParamAccess& values)
    : PopupWidget(PopupSpec::Kind::Dialog), column_(std::clamp(column0, 0, 5)), spec_(spec), scope_(scope),
      values_(values), binding_{static_cast<std::uint8_t>(column_), spec.id}
{
}

RectF ValueEntry::bounds() const noexcept
{
    const float x = std::clamp(L::columnX(column_) - 16, 0.0f, L::canvasW - kW);
    return place({x, kY, kW, kH});
}

bool ValueEntry::rawFor(const core::ParamSpec& s, double typed, float& raw) noexcept
{
    if (s.format == nullptr && s.enumLabels.empty())
    {
        raw = s.clamp(static_cast<float>(typed));
        return true;
    }
    // A formatted (or enumerated) value: the raw value whose display reads nearest to the typed number.
    const double range = static_cast<double>(s.maxValue) - static_cast<double>(s.minValue);
    const bool integral = s.type != core::ParamType::Continuous;
    const int n = integral ? std::min(kMaxCandidates, static_cast<int>(std::lround(range)) + 1) : kMaxCandidates;
    if (n <= 0)
        return false;
    bool found = false;
    double best = 0;
    for (int i = 0; i < n; ++i)
    {
        const float v = integral ? s.minValue + static_cast<float>(i)
                                 : s.minValue + static_cast<float>(range * i / std::max(1, n - 1));
        char buf[32];
        if (s.formatValue(v, buf, sizeof buf) <= 0)
            continue;
        double shown = 0;
        if (! displayNumber(buf, shown))
            continue;
        const double d = std::fabs(shown - typed);
        if (! found || d < best)
        {
            found = true;
            best = d;
            raw = v;
        }
    }
    return found;
}

void ValueEntry::apply()
{
    if (closed)
        return;
    closed = true;
    if (typed_.length == 0 || selectAll_)
        return; // nothing typed: the value stays
    double t = 0;
    if (! displayNumber(typed_.text, t))
        return;
    float raw = 0;
    if (rawFor(spec_, t, raw))
        values_.set(spec_, scope_, spec_.clamp(raw));
}

bool ValueEntry::text(std::string_view utf8)
{
    for (const char c : utf8)
    {
        const bool numeric = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+';
        if (! numeric)
            continue;
        if (selectAll_)
        {
            typed_.set({});
            selectAll_ = false;
        }
        char one[2] = {c, 0};
        if (typed_.length < 23)
        {
            FixedText<24> next;
            next.format("%s%s", typed_.text, one);
            typed_ = next;
        }
    }
    return true; // a value field takes every printable key
}

bool ValueEntry::textKey(TextKey k)
{
    switch (k)
    {
    case TextKey::Backspace:
        if (selectAll_)
        {
            typed_.set({});
            selectAll_ = false;
        }
        else if (typed_.length > 0)
            typed_.text[--typed_.length] = 0;
        return true;
    case TextKey::Return:
        apply();
        return true;
    case TextKey::Escape:
        cancel();
        return true;
    default:
        return true;
    }
}

bool ValueEntry::handle(const io::ControlEvent& e, UiContext&)
{
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        apply();
        return true;
    }
    if (e.control == io::Control::Back && e.kind == Kind::Press)
    {
        cancel();
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown && ! contains(bounds(), touch::x(e), touch::y(e)))
    {
        cancel(); // a tap outside cancels
        return true;
    }
    return true;
}

void ValueEntry::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto b = bounds();
    draw::panel(dl, p, b);
    dl.text(b.x + 16, b.y + 18, Align::Left, TextStyleId::Caption, p.text2, spec_.label);
    const RectF field{b.x + 16, b.y + 36, 208, 44};
    dl.fill(field, p.surface1, tk::radiusS);
    dl.stroke(field, p.accent, tk::borderFocus, tk::radiusS);
    char buf[32];
    std::string_view shown = typed_.view();
    if (selectAll_) // the whole current value, selected (accent-weak behind it)
    {
        const int len = std::max(0, spec_.formatValue(values_.get(spec_, scope_), buf, sizeof buf));
        shown = {buf, static_cast<std::size_t>(len)};
        const float w = ctx.measure ? ctx.measure(TextStyleId::ValueS, shown) : 8.0f * static_cast<float>(len);
        dl.fill({field.x + 10, field.y + 10, w + 4, 24}, p.accentWeak, tk::radiusXs);
    }
    dl.text(field.x + 12, field.y + 22, Align::Left, TextStyleId::ValueS, p.text1, shown);
    if (! selectAll_ && std::fmod(ctx.nowMs, timing::caretBlinkPeriodMs) < timing::caretBlinkPeriodMs / 2)
    {
        const float w = ctx.measure ? ctx.measure(TextStyleId::ValueS, shown) : 8.0f * static_cast<float>(shown.size());
        dl.fill({field.x + 13 + w, field.y + 10, 2, 24}, p.accent);
    }
    glyph::draw(dl, glyph::Kind::Return, field.x + field.w - 28, field.y + 12, 20, p.text3); // ↵ hint
}
} // namespace ui
