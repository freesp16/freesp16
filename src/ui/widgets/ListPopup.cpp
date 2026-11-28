#include "ui/widgets/ListPopup.h"

#include "ui/shell/Layout.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/ListRow.h"
#include "ui/widgets/Scrollbar.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ui
{
namespace
{
namespace L = layout;
namespace tk = ::theme::tokens;
using Kind = io::ControlEvent::Kind;

// TrackChip, inline variant (components/visuals.md): 32 × 24, radius 4, track-colour fill, body-strong number in
// on-accent; an empty track uses the colour's dim variant (number in text-1), a muted one gets a 10 lpx corner mark.
void drawChip(DrawList& dl, const UiContext& ctx, float x, float y, const ListChip& c)
{
    const auto& p = ctx.palette;
    const int colour = c.colour > 0 ? c.colour : c.track;
    dl.fill({x, y, 32, 24}, theme::trackColour(p, colour, c.dim), tk::radiusXs);
    static constexpr const char* numbers[] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                              "9", "10", "11", "12", "13", "14", "15", "16"};
    dl.text(x + 16, y + 13, Align::Centre, TextStyleId::BodyStrong, c.dim ? p.text1 : p.onAccent,
            numbers[(c.track - 1) & 15]);
    if (c.muted)
        dl.fill({x + 22, y, 10, 10}, p.surface3, tk::radiusXs);
}
} // namespace

ListPopup::ListPopup(ListRequest request) : PopupWidget(PopupSpec::Kind::List), req_(std::move(request))
{
    std::string_view group{};
    for (int i = 0; i < static_cast<int>(req_.items.size()); ++i)
    {
        const auto& it = req_.items[static_cast<std::size_t>(i)];
        if (req_.grouped && it.group != group && ! it.group.empty())
            rows_.push_back({-1, it.group});
        group = it.group;
        rows_.push_back({i, it.label});
    }
    for (int r = 0; r < rowCount(); ++r)
        if (rows_[static_cast<std::size_t>(r)].item == req_.current && req_.current >= 0)
            focus_ = r;
    if (focus_ < 0 || ! focusable(focus_))
        for (int r = 0; r < rowCount() && focus_ < 0; ++r)
            if (focusable(r))
                focus_ = r;
    scrollToFocus();
}

bool ListPopup::focusable(int r) const noexcept
{
    if (r < 0 || r >= rowCount())
        return false;
    const auto item = rows_[static_cast<std::size_t>(r)].item;
    return item >= 0 && req_.items[static_cast<std::size_t>(item)].enabled;
}

int ListPopup::focusedItem() const noexcept
{
    return focus_ >= 0 ? rows_[static_cast<std::size_t>(focus_)].item : -1;
}

RectF ListPopup::bounds() const noexcept
{
    const int n = std::min(rowCount(), L::listVisibleRows);
    return place(L::centred(L::popupW, L::popupTitleH + L::rowH * static_cast<float>(n)));
}

void ListPopup::scrollToFocus()
{
    const int visible = L::listVisibleRows;
    if (rowCount() <= visible || focus_ < 0)
    {
        first_ = 0;
        return;
    }
    // One row of context above and below the focus (UID-170).
    if (focus_ - 1 < first_)
        first_ = std::max(0, focus_ - 1);
    if (focus_ + 1 >= first_ + visible)
        first_ = std::min(rowCount() - visible, focus_ + 2 - visible);
}

void ListPopup::moveFocus(int detents, bool byGroup)
{
    const int dir = detents > 0 ? 1 : -1;
    for (int d = std::abs(detents); d > 0; --d)
    {
        int r = focus_;
        if (byGroup) // next / previous group header, then the first focusable row below it
        {
            auto header = [&](int row)
            { return row >= 0 && row < rowCount() && rows_[static_cast<std::size_t>(row)].item < 0; };
            int h = -1;
            if (dir > 0)
            {
                for (int k = r + 1; k < rowCount() && h < 0; ++k)
                    h = header(k) ? k : -1;
            }
            else
            {
                int own = -1;
                for (int k = r - 1; k >= 0 && own < 0; --k)
                    own = header(k) ? k : -1;
                for (int k = own - 1; k >= 0 && h < 0; --k)
                    h = header(k) ? k : -1;
            }
            int t = h + 1; // h = -1 going back past the first group: the first focusable row
            while (t < rowCount() && ! focusable(t))
                ++t;
            if ((h >= 0 || dir < 0) && t < rowCount())
                focus_ = t;
            continue;
        }
        int t = r + dir;
        while (t >= 0 && t < rowCount() && ! focusable(t))
            t += dir;
        if (t >= 0 && t < rowCount())
            focus_ = t; // no wrap
    }
    scrollToFocus();
}

