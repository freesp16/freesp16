#include "ui/widgets/Modal.h"

#include "ui/shell/Layout.h"
#include "ui/theme/Draw.h"

namespace ui
{
namespace
{
namespace L = layout;
using Kind = io::ControlEvent::Kind;
} // namespace

Modal::Modal(std::string_view title, std::unique_ptr<Widget> body, std::span<const PopupBinding> bindings)
    : PopupWidget(PopupSpec::Kind::Modal), title_(title), body_(std::move(body)), bindings_(bindings)
{
}

RectF Modal::bounds() const noexcept
{
    return place(L::modalPanel);
}

RectF Modal::content() const noexcept
{
    const auto b = bounds();
    return {b.x, b.y + L::modalTitleH, b.w, b.h - L::modalTitleH};
}

void Modal::build(DrawList& dl, const UiContext& ctx) const
{
    const auto b = bounds();
    draw::panel(dl, ctx.palette, b);
    draw::titleBand(dl, ctx.palette, b, L::modalTitleH, title_, false, true, true);
    if (body_)
        body_->build(dl, ctx);
}

bool Modal::handle(const io::ControlEvent& e, UiContext& ctx)
{
    if (body_ && body_->handle(e, ctx))
        return true;
    if (e.control == io::Control::Back && e.kind == Kind::Press)
    {
        cancel();
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        const auto b = bounds();
        if (touch::y(e) >= b.y && touch::y(e) < b.y + L::modalTitleH && touch::x(e) >= b.x + b.w - 28 - L::rowH / 2 &&
            touch::x(e) < b.x + b.w)
            cancel(); // ✕
        return true;  // click outside a modal: ignored
    }
    return e.control == io::Control::Selector || e.control == io::Control::SelectorPush;
}
} // namespace ui
