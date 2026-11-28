#include "ui/widgets/EffectPanel.h"
#include "ui/screens/plugin/PluginSlots.h"

#include "core/params/ParamRegistry.h"
#include "ui/screens/effects/FxCatalogue.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/PopupFactory.h"
#include "ui/shell/ShellApi.h"
#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/EffectIcon.h"
#include "ui/widgets/FxGrid.h"
#include "ui/widgets/GridPopup.h"
#include "ui/widgets/OptionSelector.h"
#include "ui/widgets/Segmented.h"
#include "ui/widgets/Toggle.h"
#include "ui/widgets/TrackTile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
using Kind = io::ControlEvent::Kind;

constexpr RectF kPanel{16, 152, 1248, 456};
constexpr RectF kBanner{24, 240, 1216, 80};
constexpr RectF kBannerButton{1032, 252, 192, 56};
constexpr std::string_view kBusItems[] = {"MASTER", "SEND"};

// Knob glyph (components/visuals.md "KnobGlyph"): Ø 72 (value arc r 33, 4 lpx, 270° from 7:30 to 4:30), body r 26
// surface-3, pointer 3 lpx text-1 from r 8 to r 22. The value arc is a run of 4 lpx dots 2 lpx apart along a cached
// unit circle (no trigonometry per frame).
constexpr float kArcR = 33, kArcW = 4, kBodyR = 26, kStart = 135, kSweep = 270;
constexpr int kArcSteps = 78; // ≈ 2 lpx per dot along the arc
struct ArcTable
{
    std::array<float, kArcSteps + 1> c{}, s{};
    ArcTable()
    {
        for (int i = 0; i <= kArcSteps; ++i)
        {
            const float a = (kStart + kSweep * static_cast<float>(i) / kArcSteps) * 3.14159265f / 180.0f;
            c[static_cast<std::size_t>(i)] = std::cos(a);
            s[static_cast<std::size_t>(i)] = std::sin(a);
        }
    }
};
const ArcTable& arcTable()
{
    static const ArcTable t;
    return t;
}

void knobGlyph(DrawList& dl, const tk::Palette& p, float cx, float cy, float pos, bool bipolar, std::uint32_t arc)
{
    pos = std::clamp(pos, 0.0f, 1.0f);
    dl.stroke({cx - kArcR, cy - kArcR, 2 * kArcR, 2 * kArcR}, p.borderStrong, kArcW, kArcR);
    dl.fill({cx - 27, cy + 24, 54, 13}, p.surface1); // the open quarter at the bottom
    const auto& t = arcTable();
    const int end = static_cast<int>(std::lround(pos * kArcSteps)), mid = kArcSteps / 2;
    const int a = bipolar ? std::min(mid, end) : 0, b = bipolar ? std::max(mid, end) : end;
    if (b > a)
        for (int i = a; i <= b; ++i)
            dl.fill({cx + kArcR * t.c[static_cast<std::size_t>(i)] - kArcW / 2,
                     cy + kArcR * t.s[static_cast<std::size_t>(i)] - kArcW / 2, kArcW, kArcW},
                    arc, kArcW / 2);
    dl.fill({cx - kBodyR, cy - kBodyR, 2 * kBodyR, 2 * kBodyR}, p.surface3, kBodyR);
    const float pc = t.c[static_cast<std::size_t>(end)], ps = t.s[static_cast<std::size_t>(end)];
    glyph::line(dl, cx + 8 * pc, cy + 8 * ps, cx + 22 * pc, cy + 22 * ps, 3, p.text1);
}

// Keep the FX header, including BYPASS and SELECT FX, accessible on every editor page.
constexpr RectF kEditor{16, 236, 1248, 372};
constexpr RectF kPresets{312, 160, 144, 56};
constexpr RectF kEditorMode{470, 160, 152, 56};

GridRequest slotPicker(ShellApi& shell, fx::Slot slot, int scene, int track)
{
    auto& slots = fx::environment().slotAccess();
    return FxGrid::request(slot, track, slots.effect(slot, scene, track),
                           [&shell, slot, scene, track](const engine::fx::EffectDescriptor& d)
                           {
                               if (d.id == "plugin")
                                   plugin::showEffectPicker(shell, slot, scene, track);
                               else
                                   fx::environment().slotAccess().selectEffect(slot, scene, track, d.id);
                           });
}

