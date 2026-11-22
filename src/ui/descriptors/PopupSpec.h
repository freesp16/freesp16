// Popups and feedback (07-ui-design/popups-and-feedback.md UID-900…916): list / setting / grid /
// modal / dialog / message / toast, with sizes and positions from 03-layout-grid.md "Popups and modals". The shell
// dims the frame with the scrim and rebinds the knob strip (UID-125) while a popup is open.
#pragma once

#include "core/util/Registrar.h"
#include "ui/descriptors/ListSpec.h"
#include "ui/descriptors/SettingsSpec.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ui
{
// A knob-strip column bound while the popup is open (UID-125): encoder n edits `param`.
struct PopupBinding
{
    std::uint8_t column; // 0–5
    std::string_view param;
};

struct PopupSpec
{
    enum class Kind : std::uint8_t { List, Setting, Grid, Modal, Dialog, Message, Toast } kind;
    std::string_view id, title;
    const ListSpec* list = nullptr;         // List / Grid
    const SettingsSpec* settings = nullptr; // Setting
    std::span<const PopupBinding> bindings {};
    std::string_view bodyComponent {};      // Modal / Dialog body widget key (e.g. "SoftwareKeyboard")
    double autoDismissMs = 0;               // Toast / Message: 0 = until dismissed
    std::string_view featureFlag {};
};

class PopupRegistry
{
public:
    static PopupRegistry& builtin() noexcept
    {
        static PopupRegistry registry;
        return registry;
    }
    void add(const PopupSpec& p) { popups_.push_back(p); }
    const PopupSpec* find(std::string_view id) const noexcept
    {
        for (const auto& p : popups_)
            if (p.id == id)
                return &p;
        return nullptr;
    }
    const std::vector<PopupSpec>& all() const noexcept { return popups_; }

private:
    std::vector<PopupSpec> popups_;
};
} // namespace ui

#define APP_REGISTER_POPUP(...) APP_STATIC_REGISTRAR(::ui::PopupRegistry::builtin().add(__VA_ARGS__))