void ListPopup::choose()
{
    const int item = focusedItem();
    if (item < 0 || chosen_)
        return;
    chosen_ = true;
    closed = true;
    if (req_.onChoose)
        req_.onChoose(item, req_.items[static_cast<std::size_t>(item)]);
}

void ListPopup::cancel()
{
    if (! closed && ! chosen_ && req_.onCancel)
        req_.onCancel();
    closed = true;
}

int ListPopup::rowAt(float x, float y) const noexcept
{
    const auto b = bounds();
    if (x < b.x || x >= b.x + b.w || y < b.y + L::popupTitleH || y >= b.y + b.h)
        return -1;
    const int r = first_ + static_cast<int>((y - b.y - L::popupTitleH) / L::rowH);
    return r < rowCount() ? r : -1;
}

bool ListPopup::handle(const io::ControlEvent& e, UiContext&)
{
    const bool shift = state != nullptr && state->shift;
    if (e.control == io::Control::Selector && e.kind == Kind::Turn)
    {
        moveFocus(e.value, shift);
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
        if (! contains(b, x, y)) // click outside: close, no change
        {
            cancel();
            return true;
        }
        if (y < b.y + L::popupTitleH)
        {
            if (x >= b.x + b.w - 28 - L::rowH / 2) // ✕ (44 × 44 hit)
                cancel();
            return true;
        }
        const int r = rowAt(x, y);
        if (focusable(r))
        {
            if (r == focus_)
                choose();
            else
                focus_ = r;
        }
        return true;
    }
    if (e.control == io::Control::Shift)
        return false; // SHIFT state is tracked by the shell
    return false;
}

bool ListPopup::textKey(TextKey k)
{
    switch (k)
    {
    case TextKey::Up:
        moveFocus(-1, false);
        return true;
    case TextKey::Down:
        moveFocus(1, false);
        return true;
    case TextKey::PageUp:
        moveFocus(-L::listVisibleRows, false);
        return true;
    case TextKey::PageDown:
        moveFocus(L::listVisibleRows, false);
        return true;
    case TextKey::Home:
        moveFocus(-rowCount(), false);
        return true;
    case TextKey::End:
        moveFocus(rowCount(), false);
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

bool ListPopup::text(std::string_view s)
{
    if (s.empty())
        return false;
    const auto want = std::toupper(static_cast<unsigned char>(s[0]));
    for (int k = 1; k <= rowCount(); ++k) // the next row starting with the letter, wrapping
    {
        const int r = (std::max(focus_, 0) + k) % rowCount();
        const auto label = rows_[static_cast<std::size_t>(r)].label;
        if (focusable(r) && ! label.empty() && std::toupper(static_cast<unsigned char>(label[0])) == want)
        {
            focus_ = r;
            scrollToFocus();
            return true;
        }
    }
    return true;
}

void ListPopup::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto b = bounds();
    draw::panel(dl, p, b);
    draw::titleBand(dl, p, b, L::popupTitleH, req_.title, false, true, false);
    const int n = std::min(rowCount(), L::listVisibleRows);
    for (int i = 0; i < n; ++i)
    {
        const int r = first_ + i;
        const auto& row = rows_[static_cast<std::size_t>(r)];
        const RectF rr{b.x, b.y + L::popupTitleH + L::rowH * static_cast<float>(i), b.w, L::rowH};
        ListRowModel m;
        m.label = row.label;
        m.zebra = r;
        if (row.item < 0)
            m.header = true;
        else
        {
            const auto& it = req_.items[static_cast<std::size_t>(row.item)];
            m.focused = r == focus_;
            m.current = row.item == req_.current;
            m.disabled = ! it.enabled;
            m.indent = req_.grouped ? 16.0f : 0.0f;
            m.labelX = it.icon.empty() && ! hasChip(row.item) ? 24.0f : 72.0f;
        }
        ListRow::draw(dl, ctx, rr, m);
        if (row.item >= 0 && hasChip(row.item)) // UID-842: the track's chip before the name
            drawChip(dl, ctx, rr.x + 24 + m.indent, rr.y + (L::rowH - 24) / 2,
                     req_.chips[static_cast<std::size_t>(row.item)]);
    }
    if (rowCount() > L::listVisibleRows)
        Scrollbar::draw(
            dl, ctx, {b.x + b.w - 8, b.y + L::popupTitleH + 4, 4, b.h - L::popupTitleH - 8},
            {static_cast<float>(rowCount()), static_cast<float>(L::listVisibleRows), static_cast<float>(first_)});
}
} // namespace ui