fx::Slot& lastBusPage() noexcept
{
    return fx::busPage(); // the bus page opens on the page last shown (SEND first), or the one MIXER asked for
}

const core::ParamRegistry& registryOf(const ShellApi* shell)
{
    return shell != nullptr ? shell->params() : core::builtinParams();
}

EffectPanel::Control controlOf(const core::ParamSpec& s, const EffectUiSpec* ui, std::string_view optionId)
{
    if (ui != nullptr)
        for (const auto& o : ui->options)
            if (o.param == optionId || o.param == s.id)
                return o.control == EffectOptionUi::Control::Toggle      ? EffectPanel::Control::Toggle
                       : o.control == EffectOptionUi::Control::Segmented ? EffectPanel::Control::Segmented
                                                                         : EffectPanel::Control::Selector;
    if (s.type == core::ParamType::Bool)
        return EffectPanel::Control::Toggle;
    const auto n = static_cast<int>(std::lround(s.maxValue - s.minValue)) + 1;
    return n == 2 && s.enumLabels.size() == 2 ? EffectPanel::Control::Segmented : EffectPanel::Control::Selector;
}

int enumIndex(const core::ParamSpec& s, float v) noexcept
{
    const int i = static_cast<int>(std::lround(v - s.minValue));
    return std::clamp(i, 0, std::max(0, static_cast<int>(s.enumLabels.size()) - 1));
}
} // namespace

EffectPanel::EffectPanel(RectF bounds, ShellApi* shell, UiState* state, fx::Slot slot)
    : bounds_(bounds.w > 0 ? bounds : kPanel), shell_(shell), state_(state != nullptr ? state : &ownState_), slot_(slot)
{
    EffectIcon::warm();
    (void)arcTable();
    refresh();
}

EffectPanel::~EffectPanel() = default;

int EffectPanel::scene() const noexcept
{
    return state_->scene;
}

int EffectPanel::track() const noexcept
{
    return fixedTrack_ > 0 ? fixedTrack_ : state_->selectedTrack;
}

fx::FxMode EffectPanel::mode() const
{
    return fx::environment().slotAccess().mode(scene());
}

EffectPanel::Key EffectPanel::currentKey() const
{
    const auto& slots = fx::environment().slotAccess();
    const auto effect = slots.effect(slot_, scene(), track());
    const bool custom = effect == "plugin" && fx::active(slot_, mode()) &&
                        plugin::environment().pluginAccess().hasCustomScreen(scene(), track(), slot_);
    return {slot_, scene(), track(), effect, syncOn(), custom, slots.changes()};
}

void EffectPanel::refresh() const
{
    const auto k = currentKey();
    if (! bound_ || ! (k == key_))
        rebind(k);
}

bool EffectPanel::syncOn() const
{
    return bound_ && syncSpec_ != nullptr && value(*syncSpec_) >= 0.5f;
}

float EffectPanel::value(const core::ParamSpec& s) const
{
    if (shell_ == nullptr)
        return s.defaultValue;
    return shell_->values().get(s, fx::scopeOf(s, scene(), track()));
}

bool EffectPanel::lockedValue(const core::ParamSpec& s, float& v) const
{
    if (shell_ == nullptr || state_->stepHeld <= 0 || ! s.lockable)
        return false;
    return shell_->values().lockOf(s, fx::scopeOf(s, scene(), track()), state_->stepHeld, v);
}

void EffectPanel::setValue(const core::ParamSpec& s, float v)
{
    if (shell_ != nullptr)
        shell_->values().set(s, fx::scopeOf(s, scene(), track()), v);
}

