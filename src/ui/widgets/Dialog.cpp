#include "ui/widgets/Dialog.h"

#include "ui/shell/Layout.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
} // namespace

Dialog::Dialog(DialogRequest request)
    : PopupWidget(PopupSpec::Kind::Dialog), req_(std::move(request)), field_(L::textField)
{
    field_.setText(req_.text, true);
    if (req_.buttons.empty())
        req_.buttons = {{"CANCEL", ButtonStyle::Plain}, {"OK", ButtonStyle::Primary}};
    if (req_.primary >= 0 && req_.primary < static_cast<int>(req_.buttons.size()))
        keyboard_.doneLabel = req_.buttons[static_cast<std::size_t>(req_.primary)].label;
    revalidate();
}

RectF Dialog::bounds() const noexcept
{
    return place(L::dialog);
}

void Dialog::revalidate()
{
    error_.clear();
    if (field_.characters() == 0)
        error_ = " "; // empty: primary disabled, no message (project.md)
    else if (req_.validate)
        error_ = req_.validate(field_.text());
    keyboard_.doneEnabled = error_.empty();
    field_.invalid = ! error_.empty() && error_ != " ";
}

void Dialog::finish(int button)
{
    if (closed)
        return;
    if (button == req_.primary && ! valid())
        return; // disabled while the name is empty or invalid
    closed = true;
    if (req_.onResult)
        req_.onResult(button, field_.text());
}

bool Dialog::text(std::string_view utf8)
{
    field_.insert(utf8);
    revalidate();
    return true;
}

bool Dialog::textKey(TextKey k)
{
    switch (k)
    {
    case TextKey::Backspace:
        field_.backspace();
        break;
    case TextKey::Left:
        field_.moveCaret(-1);
        break;
    case TextKey::Right:
        field_.moveCaret(1);
        break;
    case TextKey::WordLeft:
        field_.moveWord(-1);
        break;
    case TextKey::WordRight:
        field_.moveWord(1);
        break;
    case TextKey::Home:
        field_.moveCaret(-1000);
        break;
    case TextKey::End:
        field_.moveCaret(1000);
        break;
    case TextKey::Return:
        finish(req_.primary);
        return true;
    case TextKey::Escape:
        finish(req_.cancel);
        return true;
    default:
        return false;
    }
    revalidate();
    return true;
}

bool Dialog::handle(const io::ControlEvent& e, UiContext& ctx)
{
    field_.caretSinceMs = ctx.nowMs;
    if (e.control == io::Control::Selector && e.kind == Kind::Turn)
    {
        if (state != nullptr && state->shift)
            field_.moveWord(e.value);
        else
            field_.moveCaret(e.value);
        return true;
    }
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        finish(req_.primary);
        return true;
    }
    if (e.control == io::Control::Back && e.kind == Kind::Press)
    {
        finish(req_.cancel);
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        const float x = touch::x(e), y = touch::y(e);
        const int k = keyboard_.keyAt(x, y);
        if (k >= 0)
        {
            keyboard_.pressedKey = k;
            keyboard_.pressedAtMs = ctx.nowMs;
            const auto key = keyboard_.key(k);
            using KK = SoftwareKeyboard::KeyKind;
            switch (key.kind)
            {
            case KK::Backspace:
                field_.backspace();
                break;
            case KK::Left:
                field_.moveCaret(-1);
                break;
            case KK::Right:
                field_.moveCaret(1);
                break;
            case KK::Done:
                finish(req_.primary);
                return true;
            default:
            {
                const auto t = keyboard_.press(k);
                if (! t.empty())
                {
                    field_.insert(t);
                    if (keyboard_.shift && key.kind == KK::Char) // ⇧ applies to the next letter
                        keyboard_.shift = false;
                }
                break;
            }
            }
            revalidate();
            return true;
        }
        for (int i = 0; i < static_cast<int>(req_.buttons.size()) && i < 3; ++i)
            if (contains(L::dialogButton(i), x, y))
            {
                finish(i);
                return true;
            }
        const auto b = bounds();
        if (y >= b.y && y < b.y + L::dialogTitleH && x >= b.x + b.w - 28 - L::rowH / 2 && x < b.x + b.w)
            finish(req_.cancel); // ✕
        else if (contains(L::textField, x, y))
            field_.moveCaret(1000); // field → caret (at the end)
        return true;                // click outside: ignored
    }
    return false;
}

void Dialog::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto b = bounds();
    draw::panel(dl, p, b);
    draw::titleBand(dl, p, b, L::dialogTitleH, req_.title, true, true, true);
    dl.text(b.x + b.w / 2, b.y + 84, Align::Centre, TextStyleId::Body, p.text2, req_.prompt);
    field_.build(dl, ctx);
    if (field_.invalid)
    {
        glyph::draw(dl, glyph::Kind::Caution, L::dialogHint.x, L::dialogHint.y + 2, tk::sizeIconS, p.rec);
        dl.text(L::dialogHint.x + 24, L::dialogHint.y + L::dialogHint.h / 2, Align::Left, TextStyleId::Body, p.rec,
                error_);
    }
    else
        dl.text(L::dialogHint.x, L::dialogHint.y + L::dialogHint.h / 2, Align::Left, TextStyleId::Body, p.text3,
                req_.hint);
    for (int i = 0; i < static_cast<int>(req_.buttons.size()) && i < 3; ++i)
    {
        const auto& btn = req_.buttons[static_cast<std::size_t>(i)];
        const bool disabled = i == req_.primary && ! valid();
        Button::draw(dl, ctx, L::dialogButton(i),
                     {btn.label, i == req_.primary ? ButtonStyle::Primary : btn.style, disabled});
    }
    keyboard_.build(dl, ctx);
}
} // namespace ui
