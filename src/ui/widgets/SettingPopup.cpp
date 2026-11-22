#include "ui/widgets/SettingPopup.h"

#include "ui/shell/Layout.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/ListRow.h"

#include <algorithm>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
} // namespace

SettingPopup::SettingPopup(SettingRequest request) : PopupWidget(PopupSpec::Kind::Setting), req_(std::move(request))
{
    while (focus_ < static_cast<int>(req_.rows.size()) - 1 && req_.rows[static_cast<std::size_t>(focus_)].disabled)
        ++focus_;
}

RectF SettingPopup::bounds() const noexcept
{
    return place(L::centred(L::popupW, L::popupTitleH + L::rowH * static_cast<float>(req_.rows.size())));
}

std::string_view SettingPopup::valueText(const SettingRow& r, char* buf, int n) const
{
    if (r.spec != nullptr)
    {
        const int len = std::clamp(r.spec->formatValue(r.value, buf, n), 0, n - 1);
        return {buf, static_cast<std::size_t>(len)};
    }
    if (r.current >= 0 && r.current < static_cast<int>(r.options.size()))
        return r.options[static_cast<std::size_t>(r.current)];
    return {};
}

void SettingPopup::push()
{
    if (req_.rows.empty())
        return;
    auto& r = req_.rows[static_cast<std::size_t>(focus_)];
    if (r.disabled)
        return;
    if (r.action) // an action row runs at once
    {
        if (req_.onCommit)
            req_.onCommit(focus_, r.current, r.value);
        return;
    }
    if (! editing_ && r.spec == nullptr && r.options.size() >= kNestedListMin && shell != nullptr)
    {
        // A long enum: the nested list (popups-and-feedback.md "Interactions", push on the SettingPopup).
        ListRequest list;
        list.title = r.label;
        for (std::size_t i = 0; i < r.options.size(); ++i)
            list.items.push_back({r.options[i], {}, {}, static_cast<int>(i)});
        list.current = r.current;
        const int row = focus_;
        list.onChoose = [this, row](int, const ListItem& it)
        {
            auto& rr = req_.rows[static_cast<std::size_t>(row)];
            rr.current = it.value;
            if (req_.onCommit)
                req_.onCommit(row, rr.current, rr.value);
        };
        shell->showList(std::move(list));
        return;
    }
    if (! editing_)
    {
        editing_ = true;
        savedOption_ = r.current;
        savedValue_ = r.value;
        return;
    }
    editing_ = false; // confirm (SET-014 pattern)
    if (req_.onCommit)
        req_.onCommit(focus_, r.current, r.value);
}

void SettingPopup::back()
{
    if (editing_)
    {
        auto& r = req_.rows[static_cast<std::size_t>(focus_)];
        r.current = savedOption_;
        r.value = savedValue_;
        editing_ = false;
        return;
    }
    closed = true;
}

void SettingPopup::turn(int d, bool fine)
{
    const int n = static_cast<int>(req_.rows.size());
    if (n == 0)
        return;
    if (! editing_)
    {
        int t = focus_;
        for (int k = std::abs(d); k > 0; --k)
        {
            int u = t + (d > 0 ? 1 : -1);
            while (u >= 0 && u < n && req_.rows[static_cast<std::size_t>(u)].disabled)
                u += d > 0 ? 1 : -1;
            if (u < 0 || u >= n)
                break;
            t = u;
        }
        focus_ = t;
        return;
    }
    auto& r = req_.rows[static_cast<std::size_t>(focus_)];
    if (r.spec != nullptr)
        r.value = r.spec->step(r.value, d, fine);
    else if (! r.options.empty())
        r.current = std::clamp(r.current + d, 0, static_cast<int>(r.options.size()) - 1);
}

bool SettingPopup::handle(const io::ControlEvent& e, UiContext&)
{
    if (e.control == io::Control::Selector && e.kind == Kind::Turn)
    {
        turn(e.value, state != nullptr && state->shift);
        return true;
    }
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        push();
        return true;
    }
    if (e.control == io::Control::Back && e.kind == Kind::Press)
    {
        back();
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        const float x = touch::x(e), y = touch::y(e);
        const auto b = bounds();
        if (! contains(b, x, y))
        {
            closed = true; // click outside: close, no change
            return true;
        }
        if (y < b.y + L::popupTitleH)
        {
            if (x >= b.x + b.w - 28 - L::rowH / 2)
                closed = true;
            return true;
        }
        const int r = static_cast<int>((y - b.y - L::popupTitleH) / L::rowH);
        if (r < 0 || r >= static_cast<int>(req_.rows.size()) || req_.rows[static_cast<std::size_t>(r)].disabled)
            return true;
        if (r != focus_)
        {
            if (editing_)
                back();
            focus_ = r;
        }
        else if (x >= b.x + b.w / 2)
            push(); // focused value cell → enter / confirm
        return true;
    }
    return false;
}

bool SettingPopup::textKey(TextKey k)
{
    switch (k)
    {
    case TextKey::Up:
        if (! editing_)
            turn(-1, false);
        return true;
    case TextKey::Down:
        if (! editing_)
            turn(1, false);
        return true;
    case TextKey::Left:
        if (editing_)
            turn(-1, false);
        else
            push();
        return true;
    case TextKey::Right:
        if (editing_)
            turn(1, false);
        else
            push();
        return true;
    case TextKey::Return:
        push();
        return true;
    case TextKey::Escape:
        back();
        return true;
    default:
        return false;
    }
}

void SettingPopup::build(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto b = bounds();
    draw::panel(dl, p, b);
    draw::titleBand(dl, p, b, L::popupTitleH, req_.title, false, true, false);
    const float half = b.w / 2;
    char buf[32];
    for (int i = 0; i < static_cast<int>(req_.rows.size()); ++i)
    {
        const auto& r = req_.rows[static_cast<std::size_t>(i)];
        const RectF rr{b.x, b.y + L::popupTitleH + L::rowH * static_cast<float>(i), b.w, L::rowH};
        const bool f = i == focus_;
        ListRowModel m;
        m.label = r.label;
        m.zebra = i;
        m.focused = f;
        m.disabled = r.disabled;
        m.valueX = half + tk::space[5];
        if (! f)
            m.value = valueText(r, buf, sizeof buf);
        ListRow::draw(dl, ctx, rr, m);
        draw::vline(dl, b.x + half, rr.y + 8, rr.y + L::rowH - 8, p.border);
        if (f) // focused value cell: a second accent-weak layer, the value in text-1 and a › (kit_track setting_popup)
        {
            dl.fill({b.x + half, rr.y, half, L::rowH}, p.accentWeak);
            dl.text(b.x + half + tk::space[5], rr.y + L::rowH / 2, Align::Left,
                    editing_ ? TextStyleId::BodyStrong : TextStyleId::Body, p.text1, valueText(r, buf, sizeof buf));
            if (editing_)
            {
                dl.stroke({b.x + half + 2, rr.y + 2, half - 4, L::rowH - 4}, p.accent, tk::borderFocus, tk::radiusXs);
                dl.text(b.x + b.w - 56, rr.y + L::rowH / 2, Align::Centre, TextStyleId::Title, p.accent,
                        "\xE2\x80\xB9");
                dl.text(b.x + b.w - 28, rr.y + L::rowH / 2, Align::Centre, TextStyleId::Title, p.accent,
                        "\xE2\x80\xBA");
            }
            else
                dl.text(b.x + b.w - 28, rr.y + L::rowH / 2, Align::Centre, TextStyleId::Title, p.text2, "\xE2\x80\xBA");
        }
    }
}
} // namespace ui