void EffectPanel::rebind(const Key& k) const
{
    const auto& env = fx::environment();
    const auto& params = registryOf(shell_);
    const auto* d = fx::findEffect(env.effectRegistry(), k.effect);
    effect_ = d != nullptr ? d : &fx::noneDescriptor();
    ui_ = env.uiRegistry().find(effect_->id);
    hints_ = env.hintsRegistry().find(effect_->id);
    bound_ = true;
    syncSpec_ = fx::syncOption(params, slot_, *effect_);
    const bool sync = syncOn();

    for (std::size_t n = 0; n < 6; ++n)
    {
        const auto* s = fx::resolveParam(params, slot_, *effect_, effect_->params[n]);
        if (s != nullptr && sync)
            if (const auto* div = fx::syncForm(params, *s))
                s = div;
        columns_[n] = s;
    }
    options_ = {};
    cellOption_.fill(-1);
    for (std::size_t i = 0; i < effect_->options.size() && i < options_.size(); ++i)
    {
        const auto id = effect_->options[i];
        const auto* s = fx::resolveParam(params, slot_, *effect_, id);
        int cell = static_cast<int>(i);
        if (hints_ != nullptr && hints_->optionCell[i] >= 1 && hints_->optionCell[i] <= 6)
            cell = hints_->optionCell[i] - 1;
        if (s == nullptr || (s->type != core::ParamType::Bool && s->enumLabels.empty()))
            continue; // not registered for this slot: the cell stays empty (OQ-15)
        options_[i] = {s, controlOf(*s, ui_, id), cell};
        if (cellOption_[static_cast<std::size_t>(cell)] < 0)
            cellOption_[static_cast<std::size_t>(cell)] = static_cast<std::int8_t>(i);
    }

    // Knob strip (UID-125): the six per-slot ParamSpecs; double-buffered so the shell sees a new source each time.
    stripIndex_ ^= 1;
    auto& strip = strips_[stripIndex_];
    strip.tab = fx::tab(slot_);
    strip.groupMask = 0;
    for (std::size_t n = 0; n < 6; ++n)
        strip.columns[n] =
            (! k.custom || slotControls_) && columns_[n] != nullptr ? columns_[n]->id : std::string_view{};
    if (shell_ != nullptr)
        shell_->bindStrip(&strip);

    // Visual: keep the renderer when the kind is unchanged (type changes between two lfoCurve effects).
    std::string_view widgetKey = ui_ != nullptr ? ui_->visualWidget : std::string_view{};
    const auto kind = effect_->visual;
    WidgetCreateFn create = nullptr;
    std::string_view createdKey;
    for (const auto& e : WidgetRegistry::builtin().all())
    {
        const bool match = ! widgetKey.empty()
                               ? e.name == widgetKey
                               : (! kind.empty() && kind != "none" && e.name.size() == 9 + kind.size() &&
                                  e.name.substr(0, 9) == "fxvisual." && e.name.substr(9) == kind);
        if (match)
        {
            create = e.create;
            createdKey = e.name;
            break;
        }
    }
    if (create == nullptr)
        visual_.reset();
    else if (! visual_ || visual_->kind() != createdKey.substr(createdKey.find('.') + 1))
    {
        auto w = create(ScreenSpec{}, WidgetArgs{fxzone::zone, effect_->id, &params, nullptr, nullptr});
        visual_.reset(dynamic_cast<FxVisual*>(w.get()));
        if (visual_)
            (void)w.release();
    }
    adapter_ = hints_ != nullptr && hints_->visual != nullptr ? hints_->visual : defaultVisualAdapter(kind);

    // Header texts.
    if (fx::isInsert(slot_))
        caption_.format("%.*s \xC2\xB7 INSERT", static_cast<int>(fx::tab(slot_).size()), fx::tab(slot_).data());
    else
        caption_.format("%.*s \xC2\xB7 FX MODE %.*s", static_cast<int>(fx::tab(slot_).size()), fx::tab(slot_).data(),
                        static_cast<int>(fx::modeName(mode()).size()), fx::modeName(mode()).data());
    if (columns_[5] != nullptr)
        mixText_.format("%.*s is 0", static_cast<int>(columns_[5]->label.size()), columns_[5]->label.data());
    key_ = currentKey();
}

void EffectPanel::bindSlot(std::string_view slot, int track)
{
    slot_ = slot == "fx2"      ? fx::Slot::Fx2
            : slot == "send"   ? fx::Slot::Send
            : slot == "master" ? fx::Slot::Master
                               : fx::Slot::Fx1;
    fixedTrack_ = std::clamp(track, 0, 16);
    bound_ = false;
    refresh();
}

bool EffectPanel::mixHint() const
{
    const auto* mix = columns_[5];
    return mix != nullptr && mix->defaultValue == 0 && value(*mix) == 0; // UID-837: silent because Mix is 0
}

