#include "ui/widgets/GridPopup.h"

#include "ui/shell/Layout.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
} // namespace

GridPopup::GridPopup(GridRequest request) : PopupWidget(PopupSpec::Kind::Grid), req_(std::move(request))
{
    for (int i = 0; i < static_cast<int>(req_.items.size()); ++i)
        if (req_.items[static_cast<std::size_t>(i)].available)
        {
            if (i == req_.current)
                focus_ = static_cast<int>(visible_.size());
            visible_.push_back(i);
        }
}

RectF GridPopup::bounds() const noexcept
{
    return place(L::gridPopup);
}

RectF GridPopup::cell(int slot) const noexcept
{
    const auto b = bounds();
    const int c = slot % kColumns, r = slot / kColumns;
    return {b.x + 24 + (L::gridCellW + L::gridCellGap) * static_cast<float>(c), b.y + 80 + 176 * static_cast<float>(r),
            L::gridCellW, L::gridCellH};
}

void GridPopup::choose()
{
    if (visible_.empty() || closed)
        return;
    closed = true;
    if (req_.onChoose)
        req_.onChoose(focusedItem());
}

bool GridPopup::handle(const io::ControlEvent& e, UiContext&)
{
    const int n = static_cast<int>(visible_.size());
    if (e.control == io::Control::Selector && e.kind == Kind::Turn)
    {
        if (n > 0)
        {
            if (state != nullptr && state->shift) // SHIFT + turn jumps a row per detent (UID-170), no wrap
                focus_ = std::clamp(focus_ + e.value * kColumns, 0, n - 1);
            else // a turn wraps around the ends (our choice)
                focus_ = ((focus_ + e.value) % n + n) % n;
        }
        return true;
    }
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        choose();
        return true;
    }
    if (e.control == io::Control::Back && e.kind == Kind::Press)
    {
        cancel();
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        const float x = touch::x(e), y = touch::y(e);
        const auto b = bounds();
        if (! contains(b, x, y))
        {
            cancel();
            return true;
        }
        if (y < b.y + L::gridTitleH && x >= b.x + b.w - 28 - L::rowH / 2)
        {
            cancel();
            return true;
        }
        for (int s = 0; s < kPerPage; ++s)
        {
            const int idx = page() * kPerPage + s;
            if (idx < n && contains(cell(s), x, y))
            {
                focus_ = idx; // one tap selects (UI-262)
                choose();
                return true;
            }
        }
        return true;
    }
    return false;
}

bool GridPopup::textKey(TextKey k)
{
    const int n = static_cast<int>(visible_.size());
    switch (k)
    {
    case TextKey::Left:
        if (n > 0)
            focus_ = (focus_ - 1 + n) % n;
        return true;
    case TextKey::Right:
        if (n > 0)
            focus_ = (focus_ + 1) % n;
        return true;
    case TextKey::Up:
        focus_ = std::max(0, focus_ - kColumns);
        return true;
    case TextKey::Down:
        focus_ = std::min(n - 1, focus_ + kColumns);
        return true;
    case TextKey::Return:
        choose();
        return true;
    case TextKey::Escape:
        cancel();
        return true;
    default:
        return false;
    }
}

void GridPopup::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto b = bounds();
    draw::panel(dl, p, b);
    draw::titleBand(dl, p, b, L::gridTitleH, req_.title, false, true, false);
    const int n = static_cast<int>(visible_.size());
    for (int s = 0; s < kPerPage; ++s)
    {
        const int idx = page() * kPerPage + s;
        if (idx >= n)
            break;
        const int item = visible_[static_cast<std::size_t>(idx)];
        const auto& it = req_.items[static_cast<std::size_t>(item)];
        const auto c = cell(s);
        const bool cur = item == req_.current;
        dl.fill(c, p.surface2, tk::radiusM);
        if (cur)
            dl.stroke(c, p.accent, tk::borderFocus, tk::radiusM);
        const auto ink = cur ? p.accent : p.text1;
        const RectF icon48{c.x + 80, c.y + 24, 48, 48};
        if (req_.drawCell)
        {
            GridCell gc;
            gc.item = item;
            gc.icon = it.icon;
            gc.cell = c;
            gc.icon48 = icon48;
            gc.popup = b;
            gc.ink = ink;
            gc.palette = &p;
            gc.slotOnPage = s;
            gc.page = page();
            gc.pages = pages();
            gc.focused = idx == focus_;
            gc.current = cur;
            req_.drawCell(dl, gc);
        }
        else if (req_.drawIcon)
            req_.drawIcon(dl, it.icon, icon48, ink);
        dl.text(c.x + c.w / 2, c.y + 112, Align::Centre, TextStyleId::Label, ink, it.label);
        if (cur)
            glyph::draw(dl, glyph::Kind::Check, c.x + 188 - 10, c.y + 20 - 10, 20, p.accent);
        if (idx == focus_)
        {
            draw::focusRing(dl, p, c, tk::radiusM);
            for (int k = 0; k < 8; ++k) // corner triangle 8 × 8 (UID-152)
                dl.fill({c.x + 200 - static_cast<float>(k), c.y + 136 + static_cast<float>(k),
                         static_cast<float>(k) + 1, 1},
                        p.accent);
        }
    }
    dl.text(b.x + 24, b.y + 636, Align::Left, TextStyleId::Caption, p.text3,
            "Turn to move \xC2\xB7 push or tap again to choose \xC2\xB7 BACK cancels");
    if (pages() > 1)
    {
        for (int k = 0; k < pages(); ++k)
            dl.fill({b.x + 1028 + 16 * static_cast<float>(k), b.y + 632, 8, 8}, k == page() ? p.accent : p.borderStrong,
                    4);
    }
}
} // namespace ui
