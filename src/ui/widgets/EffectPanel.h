// EffectPanel (07-ui-design/components/visuals.md "EffectPanel"; screens/effects.md; UID-825…842, UI-260…268): the one
// panel every effect in every slot is edited on, generated from the EffectDescriptor (+ EffectUiSpec) and the
// ParamSpecs of the slot's current effect. A CLAP editor can take over the body below the header. Geometry from
// screens/effects.md "EffectPanel geometry": panel 16,152,1248×456; header (40 lpx icon, slot caption, name, MIX hint,
// MASTER | SEND on the bus page, BYPASS, SELECT FX); option cells x = 24 + 208·(n−1), y 240, 192 × 80; visual zone
// 32,336,1216×152; six knob glyphs centred (120 + 208·(n−1), 548), Ø 72. Nothing re-flows between effects (UID-003).
//
// Derived, never written per effect:
//   - knob n = EffectDescriptor::params[n−1] (page order, FX-067 / UID-826); "" = empty column and glyph;
//   - option i sits in cell i+1 (span order; a ParamSpec not registered for this slot leaves its cell empty — the
//     insert Compressor's LOOK AHEAD, OQ-15), unless PanelHints::optionCell moves it (UID-827);
//   - control kind from the ParamSpec (or EffectUiSpec::options): Bool → Toggle, 2-value Enum → Segmented with the
//     caption on a hairline, larger Enum → OptionSelector opening a ListPopup (UID-842);
//   - the knob strip is rebound to the six per-slot ParamSpecs (UID-125/UID-826): tab FX1 / FX2 / SEND FX /
//     MASTER FX; insert columns lockable (●), bus columns not and disabled while a step is held (UID-829) — both are
//     ParamSpec facts the shell renders; with SYNC on a column swaps to its `<id>.div` form (UID-830);
//   - the "MIX RATIO is 0" hint: knob 6's ParamSpec default is 0 and its value is 0 (UID-837, FX-093);
//   - the visual from EffectDescriptor::visual → widget "fxvisual.<kind>" (or EffectUiSpec::visualWidget), fed by the
//     effect's VisualAdapter (PanelHints::visual) or the kind's default (UID-834/835).
// Rotary focus order: SELECT FX → BYPASS → MASTER | SEND → option cells left to right; push activates.
#pragma once

#include "core/util/Registrar.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/screens/effects/VisualModel.h"
#include "ui/screens/plugin/PluginSlots.h"
#include "ui/shell/StripOwner.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace core
{
struct ParamSpec;
}

namespace ui
{
class ShellApi;
class ParamAccess;

// What a VisualAdapter gets: the six knob positions (normalised 0–1 of the bound ParamSpec, 0 for an empty column),
// up to four option values (descriptor option order; -1 = not registered), SYNC, the tempo, the live telemetry (null
// when none) and the slot ink.
struct EffectVisualInputs
{
    std::array<float, 6> knob01{};
    std::array<int, 4> options{-1, -1, -1, -1};
    bool sync = false;
    double bpm = 120;
    const fx::FxMeterView* live = nullptr; // engine::fx::FxMeterFeed's view (EngineFxMeters)
    std::uint32_t slotColour = 0;
    bool bus = false;
};
using VisualAdapter = void (*)(const EffectVisualInputs&, VisualModel& out) noexcept; // 0–1 → units, per effect

struct PanelHints
{                                                                  // optional, per effect; defaults are derived
    std::array<std::int8_t, 6> optionCell{-1, -1, -1, -1, -1, -1}; // UID-827: cell 1–6 per option, -1 = span order
    std::int8_t bracketOption = -1;                                // UID-828 (LO-FI PRE | POST → knobs 4–5)
    std::uint8_t bracketKnobMask = 0;                              // bit n−1 = knob n
    VisualAdapter visual = nullptr;
    std::string_view gridLabel{}; // SELECT FX label when it differs from the header name (FILTER)
};

class PanelHintsRegistry
{
public:
    struct Entry
    {
        std::string_view effectId;
        PanelHints hints;
    };
    static PanelHintsRegistry& builtin() noexcept
    {
        static PanelHintsRegistry registry;
        return registry;
    }
    void add(std::string_view effectId, const PanelHints& h) { entries_.push_back({effectId, h}); }
    const PanelHints* find(std::string_view effectId) const noexcept
    {
        for (const auto& e : entries_)
            if (e.effectId == effectId)
                return &e.hints;
        return nullptr;
    }

private:
    std::vector<Entry> entries_;
};

// The default adapter of a visual kind (used when an effect registers none).
VisualAdapter defaultVisualAdapter(std::string_view kind) noexcept;

class EffectPanel final : public Widget, public StripOwner
{
public:
    enum class Control : std::uint8_t
    {
        None,
        Toggle,
        Segmented,
        Selector
    };
    struct Option
    {
        const core::ParamSpec* spec = nullptr;
        Control control = Control::None;
        int cell = -1; // 0–5
    };
    // Focus targets in rotary order.
    enum class Target : std::uint8_t
    {
        SelectFx,
        Bypass,
        BusPage,
        Presets,
        OptionCell,
        Banner
    };

    // `slot` = the slot this screen edits (the bus page starts on SEND); shell/state may be null (tests of geometry).
    EffectPanel(RectF bounds, ShellApi* shell, UiState* state, fx::Slot slot);
    ~EffectPanel() override;

