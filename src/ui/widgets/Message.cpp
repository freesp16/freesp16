#include "ui/widgets/Message.h"

#include "ui/shell/Layout.h"
#include "ui/theme/Draw.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
constexpr int kMaxLines = 6;
} // namespace

Message::Message(MessageRequest request) : PopupWidget(PopupSpec::Kind::Message), req_(std::move(request))
{
    if (req_.buttons.empty())
        req_.buttons.push_back({"OK", ButtonStyle::Primary});
    if (req_.lines.size() > kMaxLines)
        req_.lines.resize(kMaxLines);
    const int n = static_cast<int>(req_.buttons.size());
    focus_ = req_.defaultButton >= 0 && req_.defaultButton < n ? req_.defaultButton : n - 1;
    req_.cancelButton = n == 1 ? 0 : std::clamp(req_.cancelButton, 0, n - 1);
}

RectF Message::bounds() const noexcept
{
    const float h = 72 + 20 * static_cast<float>(req_.lines.size()) + 24 + L::buttonH + 24;
    return place(L::centred(L::popupW, h));
}

RectF Message::buttonRect(int i, const TextMeasure& m) const noexcept
{
    const auto b = bounds();
    float right = b.x + b.w - tk::space[5];
    for (int k = static_cast<int>(req_.buttons.size()) - 1; k >= 0; --k)
    {
        const auto& label = req_.buttons[static_cast<std::size_t>(k)].label;
        const float lw = m ? m(TextStyleId::Label, label) : 10.0f * static_cast<float>(label.size());
        const float w = std::max(L::messageMinButtonW, lw + 48);
        if (k == i)
            return {right - w, b.y + b.h - 72, w, L::buttonH};
        right -= w + tk::space[4];
    }
    return {};
}

void Message::press(int button)
{
    if (closed)
        return;
    closed = true;
    if (req_.onResult)
        req_.onResult(button);
}

bool Message::handle(const io::ControlEvent& e, UiContext& ctx)
{
    const int n = static_cast<int>(req_.buttons.size());
    if (e.control == io::Control::Selector && e.kind == Kind::Turn)
    {
        focus_ = std::clamp(focus_ + (e.value > 0 ? 1 : -1), 0, n - 1);
        return true;
    }
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        press(focus_);
        return true;
    }
    if (e.control == io::Control::Back && e.kind == Kind::Press)
    {
        press(req_.cancelButton);
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        for (int i = 0; i < n; ++i)
            if (contains(hitArea(buttonRect(i, ctx.measure)), touch::x(e), touch::y(e)))
            {
                press(i);
                break;
            }
        return true; // click outside: ignored
    }
    return false;
}

bool Message::textKey(TextKey k)
{
    const int n = static_cast<int>(req_.buttons.size());
    switch (k)
    {
    case TextKey::Left:
        focus_ = std::max(0, focus_ - 1);
        return true;
    case TextKey::Right:
    case TextKey::Tab:
        focus_ = (focus_ + 1) % n;
        return true;
    case TextKey::Return:
        press(focus_);
        return true;
    case TextKey::Escape:
        press(req_.cancelButton);
        return true;
    default:
        return false;
    }
}

void Message::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto b = bounds();
    draw::panel(dl, p, b);
    const auto ink = req_.icon == glyph::Kind::Caution ? p.warn : (req_.icon == glyph::Kind::Error ? p.rec : p.accent);
    glyph::draw(dl, req_.icon, b.x + tk::space[5], b.y + 20, 32, ink);
    dl.text(b.x + 72, b.y + 36, Align::Left, TextStyleId::Title, p.text1, req_.title);
    for (std::size_t i = 0; i < req_.lines.size(); ++i)
        dl.text(b.x + 72, b.y + 80 + 20 * static_cast<float>(i), Align::Left, TextStyleId::Body, p.text2,
                req_.lines[i]);
    for (int i = 0; i < static_cast<int>(req_.buttons.size()); ++i)
    {
        const auto& btn = req_.buttons[static_cast<std::size_t>(i)];
        Button::draw(dl, ctx, buttonRect(i, ctx.measure), {btn.label, btn.style, false, false, i == focus_});
    }
}
} // namespace ui