std::uint32_t EffectPanel::ink(const theme::tk::Palette& p) const noexcept
{
    return fx::isInsert(slot_) ? theme::trackColour(track()) : p.accent; // UID-836
}

int EffectPanel::focusCount() const noexcept
{
    int n = headerCount();
    if (! fx::isInsert(slot_) && ! fx::active(slot_, fx::environment().slotAccess().mode(state_->scene)))
        return n + 1; // the banner's USE … button
    for (const auto c : cellOption_)
        n += c >= 0 ? 1 : 0;
    return n;
}

EffectPanel::Target EffectPanel::focusTarget() const noexcept
{
    const int header = fx::isInsert(slot_) ? 2 : 3;
    if (focus_ < 0)
        return Target::SelectFx;
    if (focus_ == 0)
        return Target::SelectFx;
    if (focus_ == 1)
        return Target::Bypass;
    if (focus_ == 2 && header == 3)
        return Target::BusPage;
    if (focus_ == header && key_.effect == "plugin")
        return Target::Presets;
    if (! fx::isInsert(slot_) && ! fx::active(slot_, fx::environment().slotAccess().mode(state_->scene)))
        return Target::Banner;
    return Target::OptionCell;
}

// --- paint ----------------------------------------------------------------------------------------------------------
void EffectPanel::paint(DrawList& dl, const RectF& damage, const UiContext& ctx) const
{
    (void)damage; // the DrawList is always the whole frame; the backends diff it (ui/render/Damage.h)
    refresh();
    const auto& p = ctx.palette;
    dl.fill(bounds_, p.surface1, tk::radiusM);
    paintHeader(dl, ctx);
    if (editingPlugin())
    {
        const auto frame = plugin::environment().pluginAccess().screenRender(
            scene(), track(), static_cast<std::uint32_t>(ctx.nowMs), slot_);
        if (frame.argb)
        {
            paintedEditorSerial_ = frame.serial;
            dl.blit(kEditor, frame.argb, frame.width, frame.height, frame.stride, frame.serial);
            return;
        }
    }
    if (fx::isNone(effect_->id))
    {
        dl.text(640, 380, Align::Centre, TextStyleId::Body, p.text3,
                "No effect in this slot. SELECT FX to choose one.");
        return;
    }
    paintOptions(dl, ctx);
    paintVisual(dl, ctx);
    paintKnobs(dl, ctx);
}

void EffectPanel::paintHeader(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const auto target = focusTarget();
    EffectIcon::draw(dl, iconIdFor(effect_->id), 40, 168, 40, p.text1);
    auto caption = caption_.view();
    auto name = effect_->name;
    char shortened[512];
    if (effect_->id == "plugin")
    {
        const auto loaded = plugin::environment().pluginAccess().effectPlugin(slot_, scene(), track());
        if (! loaded.name.empty())
            name = TrackTile::middleTruncate(loaded.name, kPresets.x - 108, TextStyleId::Title, ctx.measure, shortened,
                                             sizeof shortened);
        if (! loaded.loaded)
            caption = "PLUGIN UNAVAILABLE";
    }
    dl.text(96, 174, Align::Left, TextStyleId::Caption, p.text2, caption);
    dl.text(96, 200, Align::Left, TextStyleId::Title, p.text1, name);
    if (effect_->id == "plugin")
        Button::draw(dl, ctx, kPresets, {.label = "PRESETS", .focused = focus_ >= 0 && target == Target::Presets});
    else if (! fx::isNone(effect_->id) && mixHint())
    {
        dl.fill(kMixHint, p.surface2, 16);
        glyph::draw(dl, glyph::Kind::Info, 408, 180, 16, p.text2);
        dl.text(440, 189, Align::Left, TextStyleId::Body, p.text2, mixText_.view());
    }
    if (! fx::isInsert(slot_))
    {
        SegmentedModel seg;
        seg.items = kBusItems;
        seg.selected = slot_ == fx::Slot::Send ? 1 : 0;
        seg.focused = focus_ >= 0 && target == Target::BusPage;
        Segmented::draw(dl, ctx, kBusPage, seg);
        const auto m = mode();
        for (int i = 0; i < 2; ++i) // a disabled slot's segment carries a 12 lpx ⊘
        {
            const auto s = i == 0 ? fx::Slot::Master : fx::Slot::Send;
            if (! fx::active(s, m))
            {
                const float w = ctx.measure ? ctx.measure(TextStyleId::Label, kBusItems[i]) : 48;
                EffectIcon::draw(dl, "none", kBusPage.x + 60 + 120 * static_cast<float>(i) + w / 2 + 6, 182, 12,
                                 p.text2);
            }
        }
    }
    if (key_.custom)
        Button::draw(dl, ctx, kEditorMode, {editingPlugin() ? "SLOT MIX" : "EDITOR"});
    ToggleModel bypass;
    bypass.label = "BYPASS";
    bypass.on = fx::environment().slotAccess().bypass(slot_, scene(), track());
    bypass.onColour = p.warn;
    bypass.focused = focus_ >= 0 && target == Target::Bypass;
    Toggle::draw(dl, ctx, kBypass, bypass);
    ButtonModel select;
    select.label = "SELECT FX";
    select.focused = focus_ >= 0 && target == Target::SelectFx;
    Button::draw(dl, ctx, kSelectFx, select);
    draw::hline(dl, 32, 1232, 232.5f, p.border);
}