    // Rebinds the knob strip to the current effect's page order (UID-125/826); `slot` = "fx1", "fx2", "send" or
    // "master"; track 1–16 (inserts) — 0 keeps following the selected track.
    void bindSlot(std::string_view slot, int track);
    // Damage-driven paint: appends the whole panel (the DrawList is always the full frame); never re-flows (UID-003).
    void paint(DrawList&, const RectF& damage, const UiContext&) const;

    RectF bounds() const noexcept override { return bounds_; }
    void build(DrawList& dl, const UiContext& ctx) const override { paint(dl, bounds_, ctx); }
    bool handle(const io::ControlEvent&, UiContext&) override;
    bool dirty(double nowMs) const override;

    // State for tests and the screen code.
    fx::Slot slot() const noexcept { return slot_; }
    const engine::fx::EffectDescriptor* effect() const noexcept { return effect_; }
    const core::ParamSpec* column(int n0) const noexcept { return columns_[static_cast<std::size_t>(n0)]; }
    const Option& option(int i) const noexcept { return options_[static_cast<std::size_t>(i)]; }
    int focusIndex() const noexcept { return focus_; }
    Target focusTarget() const noexcept;
    const StripSpec& strip() const noexcept { return strips_[stripIndex_]; }
    const FxVisual* visual() const noexcept { return visual_.get(); }
    bool syncOn() const;
    bool mixHint() const;
    void openSelectFx();
    std::string_view stripTab() const noexcept override;
    void stripColumn(int, ParamColumnModel&) const override;
    bool stripTurnUnbound(int, int, bool) override;
    void setFocus(int index) noexcept { focus_ = index; }
    int focusCount() const noexcept;

    // Geometry (screens/effects.md "EffectPanel geometry").
    static constexpr RectF kSelectFx{1056, 160, 192, 56}, kBypass{896, 160, 144, 56}, kBusPage{640, 160, 240, 56};
    static constexpr RectF kMixHint{400, 172, 224, 32};
    static constexpr float kOptionY = 240, kOptionH = 80, kKnobY = 548, kKnobR = 36;
    static constexpr RectF cell(int c0, int span = 1) noexcept
    {
        return {24 + 208 * static_cast<float>(c0), kOptionY, 192 + 208 * static_cast<float>(span - 1), kOptionH};
    }
    static constexpr float knobX(int n0) noexcept { return 120 + 208 * static_cast<float>(n0); }

private:
    struct Key
    {
        fx::Slot slot;
        int scene, track;
        std::string_view effect;
        bool sync;
        bool custom;
        std::uint32_t changes;
        bool operator==(const Key&) const = default;
    };
    Key currentKey() const;
    void refresh() const;          // rebinds when slot, track, scene, effect or SYNC changed (no allocation unless the
    void rebind(const Key&) const; // visual kind changes)
    int headerCount() const noexcept { return (fx::isInsert(slot_) ? 2 : 3) + (key_.effect == "plugin" ? 1 : 0); }
    int scene() const noexcept;
    int track() const noexcept;
    float value(const core::ParamSpec&) const;
    void setValue(const core::ParamSpec&, float);
    bool lockedValue(const core::ParamSpec&, float& v) const;
    std::uint32_t ink(const theme::tk::Palette&) const noexcept;
    fx::FxMode mode() const;
    void activate(int focus, float x = -1, float y = -1);
    void activateOption(const Option&, float x);
    void openSelector(const core::ParamSpec&);
    void selectBusPage(fx::Slot page);
    void paintHeader(DrawList&, const UiContext&) const;
    void paintOptions(DrawList&, const UiContext&) const;
    void paintKnobs(DrawList&, const UiContext&) const;
    void paintVisual(DrawList&, const UiContext&) const;

    RectF bounds_;
    ShellApi* shell_;
    UiState* state_;
    UiState ownState_{};
    mutable fx::Slot slot_;
    int fixedTrack_ = 0;
    int focus_ = -1;

    mutable Key key_{};
    mutable bool bound_ = false;
    bool slotControls_ = false;
    bool editorTouch_ = false;
    mutable std::uint32_t paintedEditorSerial_ = 0;
    bool editingPlugin() const noexcept { return key_.custom && ! slotControls_; }
    mutable plugin::PluginKnobBank editorBank_{};
    mutable std::array<core::ParamSpec, 6> editorSpecs_{};
    mutable const engine::fx::EffectDescriptor* effect_ = nullptr;
    mutable const EffectUiSpec* ui_ = nullptr;
    mutable const PanelHints* hints_ = nullptr;
    mutable const core::ParamSpec* syncSpec_ = nullptr;
    mutable std::array<const core::ParamSpec*, 6> columns_{};
    mutable std::array<Option, 6> options_{};         // per descriptor option (≤ 6)
    mutable std::array<std::int8_t, 6> cellOption_{}; // option index per cell, -1 = empty
    mutable std::array<StripSpec, 2> strips_{};
    mutable std::size_t stripIndex_ = 0;
    mutable std::unique_ptr<FxVisual> visual_;
    mutable VisualAdapter adapter_ = nullptr;
    mutable FixedText<40> caption_, mixText_;
    double lfoOriginMs_ = 0;
};
} // namespace ui

// APP_FX_PANEL_HINTS("lofi", (ui::PanelHints{.bracketOption = 1, .bracketKnobMask = 0b11000})) in
// ui/screens/effects/hints/<id>.cpp (package-internal registrar).
#define APP_FX_PANEL_HINTS(effectId, ...)                                                                              \
    APP_STATIC_REGISTRAR(::ui::PanelHintsRegistry::builtin().add(effectId, __VA_ARGS__))
