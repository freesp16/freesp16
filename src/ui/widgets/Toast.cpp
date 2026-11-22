#include "ui/widgets/Toast.h"

#include "ui/shell/Layout.h"
#include "ui/shell/Timing.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
} // namespace

Toast::Toast(ToastRequest request) : PopupWidget(PopupSpec::Kind::Toast), req_(std::move(request)) {}

void Toast::tick(double nowMs, const TextMeasure& m)
{
    if (rect_.w == 0)
    {
        const float tw = m ? m(TextStyleId::Body, req_.text) : 8.0f * static_cast<float>(req_.text.size());
        rect_ = {L::toastX, L::toastY, std::min(L::toastMaxW, tw + (req_.hasGlyph ? 64.0f : 32.0f)), L::toastH};
    }
    if (req_.progress >= 0 && req_.progress < 1)
        return; // a running job keeps its toast
    if (doneAtMs_ < 0)
        doneAtMs_ = req_.progress >= 1 ? nowMs : openedMs;
    if (nowMs - doneAtMs_ >= timing::toastMs + tk::motionFastMs)
        closed = true;
}

float Toast::opacity(double nowMs) const noexcept
{
    if (doneAtMs_ < 0)
        return 1.0f;
    const double fadeStart = doneAtMs_ + timing::toastMs;
    return nowMs < fadeStart ? 1.0f : 1.0f - theme::progress(nowMs, fadeStart, tk::motionFastMs);
}

bool Toast::dirty(double nowMs) const
{
    return doneAtMs_ >= 0 && nowMs >= doneAtMs_ + timing::toastMs;
}

bool Toast::handle(const io::ControlEvent& e, UiContext&)
{
    if (! touch::isTouch(e) || e.value != touch::kDown || ! contains(rect_, touch::x(e), touch::y(e)))
        return false; // never takes focus: everything else passes through
    closed = true;
    if (req_.onClick)
        req_.onClick();
    return true;
}

void Toast::build(DrawList& dl, const UiContext& ctx) const
{
    const auto p = theme::faded(ctx.palette, opacity(ctx.nowMs));
    const auto& r = rect_;
    dl.fill(r, p.surface3, r.h / 2);
    float tx = r.x + tk::space[4];
    if (req_.hasGlyph)
    {
        const auto ink =
            req_.glyph == glyph::Kind::Check ? p.play : (req_.glyph == glyph::Kind::Caution ? p.warn : p.text2);
        glyph::draw(dl, req_.glyph, r.x + 12, r.y + 10, tk::sizeIconS, ink);
        tx = r.x + 36;
    }
    dl.text(tx, r.y + r.h / 2, Align::Left, TextStyleId::Body, p.text1, req_.text);
    if (req_.progress >= 0)
    {
        dl.fill({r.x + 16, r.y + 31, r.w - 32, 2}, p.border, 1);
        dl.fill({r.x + 16, r.y + 31, (r.w - 32) * std::clamp(req_.progress, 0.0f, 1.0f), 2}, p.accent, 1);
    }
}

float Toast::drawCautionChip(DrawList& dl, const UiContext& ctx, float right, std::string_view text)
{
    const auto& p = ctx.palette;
    const float w =
        (ctx.measure ? ctx.measure(TextStyleId::Caption, text) : 7.5f * static_cast<float>(text.size())) + 48;
    const RectF r{right - w, L::chipY, w, L::chipH};
    dl.fill(r, p.surface2, r.h / 2);
    dl.stroke(r, p.warn, tk::borderHairline, r.h / 2);
    glyph::draw(dl, glyph::Kind::Caution, r.x + 10, r.y + 6, tk::sizeIconS, p.warn);
    dl.text(r.x + 32, r.y + r.h / 2, Align::Left, TextStyleId::Caption, p.text1, text);
    return r.x;
}
} // namespace ui
