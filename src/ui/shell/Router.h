// Router (UID-206, UI-400…408, UID-171): navigation over the ScreenRegistry's
// `parent` / `opens` graph. open() validates the target against the registry (feature flags respected: a flagged
// variant replaces its reference descriptor only while its flag is on, UID-225); the breadcrumb is the parent chain
// from HOME to the current screen (UID-300), so BACK always goes one level up the breadcrumb (UI-400) and HOME returns
// to the root from any depth, closing popups (UI-005). The focus position is remembered per screen (UI-009).
// showPopup() validates a PopupSpec and hands it to the presenter (the Shell), which owns the popup widgets.
#pragma once

#include "ui/descriptors/PopupSpec.h"
#include "ui/descriptors/ScreenSpec.h"
#include "ui/shell/FeatureFlags.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace ui
{
class PopupPresenter
{
public:
    virtual ~PopupPresenter() = default;
    virtual bool present(const PopupSpec&) = 0;
    virtual bool popupOpen() const = 0;
    virtual void dismissTop() = 0; // BACK: cancel the top popup
    virtual void dismissAll() = 0; // HOME: close every popup without applying
};

class Router
{
public:
    static constexpr std::size_t kMaxDepth = 8;
    static constexpr std::string_view kRoot = "home";

    explicit Router(const ScreenRegistry& screens, const FeatureFlags* flags = nullptr,
                    const PopupRegistry* popups = &PopupRegistry::builtin());
    void setPresenter(PopupPresenter* p) noexcept { presenter_ = p; }

    bool open(std::string_view screenId); // false: unknown / disabled screen (or a popup id with no presenter)
    void back();                          // closes the top popup first, then one level up
    void home();
    bool showPopup(const PopupSpec&);
    bool showPopup(std::string_view popupId);

    const ScreenSpec& current() const; // the root's spec, or a static empty spec before any screen exists
    bool hasCurrent() const noexcept { return current_ != nullptr; }
    std::span<const std::string_view> breadcrumb() const noexcept { return {titles_.data(), depth_}; }
    std::span<const std::string_view> path() const noexcept { return {ids_.data(), depth_}; }

    // Resolves an id to the descriptor in effect (flags respected), null when absent or disabled.
    const ScreenSpec* resolve(std::string_view id) const noexcept;
    // Breadcrumb of any registered screen (titles from the root), false when its parent chain is broken / cyclic.
    bool breadcrumbOf(std::string_view id, std::array<std::string_view, kMaxDepth>& out, std::size_t& depth) const;

    int focusOf(std::string_view id) const noexcept;            // remembered focus, -1 = none
    void rememberFocus(int focus) noexcept;                     // for the current screen
    std::uint32_t changes() const noexcept { return changes_; } // bumped on every screen change
    int lastDirection() const noexcept { return direction_; }   // +1 deeper, -1 back up, 0 home / jump

private:
    void setCurrent(const ScreenSpec*, int direction);

    const ScreenRegistry& screens_;
    const FeatureFlags* flags_;
    const PopupRegistry* popups_;
    PopupPresenter* presenter_ = nullptr;
    const ScreenSpec* current_ = nullptr;
    std::array<std::string_view, kMaxDepth> titles_{}, ids_{};
    std::size_t depth_ = 0;
    struct Memory
    {
        std::string_view id;
        int focus;
    };
    std::array<Memory, 64> memory_{};
    std::size_t memoryCount_ = 0;
    std::uint32_t changes_ = 0;
    int direction_ = 0;
};
} // namespace ui