void EffectPanel::paintOptions(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    if (! fx::isInsert(slot_) && ! fx::active(slot_, mode()))
    {
        // The slot is silent in this FX mode (UI-267, UID-839): a caution banner across cells 1–6 with a one-tap fix.
        dl.fill(kBanner, p.surface2, tk::radiusM);
        glyph::draw(dl, glyph::Kind::Caution, 44, 268, 24, p.warn);
        FixedText<64> text;
        text.format("%.*s is inactive in FX mode %.*s", static_cast<int>(fx::tab(slot_).size()), fx::tab(slot_).data(),
                    static_cast<int>(fx::modeName(mode()).size()), fx::modeName(mode()).data());
        dl.text(84, 280, Align::Left, TextStyleId::Body, p.text1, text.view());
        ButtonModel fix;
        fix.label = slot_ == fx::Slot::Send ? "USE SEND" : "USE MASTER";
        fix.focused = focus_ >= 0 && focusTarget() == Target::Banner;
        Button::draw(dl, ctx, kBannerButton, fix);
        return;
    }
    int focusIndex = headerCount();
    for (int c = 0; c < 6; ++c)
    {
        const int oi = cellOption_[static_cast<std::size_t>(c)];
        if (oi < 0)
            continue;
        const auto& o = options_[static_cast<std::size_t>(oi)];
        const bool focused = focus_ == focusIndex++;
        const float v = value(*o.spec);
        const auto r = cell(c);
        switch (o.control)
        {
        case Control::Toggle:
        {
            ToggleModel m;
            m.label = o.spec->label;
            m.on = v >= 0.5f;
            m.focused = focused;
            Toggle::draw(dl, ctx, r, m);
            break;
        }
        case Control::Segmented:
        {
            SegmentedModel m;
            m.items = o.spec->enumLabels;
            m.selected = enumIndex(*o.spec, v);
            // An option bracketed to its knobs needs no caption: the bracket names what it governs (LO-FI PRE | POST).
            m.caption = hints_ != nullptr && hints_->bracketOption == oi ? std::string_view{} : o.spec->label;
            m.captionBackground = p.surface1;
            m.focused = focused;
            Segmented::draw(dl, ctx, {r.x, kOptionY + 24, r.w, 56}, m);
            break;
        }
        case Control::Selector:
        case Control::None:
        {
            OptionSelectorModel m;
            m.caption = o.spec->label;
            m.value = o.spec->enumLabels.empty() ? std::string_view{}
                                                 : o.spec->enumLabels[static_cast<std::size_t>(enumIndex(*o.spec, v))];
            m.focused = focused;
            OptionSelector::draw(dl, ctx, r, m);
            break;
        }
        }
    }
    // UID-828: a bracket from the bound option's bottom centre to the knobs it governs (LO-FI PRE | POST → 4–5).
    if (hints_ != nullptr && hints_->bracketOption >= 0 && hints_->bracketKnobMask != 0 &&
        hints_->bracketOption < static_cast<int>(options_.size()) &&
        options_[static_cast<std::size_t>(hints_->bracketOption)].spec != nullptr)
    {
        const auto& o = options_[static_cast<std::size_t>(hints_->bracketOption)];
        const float bx = cell(o.cell).x + 96;
        int first = 6, last = -1;
        for (int n = 0; n < 6; ++n)
            if ((hints_->bracketKnobMask >> n) & 1u)
                first = std::min(first, n), last = std::max(last, n);
        draw::vline(dl, bx, kOptionY + kOptionH, 328, p.borderStrong);
        draw::hline(dl, std::min(bx, knobX(first)), std::max(bx, knobX(last)), 328, p.borderStrong);
        for (int n = first; n <= last; ++n)
            if ((hints_->bracketKnobMask >> n) & 1u)
                draw::vline(dl, knobX(n), 328, 508, p.borderStrong);
    }
}

