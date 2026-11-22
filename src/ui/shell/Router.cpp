#include "ui/shell/Router.h"

namespace ui
{
Router::Router(const ScreenRegistry& screens, const FeatureFlags* flags, const PopupRegistry* popups)
    : screens_(screens), flags_(flags), popups_(popups)
{
    setCurrent(resolve(kRoot), 0);
}

const ScreenSpec* Router::resolve(std::string_view id) const noexcept
{
    const ScreenSpec* reference = nullptr;
    const ScreenSpec* variant = nullptr;
    for (const auto& s : screens_.all())
    {
        if (s.id != id)
            continue;
        if (s.featureFlag.empty())
        {
            if (reference == nullptr)
                reference = &s;
        }
        else if (flags_ != nullptr && flags_->enabled(s.featureFlag))
            variant = &s;
    }
    return variant != nullptr ? variant : reference;
}

bool Router::breadcrumbOf(std::string_view id, std::array<std::string_view, kMaxDepth>& out, std::size_t& depth) const
{
    std::array<const ScreenSpec*, kMaxDepth> chain{};
    std::size_t n = 0;
    for (const ScreenSpec* s = resolve(id); s != nullptr; s = s->parent.empty() ? nullptr : resolve(s->parent))
    {
        if (n == kMaxDepth)
            return false; // too deep or cyclic
        chain[n++] = s;
        if (! s->parent.empty() && resolve(s->parent) == nullptr)
            return false; // broken chain
    }
    if (n == 0 || chain[n - 1]->id != kRoot)
        return false; // every path starts at HOME (UID-300)
    for (std::size_t i = 0; i < n; ++i)
        out[i] = chain[n - 1 - i]->title;
    depth = n;
    return true;
}

void Router::setCurrent(const ScreenSpec* s, int direction)
{
    if (s == nullptr)
        return;
    current_ = s;
    direction_ = direction;
    ++changes_;
    std::array<const ScreenSpec*, kMaxDepth> chain{};
    std::size_t n = 0;
    for (const ScreenSpec* c = s; c != nullptr && n < kMaxDepth; c = c->parent.empty() ? nullptr : resolve(c->parent))
        chain[n++] = c;
    depth_ = n;
    for (std::size_t i = 0; i < n; ++i)
    {
        titles_[i] = chain[n - 1 - i]->title;
        ids_[i] = chain[n - 1 - i]->id;
    }
}

bool Router::open(std::string_view id)
{
    if (const auto* s = resolve(id))
    {
        if (s == current_)
            return true;
        // Deeper when the target's parent chain passes through the current screen.
        int dir = 0;
        for (const ScreenSpec* c = s; c != nullptr; c = c->parent.empty() ? nullptr : resolve(c->parent))
            if (c == current_)
            {
                dir = 1;
                break;
            }
        if (presenter_ != nullptr)
            presenter_->dismissAll();
        setCurrent(s, dir);
        return true;
    }
    return showPopup(id);
}

void Router::back()
{
    if (presenter_ != nullptr && presenter_->popupOpen())
    {
        presenter_->dismissTop();
        return;
    }
    if (current_ == nullptr || current_->parent.empty())
        return; // HOME is the base of every screen
    setCurrent(resolve(current_->parent), -1);
}

void Router::home()
{
    if (presenter_ != nullptr)
        presenter_->dismissAll();
    if (const auto* root = resolve(kRoot); root != nullptr && root != current_)
        setCurrent(root, 0);
}

bool Router::showPopup(const PopupSpec& p)
{
    if (presenter_ == nullptr || (flags_ != nullptr ? ! flags_->enabled(p.featureFlag) : ! p.featureFlag.empty()))
        return false;
    return presenter_->present(p);
}

bool Router::showPopup(std::string_view id)
{
    const auto* p = popups_ != nullptr ? popups_->find(id) : nullptr;
    return p != nullptr && showPopup(*p);
}

const ScreenSpec& Router::current() const
{
    static const ScreenSpec empty{.id = {},
                                  .title = {},
                                  .parent = {},
                                  .body = BodyTemplate::Full,
                                  .contextBar = ContextBarKind::Menu,
                                  .rail = {},
                                  .bodyComponent = {},
                                  .strip = {}};
    return current_ != nullptr ? *current_ : empty;
}

int Router::focusOf(std::string_view id) const noexcept
{
    for (std::size_t i = 0; i < memoryCount_; ++i)
        if (memory_[i].id == id)
            return memory_[i].focus;
    return -1;
}

void Router::rememberFocus(int focus) noexcept
{
    if (current_ == nullptr)
        return;
    for (std::size_t i = 0; i < memoryCount_; ++i)
        if (memory_[i].id == current_->id)
        {
            memory_[i].focus = focus;
            return;
        }
    if (memoryCount_ < memory_.size())
        memory_[memoryCount_++] = {current_->id, focus};
}
} // namespace ui
