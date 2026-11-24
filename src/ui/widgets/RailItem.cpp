#include "ui/widgets/RailItem.h"

#include "ui/shell/Layout.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/OptionSelector.h"
#include "ui/widgets/Toggle.h"

namespace ui
{
RectF RailItem::bounds() const noexcept
{
    return layout::railItem(index_, compact_, width_);
}

void RailItem::build(DrawList& dl, const UiContext& ctx) const
{
    if (spec_ == nullptr || spec_->label.empty()) // an empty label is a hole (ui/shell/RailOwner.h)
        return;
    const auto r = bounds();
    switch (spec_->kind)
    {
    case RailItemSpec::Kind::Button:
        Button::draw(dl, ctx, r, {spec_->label, ButtonStyle::Plain, state_.disabled, state_.pressed, state_.focused});
        break;
    case RailItemSpec::Kind::Toggle:
        Toggle::draw(dl, ctx, r,
                     {spec_->label, state_.on, state_.disabled, state_.pressed, state_.focused, state_.onColour});
        break;
    case RailItemSpec::Kind::Option:
        OptionSelector::draw(dl, ctx, r, {spec_->label, state_.value, state_.disabled, state_.pressed, state_.focused});
        break;
    }
}
} // namespace ui