void EffectPanel::paintVisual(DrawList& dl, const UiContext& ctx) const
{
    if (! visual_)
        return;
    const auto& env = fx::environment();
    EffectVisualInputs in;
    for (std::size_t n = 0; n < 6; ++n)
        if (const auto* s = columns_[n])
        {
            float v = value(*s);
            lockedValue(*s, v);
            in.knob01[n] = s->normalised(v);
        }
    for (std::size_t i = 0; i < in.options.size(); ++i)
        if (const auto* s = options_[i].spec)
            in.options[i] = static_cast<int>(std::lround(value(*s) - s->minValue));
    in.sync = key_.sync;
    in.bpm = std::strtod(state_->bpm.text, nullptr);
    if (in.bpm <= 0)
        in.bpm = 120;
    in.live = env.meters != nullptr ? env.meters(slot_, track(), env.metersUser) : nullptr;
    in.slotColour = ink(ctx.palette);
    in.bus = ! fx::isInsert(slot_);
    VisualModel m;
    if (adapter_ != nullptr)
        adapter_(in, m);
    m.colour = in.slotColour;
    m.playing = state_->playing;
    m.lfoStartMs = lfoOriginMs_;
    if (fx::isInsert(slot_) && env.slotAccess().bypass(slot_, scene(), track()))
        m.opacity = 0.4f;
    visual_->setModel(m);
    visual_->build(dl, ctx);
}

void EffectPanel::paintKnobs(DrawList& dl, const UiContext& ctx) const
{
    const auto& p = ctx.palette;
    const bool bypassed = fx::isInsert(slot_) && fx::environment().slotAccess().bypass(slot_, scene(), track());
    const auto base = theme::withOpacity(ink(p), bypassed ? 0.4f : 1.0f);
    for (std::size_t n = 0; n < 6; ++n)
    {
        const auto* s = columns_[n];
        if (s == nullptr)
            continue; // an empty position is an empty glyph (UID-826)
        float v = value(*s);
        const bool locked = lockedValue(*s, v);
        const auto arc = locked ? theme::withOpacity(p.mod, bypassed ? 0.4f : 1.0f) : base; // UID-836
        knobGlyph(dl, p, knobX(static_cast<int>(n)), kKnobY, s->normalised(v), s->bipolar, arc);
    }
}

// --- input ----------------------------------------------------------------------------------------------------------
void EffectPanel::openSelectFx()
{
    if (shell_ == nullptr)
        return;
    slotControls_ = false;
    bound_ = false;
    shell_->showGrid(slotPicker(*shell_, slot_, scene(), track()));
}

void EffectPanel::openSelector(const core::ParamSpec& s)
{
    if (shell_ == nullptr || s.enumLabels.empty())
        return;
    ListRequest r;
    r.title = s.label;
    for (std::size_t i = 0; i < s.enumLabels.size(); ++i)
        r.items.push_back({s.enumLabels[i], {}, {}, static_cast<int>(i)});
    r.current = enumIndex(s, value(s));
    const auto* spec = &s;
    r.onChoose = [this, spec](int, const ListItem& it)
    { setValue(*spec, spec->minValue + static_cast<float>(it.value)); };
    shell_->showList(std::move(r));
}

void EffectPanel::selectBusPage(fx::Slot page)
{
    auto& slots = fx::environment().slotAccess();
    if (slots.mode(scene()) != fx::FxMode::Both) // parity modes: the segment also sets the mode (UID-839)
        slots.setMode(scene(), page == fx::Slot::Send ? fx::FxMode::Send : fx::FxMode::Master);
    slot_ = page;
    lastBusPage() = page;
    refresh();
}

