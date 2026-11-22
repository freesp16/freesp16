#include "ui/widgets/TextField.h"

#include "ui/theme/Theme.h"

#include <cstring>

namespace ui
{
namespace tk = ::theme::tokens;
namespace
{
bool continuation(char c) noexcept
{
    return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
}
} // namespace

void TextField::setText(std::string_view s, bool selectAll) noexcept
{
    len_ = 0;
    caret_ = 0;
    selectAll_ = false;
    insert(s);
    selectAll_ = selectAll && len_ > 0;
}

std::size_t TextField::characters() const noexcept
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < len_; ++i)
        n += continuation(buf_[i]) ? 0 : 1;
    return n;
}

void TextField::insert(std::string_view s) noexcept
{
    if (selectAll_)
    {
        len_ = caret_ = 0;
        selectAll_ = false;
    }
    std::size_t n = s.size();
    if (len_ + n > kMaxBytes - 1)
        n = kMaxBytes - 1 - len_;
    while (n > 0 && n < s.size() && continuation(s[n])) // never split a code point
        --n;
    std::memmove(buf_ + caret_ + n, buf_ + caret_, len_ - caret_);
    std::memcpy(buf_ + caret_, s.data(), n);
    len_ += n;
    caret_ += n;
}

void TextField::backspace() noexcept
{
    if (selectAll_)
    {
        len_ = caret_ = 0;
        selectAll_ = false;
        return;
    }
    if (caret_ == 0)
        return;
    std::size_t start = caret_ - 1;
    while (start > 0 && continuation(buf_[start]))
        --start;
    std::memmove(buf_ + start, buf_ + caret_, len_ - caret_);
    len_ -= caret_ - start;
    caret_ = start;
}

void TextField::moveCaret(int chars) noexcept
{
    if (selectAll_)
    {
        selectAll_ = false;
        caret_ = chars < 0 ? 0 : len_;
        return;
    }
    for (; chars > 0 && caret_ < len_; --chars)
        do
            ++caret_;
        while (caret_ < len_ && continuation(buf_[caret_]));
    for (; chars < 0 && caret_ > 0; ++chars)
        do
            --caret_;
        while (caret_ > 0 && continuation(buf_[caret_]));
}

void TextField::moveWord(int words) noexcept
{
    selectAll_ = false;
    auto sep = [&](std::size_t i) { return buf_[i] == ' ' || buf_[i] == '_' || buf_[i] == '-' || buf_[i] == '.'; };
    for (; words > 0; --words)
    {
        while (caret_ < len_ && sep(caret_))
            ++caret_;
        while (caret_ < len_ && ! sep(caret_))
            ++caret_;
    }
    for (; words < 0; ++words)
    {
        while (caret_ > 0 && sep(caret_ - 1))
            --caret_;
        while (caret_ > 0 && ! sep(caret_ - 1))
            --caret_;
    }
}

void TextField::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto& r = rect_;
    dl.fill(r, p.surface1, tk::radiusS);
    const auto edge = invalid ? p.rec : (focused ? p.accent : p.borderStrong);
    dl.stroke(r, edge, focused || invalid ? 2.0f : 1.0f, tk::radiusS);
    const float tx = r.x + tk::space[4], cy = r.y + r.h / 2;
    const auto t = text();
    if (t.empty())
        dl.text(tx, cy, Align::Left, TextStyleId::Body, p.text3, placeholder);
    else
    {
        if (selectAll_ && ctx.measure)
            dl.fill({tx - 2, r.y + 12, ctx.measure(TextStyleId::Body, t) + 4, r.h - 24}, p.accentWeak, 2);
        dl.text(tx, cy, Align::Left, TextStyleId::Body, p.text1, t);
    }
    if (! focused)
        return;
    const bool on = ctx.nowMs - caretSinceMs < tk::motionBlinkSlowMs / 2.0 ||
                    theme::blinkOn(ctx.nowMs - caretSinceMs, tk::motionBlinkSlowMs);
    if (on)
    {
        const float cx =
            tx + (ctx.measure ? ctx.measure(TextStyleId::Body, t.substr(0, caret_)) : 0) + (t.empty() ? 0 : 3);
        dl.fill({cx, r.y + 14, 2, r.h - 28}, p.accent);
    }
}
} // namespace ui
