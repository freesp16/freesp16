// Screen descriptors (schema = 07-ui-design/05-extensibility.md §2).
// UID-205: every screen and popup is a ScreenSpec; UID-206: navigation is the graph of `parent` / `opens` links (HOME,
// BACK and breadcrumbs are computed from it); UID-207: screens are registered in a ScreenRegistry and validated at
// start-up and in a unit test. Body templates: 07-ui-design/03-layout-grid.md. A screen is one .cpp under
// ui/screens/<id>/ with APP_REGISTER_SCREEN(...); the router, shell and other screens are never edited.
//
// Descriptors hold string_views and spans: register only data with static storage (literals, namespace-scope arrays).
#pragma once

#include "core/util/Registrar.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core
{
class ParamRegistry;
}

namespace ui
{
struct ScreenSpec;
struct WidgetArgs;
class Widget;

// Widget factory (ui/widgets/Widget.h): resolves a ScreenSpec's bodyComponent to a widget.
using WidgetFactory = std::function<std::unique_ptr<Widget>(const ScreenSpec&, const WidgetArgs&)>;

enum class BodyTemplate : std::uint8_t { Full, ContentMeter, RailContent, RailContentMeter, RailContentMeters2,
                                         RailMeterContent, Grid };                // 03-layout-grid.md
enum class ContextBarKind : std::uint8_t { Menu, TrackContext, TrackTypeBypass, None };
struct RailItemSpec { enum class Kind : std::uint8_t { Button, Toggle, Option } kind; std::string_view label;
                      std::string_view opens {}; std::string_view binds {}; };
struct StripSpec { std::string_view tab; std::array<std::string_view, 6> columns; std::uint8_t groupMask = 0; };
struct ScreenSpec {
    std::string_view id, title, parent;                   // parent → BACK + breadcrumb (UID-206)
    BodyTemplate body; ContextBarKind contextBar;
    std::span<const RailItemSpec> rail;                    // ≤ 4 (or 5 compact)
    std::string_view bodyComponent;                        // WidgetFactory key, e.g. "WaveformEditor"
    StripSpec strip; std::string_view meter {}; std::string_view featureFlag {};
};
class ScreenRegistry {                                     // validated at start-up and in a unit test (UID-207)
public:
    static ScreenRegistry& builtin() noexcept;             // filled by APP_REGISTER_SCREEN before main()
    void add(const ScreenSpec&);
    const ScreenSpec* find(std::string_view id) const;
    const std::vector<ScreenSpec>& all() const noexcept { return screens_; }
    // One message per problem (empty = valid): duplicate ids, unknown parents, unknown `opens` targets, a strip column
    // that is not a registered ParamSpec, more than 4 rail items, a body component the factory cannot build.
    // The UI framework may extend the checks (rects inside the template, UID-225 extension slots).
    std::vector<std::string> validate(const WidgetFactory&, const core::ParamRegistry&) const;

private:
    std::vector<ScreenSpec> screens_;
};
} // namespace ui

// APP_REGISTER_SCREEN((ui::ScreenSpec{...})): static registrar into ScreenRegistry::builtin().
#define APP_REGISTER_SCREEN(...) APP_STATIC_REGISTRAR(::ui::ScreenRegistry::builtin().add(__VA_ARGS__))