void EffectPanel::activateOption(const Option& o, float x)
{
    const float v = value(*o.spec);
    switch (o.control)
    {
    case Control::Toggle:
        setValue(*o.spec, v >= 0.5f ? 0.0f : 1.0f);
        refresh(); // SYNC swaps the rate/time columns (UID-830)
        break;
    case Control::Segmented:
    {
        const int n = static_cast<int>(o.spec->enumLabels.size());
        int i = (enumIndex(*o.spec, v) + 1) % std::max(n, 1); // push steps to the next segment
        if (x >= 0)
        {
            const auto r = cell(o.cell);
            i = std::clamp(static_cast<int>((x - r.x) / (r.w / static_cast<float>(n))), 0, n - 1);
        }
        setValue(*o.spec, o.spec->minValue + static_cast<float>(i));
        break;
    }
    case Control::Selector:
    case Control::None:
        openSelector(*o.spec);
        break;
    }
}

void EffectPanel::activate(int index, float x, float y)
{
    (void)y;
    focus_ = index;
    const auto target = focusTarget();
    auto& slots = fx::environment().slotAccess();
    switch (target)
    {
    case Target::SelectFx:
        openSelectFx();
        return;
    case Target::Presets:
        if (shell_)
            plugin::showEffectPresets(*shell_, slot_, scene(), track());
        return;
    case Target::Bypass:
        slots.setBypass(slot_, scene(), track(), ! slots.bypass(slot_, scene(), track()));
        return;
    case Target::BusPage:
        if (x >= 0)
            selectBusPage(x < kBusPage.x + kBusPage.w / 2 ? fx::Slot::Master : fx::Slot::Send);
        else
            selectBusPage(slot_ == fx::Slot::Send ? fx::Slot::Master : fx::Slot::Send);
        return;
    case Target::Banner:
        slots.setMode(scene(), slot_ == fx::Slot::Send ? fx::FxMode::Send : fx::FxMode::Master);
        refresh();
        return;
    case Target::OptionCell:
    {
        int k = index - headerCount();
        for (int c = 0; c < 6; ++c)
        {
            const int oi = cellOption_[static_cast<std::size_t>(c)];
            if (oi < 0)
                continue;
            if (k-- == 0)
            {
                activateOption(options_[static_cast<std::size_t>(oi)], x);
                return;
            }
        }
        return;
    }
    }
}

std::string_view EffectPanel::stripTab() const noexcept
{
    if (! editingPlugin())
        return {};
    plugin::environment().pluginAccess().readKnobs(scene(), track(), editorBank_, slot_);
    return editorBank_.tab.view();
}

void EffectPanel::stripColumn(int col, ParamColumnModel& column) const
{
    if (editingPlugin() && col >= 0 && col < 6)
        plugin::knobColumn(editorBank_, col, editorSpecs_[static_cast<std::size_t>(col)], column);
}

bool EffectPanel::stripTurnUnbound(int col, int detents, bool fine)
{
    return editingPlugin() &&
           plugin::environment().pluginAccess().turnKnob(scene(), track(), col, detents, fine, slot_);
}

