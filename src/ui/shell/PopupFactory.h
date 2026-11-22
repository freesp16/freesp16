// Popups that a package builds itself: a PopupSpec registered with APP_REGISTER_POPUP gets its widget
// from a factory registered under the popup's id, so ShellApi::open("<id>") / Router::showPopup present the populated
// popup (SELECT FX: the slot's effect grid) instead of the generic one built from the spec's ListSpec. The factory
// runs on the message thread when the popup opens (it may allocate) and reads what it needs through the ShellApi
// (current screen, selected track, values). A factory returning null falls back to the generic popup.
#pragma once

#include "core/util/Registrar.h"
#include "ui/descriptors/PopupSpec.h"
#include "ui/shell/Popup.h"

#include <memory>
#include <string_view>
#include <vector>

namespace ui
{
class ShellApi;

using PopupFactoryFn = std::unique_ptr<PopupWidget> (*)(const PopupSpec&, ShellApi&);

class PopupFactoryRegistry
{
public:
    struct Entry
    {
        std::string_view id;
        PopupFactoryFn create;
    };
    static PopupFactoryRegistry& builtin() noexcept
    {
        static PopupFactoryRegistry registry;
        return registry;
    }
    void add(std::string_view popupId, PopupFactoryFn create) { entries_.push_back({popupId, create}); }
    PopupFactoryFn find(std::string_view popupId) const noexcept
    {
        for (const auto& e : entries_)
            if (e.id == popupId)
                return e.create;
        return nullptr;
    }

private:
    std::vector<Entry> entries_;
};
} // namespace ui

// APP_REGISTER_POPUP_FACTORY("fx.select", &makeSelectFx)
#define APP_REGISTER_POPUP_FACTORY(id, factory)                                                                        \
    APP_STATIC_REGISTRAR(::ui::PopupFactoryRegistry::builtin().add(id, factory))
