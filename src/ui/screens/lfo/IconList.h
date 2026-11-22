// A ListPopup with a drawn icon column: the LFO SHAPE popup's 24 lpx shape glyphs (UID-735) and SET SCALE's
// mini degree strips (playback.md "SCALE popup"). The shell's ListPopup reserves the icon column (labelX 72 when
// ListItem::icon is set) but draws no icon; this decorator owns one, forwards input and the shell's placement fields,
// and draws `icon(dl, item, x, y, ink)` per visible row (24 × 24 at x + 24, row-centred; ink = accent on the current
// row, text-2 elsewhere). Ungrouped lists only (row = item). Header-only, message thread.
#pragma once

#include "ui/shell/Layout.h"
#include "ui/widgets/ListPopup.h"

#include <algorithm>
#include <functional>
#include <memory>

namespace ui::m4
{
class IconListPopup final : public PopupWidget
{
public:
    using IconFn = std::function<void(DrawList&, int item, float x, float y, std::uint32_t ink)>;
    IconListPopup(ListRequest req, IconFn icon)
        : PopupWidget(PopupSpec::Kind::List), current_(req.current), count_(static_cast<int>(req.items.size())),
          inner_(std::move(req)), icon_(std::move(icon))
    {
    }
    RectF bounds() const noexcept override
    {
        sync();
        return inner_.bounds();
    }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        sync();
        inner_.build(dl, ctx);
        if (! icon_)
            return;
        const auto b = inner_.bounds();
        const int first = inner_.firstVisibleRow();
        const int n = std::min(count_ - first, layout::listVisibleRows);
        for (int i = 0; i < n; ++i)
        {
            const int item = first + i;
            const float y = b.y + layout::popupTitleH + layout::rowH * static_cast<float>(i) + (layout::rowH - 24) / 2;
            icon_(dl, item, b.x + 24, y, item == current_ ? ctx.palette.accent : ctx.palette.text2);
        }
    }
    bool handle(const io::ControlEvent& e, UiContext& ctx) override
    {
        sync();
        const bool used = inner_.handle(e, ctx);
        closed = closed || inner_.closed;
        return used;
    }
    bool textKey(TextKey k) override
    {
        const bool used = inner_.textKey(k);
        closed = closed || inner_.closed;
        return used;
    }
    bool text(std::string_view s) override
    {
        const bool used = inner_.text(s);
        closed = closed || inner_.closed;
        return used;
    }
    void cancel() override
    {
        inner_.cancel();
        closed = true;
    }
    int focusedItem() const noexcept { return inner_.focusedItem(); }

private:
    void sync() const noexcept
    {
        auto& in = const_cast<ListPopup&>(inner_);
        in.dx = dx;
        in.dy = dy;
        in.state = state;
        in.shell = shell;
        in.openedMs = openedMs;
    }
    int current_, count_;
    ListPopup inner_;
    IconFn icon_;
};
} // namespace ui::m4
