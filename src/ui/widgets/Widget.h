// The widget contract (07-ui-design/components/, UID-300…459): a widget appends to a DrawList in canvas lpx (no JUCE
// Component per widget), handles logical control events (03-ui/control-surface.md) and reports when it must be redrawn
// (damage tracking). Widgets receive models and ParamSpecs, never feature objects (UID-201); every colour, size and
// duration comes from the theme tokens (UID-050). Custom body components register by name with APP_REGISTER_WIDGET and
// are found through a ScreenSpec's bodyComponent (WidgetRegistry::dispatcher()).
#pragma once

#include "core/util/Registrar.h"
#include "io/controllers/ControlSurface.h"
#include "ui/descriptors/ScreenSpec.h"
#include "ui/render/DrawList.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/Widgets.h"

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace core
{
class ParamRegistry;
}

namespace ui
{
class ShellApi;
struct UiState;
class EngineFeeds;

// Focus and two-tap state of the current screen (UID-150…181). The shell owns the behaviour and may add members.
struct FocusState
{
    std::string_view screen {};
    int focused = -1;        // focus ring index, -1 = none
    bool stripBound = false; // first tap bound the knob strip (two-tap model)
};

// What a factory gets to build a widget for a screen (the shell may add members).
struct WidgetArgs
{
    RectF bounds {};                             // canvas lpx, inside the screen's body template
    std::string_view binds {};                   // the model path or ParamSpec id the widget shows
    const core::ParamRegistry* params = nullptr;
    ShellApi* shell = nullptr;                   // navigation, popups, strip binding (ui/shell/ShellApi.h)
    UiState* state = nullptr;                    // selection, transport, SHIFT/step hold (ui/shell/UiState.h)
    const EngineFeeds* feeds = nullptr;          // meters, playhead, engine events (ui/shell/Feeds.h)
};

struct UiContext { const theme::tk::Palette& palette; const TextMeasure& measure; double nowMs; FocusState& focus; };
class Widget {
public:
    virtual ~Widget() = default;
    virtual RectF bounds() const noexcept = 0;
    virtual void build(DrawList&, const UiContext&) const = 0;            // append only; no allocation after warm-up
    virtual bool handle(const io::ControlEvent&, UiContext&) { return false; }  // encoder, push, touch
    virtual bool dirty(double nowMs) const { (void) nowMs; return false; }             // animations / feeds → DamageTracker
};
using WidgetFactory = std::function<std::unique_ptr<Widget>(const ScreenSpec&, const WidgetArgs&)>;

using WidgetCreateFn = std::unique_ptr<Widget> (*)(const ScreenSpec&, const WidgetArgs&);
class WidgetRegistry
{
public:
    struct Entry
    {
        std::string_view name;
        WidgetCreateFn create;
    };
    static WidgetRegistry& builtin() noexcept
    {
        static WidgetRegistry registry;
        return registry;
    }
    void add(std::string_view name, WidgetCreateFn create) { entries_.push_back({name, create}); }
    WidgetCreateFn find(std::string_view name) const noexcept
    {
        for (const auto& e : entries_)
            if (e.name == name)
                return e.create;
        return nullptr;
    }
    // A WidgetFactory that builds a screen's bodyComponent from this registry (null when it is not registered).
    WidgetFactory dispatcher() const
    {
        return [this](const ScreenSpec& spec, const WidgetArgs& args) -> std::unique_ptr<Widget> {
            const auto create = find(spec.bodyComponent);
            return create != nullptr ? create(spec, args) : nullptr;
        };
    }
    const std::vector<Entry>& all() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};
} // namespace ui

// APP_REGISTER_WIDGET("WaveformEditor", &makeWaveformEditor)
#define APP_REGISTER_WIDGET(name, factory) APP_STATIC_REGISTRAR(::ui::WidgetRegistry::builtin().add(name, factory))
