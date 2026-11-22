#include "ui/widgets/Progress.h"

#include "ui/shell/Layout.h"
#include "ui/shell/Timing.h"
#include "ui/theme/Draw.h"

#include <algorithm>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
} // namespace

Progress::Progress(ProgressRequest request) : PopupWidget(PopupSpec::Kind::Message), req_(std::move(request)) {}

RectF Progress::bounds() const noexcept
{
    return place(L::centred(L::popupW, req_.cancellable ? 212.0f : 140.0f));
}

bool Progress::visibleAt(double nowMs) const
{
    return nowMs - openedMs >= timing::progressDelayMs;
}

void Progress::set(float f, std::string_view detail)
{
    fraction_ = std::clamp(f, 0.0f, 1.0f);
    if (! detail.empty())
        req_.detail.assign(detail);
}

void Progress::cancel()
{
    if (! req_.cancellable || closed)
        return; // BACK is ignored while a non-cancellable job runs
    closed = true;
    if (req_.onCancel)
        req_.onCancel();
}

bool Progress::handle(const io::ControlEvent& e, UiContext&)
{
    if ((e.control == io::Control::SelectorPush || e.control == io::Control::Back) && e.kind == Kind::Press)
    {
        cancel();
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        const auto b = bounds();
        if (req_.cancellable &&
            contains(hitArea({b.x + b.w - 24 - 144, b.y + b.h - 72, 144, 48}), touch::x(e), touch::y(e)))
            cancel();
        return true;
    }
    return e.control == io::Control::Selector;
}

bool Progress::textKey(TextKey k)
{
    if (k == TextKey::Escape)
        cancel();
    return true;
}

void Progress::build(DrawList& dl, const UiContext& ctx) const
{
    if (! visibleAt(ctx.nowMs))
        return;
    const auto& p = ctx.palette;
    const auto b = bounds();
    draw::panel(dl, p, b);
    dl.text(b.x + tk::space[5], b.y + 36, Align::Left, TextStyleId::Title, p.text1, req_.title);
    char pct[8];
    const int n = std::snprintf(pct, sizeof pct, "%d %%", static_cast<int>(fraction_ * 100));
    dl.text(b.x + b.w - tk::space[5], b.y + 36, Align::Right, TextStyleId::ValueS, p.text2,
            std::string_view(pct, static_cast<std::size_t>(std::max(0, n))));
    dl.text(b.x + tk::space[5], b.y + 80, Align::Left, TextStyleId::Body, p.text2, req_.detail);
    const RectF bar{b.x + tk::space[5], b.y + 104, b.w - 2 * tk::space[5], 8};
    dl.fill(bar, p.border, 4);
    if (fraction_ > 0)
        dl.fill({bar.x, bar.y, std::max(8.0f, bar.w * fraction_), bar.h}, p.accent, 4);
    if (req_.cancellable)
        Button::draw(dl, ctx, {b.x + b.w - 24 - 144, b.y + b.h - 72, 144, 48},
                     {"CANCEL", ButtonStyle::Plain, false, false, true});
}

void Progress::drawInline(DrawList& dl, const UiContext& ctx, RectF host, float fraction)
{
    dl.fill({host.x, host.y, host.w, 4}, ctx.palette.border, 2);
    dl.fill({host.x, host.y, host.w * std::clamp(fraction, 0.0f, 1.0f), 4}, ctx.palette.accent, 2);
}
} // namespace ui