bool EffectPanel::handle(const io::ControlEvent& e, UiContext&)
{
    refresh();
    if (key_.effect == "plugin" && touch::isTouch(e) && e.value == touch::kDown &&
        contains(kPresets, touch::x(e), touch::y(e)))
    {
        activate(headerCount() - 1);
        return true;
    }
    if (key_.custom && touch::isTouch(e) && e.value == touch::kDown && contains(kEditorMode, touch::x(e), touch::y(e)))
    {
        slotControls_ = ! slotControls_;
        editorTouch_ = false;
        rebind(currentKey());
        return true;
    }
    if (editingPlugin())
    {
        auto& access = plugin::environment().pluginAccess();
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
            return access.screenSelectorTurn(scene(), track(), e.value, slot_);
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
            return access.screenSelectorPush(scene(), track(), slot_);
        if (touch::isTouch(e))
        {
            const auto x = touch::x(e), y = touch::y(e);
            if (e.value == touch::kDown)
                editorTouch_ = contains(kEditor, x, y);
            if (editorTouch_)
            {
                const int phase = e.value == touch::kDown ? 0 : (e.value == touch::kDrag ? 1 : 2);
                const int px = std::clamp(static_cast<int>((x - kEditor.x) * plugin::kScreenWidth / kEditor.w), 0,
                                          plugin::kScreenWidth - 1);
                const int py = std::clamp(static_cast<int>((y - kEditor.y) * plugin::kScreenHeight / kEditor.h), 0,
                                          plugin::kScreenHeight - 1);
                access.screenTouch(scene(), track(), phase, px, py, slot_);
                editorTouch_ = phase != 2;
                return true;
            }
        }
    }
    if (e.control == io::Control::Selector && e.kind == Kind::Turn)
    {
        const int n = focusCount();
        focus_ = std::clamp(focus_ < 0 ? (e.value > 0 ? e.value - 1 : 0) : focus_ + e.value, 0, std::max(0, n - 1));
        return true;
    }
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        activate(focus_ < 0 ? 0 : focus_);
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        const float x = touch::x(e), y = touch::y(e);
        const bool bus = ! fx::isInsert(slot_);
        if (contains(hitArea(kSelectFx), x, y))
            return activate(0), true;
        if (contains(hitArea(kBypass), x, y))
            return activate(1), true;
        if (bus && contains(hitArea(kBusPage), x, y))
            return activate(2, x, y), true;
        if (bus && ! fx::active(slot_, mode()))
        {
            if (contains(hitArea(kBannerButton), x, y))
                return activate(headerCount()), true;
            return false;
        }
        int index = headerCount();
        for (int c = 0; c < 6; ++c)
        {
            const int oi = cellOption_[static_cast<std::size_t>(c)];
            if (oi < 0)
                continue;
            if (contains(cell(c), x, y))
                return activate(index, x, y), true;
            ++index;
        }
        // A drag on a knob glyph turns its strip column (UID-163, effects.md "Interactions"): the shell turns it.
        for (int n = 0; n < 6; ++n)
            if (columns_[static_cast<std::size_t>(n)] != nullptr && shell_ != nullptr &&
                std::fabs(x - knobX(n)) <= kKnobR + 8 && std::fabs(y - kKnobY) <= kKnobR + 8)
            {
                shell_->dragColumn(n);
                return true;
            }
        return false;
    }
    if (io::isStep(e.control))
        return false; // the shell turns the strip into STEP n; the glyphs follow the locks in paint
    return false;
}

bool EffectPanel::dirty(double nowMs) const
{
    const auto& slots = fx::environment().slotAccess();
    if (slots.effect(slot_, scene(), track()) != key_.effect || slots.changes() != key_.changes)
        return true;
    if (editingPlugin())
    {
        const auto frame = plugin::environment().pluginAccess().screenRender(scene(), track(),
                                                                             static_cast<std::uint32_t>(nowMs), slot_);
        return frame.argb && frame.serial != paintedEditorSerial_;
    }
    return visual_ && visual_->dirty(nowMs);
}

// --- registration ---------------------------------------------------------------------------------------------------
namespace
{
std::unique_ptr<Widget> makeEffectPanel(const ScreenSpec& s, const WidgetArgs& a)
{
    const auto slot = s.id == "track.fx2" ? fx::Slot::Fx2 : s.id == "mixer.busFx" ? lastBusPage() : fx::Slot::Fx1;
    return std::make_unique<EffectPanel>(a.bounds, a.shell, a.state, slot);
}
} // namespace
} // namespace ui

namespace ui
{
namespace
{
// fx.select by id (ShellApi::open("fx.select"), a rail item or TRACK MENU's FX card): the populated SELECT FX grid of
// the slot shown — FX2 on its page, the bus page last shown under MASTER/SEND FX, else FX1 of the selected track.
std::unique_ptr<PopupWidget> makeSelectFx(const PopupSpec&, ShellApi& shell)
{
    const auto id = shell.current().id;
    const auto slot = id == "track.fx2" ? fx::Slot::Fx2 : id == "mixer.busFx" ? lastBusPage() : fx::Slot::Fx1;
    const int scene = shell.state().scene, track = shell.state().selectedTrack;
    return std::make_unique<GridPopup>(slotPicker(shell, slot, scene, track));
}
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("EffectPanel", &ui::makeEffectPanel)
APP_REGISTER_POPUP_FACTORY("fx.select", &ui::makeSelectFx)
