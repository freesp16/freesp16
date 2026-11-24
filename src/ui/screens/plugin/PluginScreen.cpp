// Plugin screen (`track.plugin`) and plugin popups (`plugin.select`, `plugin.presets`).
//
// Body ownership (locked design):
//   - Standard host chrome stays intact: StatusRow (top 0..40 lpx), ContextBar (40..136 lpx, TrackContext),
//     and 6-column KnobStrip (624..768 lpx).
//   - PluginPanel owns BodyTemplate::Full ({16, 152, 1248, 456} lpx = 780 x 285 physical px at 800 x 480).
//   - Plugins exposing org.sixteen.screen/1 render directly into a 780 x 285 ARGB8888 surface blitted via
//     DrawList::blit and supply the active 6-knob bank + tab name through StripOwner.
//   - Plugins without a custom screen get a native 6-column paged parameter view + PRESETS / SELECT PLUGIN / BYPASS.
#include "ui/descriptors/ModuleSpec.h"
#include "ui/descriptors/PopupSpec.h"
#include "ui/descriptors/ScreenSpec.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/plugin/PluginSlots.h"
#include "ui/shell/Layout.h"
#include "ui/shell/PopupFactory.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/Toggle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;

constexpr RectF kBody{16, L::contentTop, 1248, L::contentH}; // {16, 152, 1248, 456} lpx = 780 x 285 px
constexpr RectF kEmptyPickBtn{490, 404, 300, 64};

// Generic fallback header actions inside kBody
constexpr RectF kBtnPresets{748, 168, 152, 56};
constexpr RectF kBtnSelect{912, 168, 184, 56};
constexpr RectF kBtnBypass{1108, 168, 136, 56};
constexpr RectF kBtnPrevPage{496, 168, 60, 56};
constexpr RectF kBtnNextPage{672, 168, 60, 56};

constexpr RectF paramCardRect(int col) noexcept
{
    return {32.0f + static_cast<float>(col) * 204.0f, 252.0f, 192.0f, 336.0f};
}

// Lifetime-safe wrapper around ListPopup that owns backing label strings for ListItem::label string_views.
class OwningListPopup final : public PopupWidget
{
public:
    struct Storage
    {
        std::vector<std::string> labels;
        std::vector<plugin::PluginEntry> plugins;
        std::vector<plugin::PluginPresetEntry> presets;
    };

    OwningListPopup(std::shared_ptr<Storage> storage, ListRequest req)
        : PopupWidget(PopupSpec::Kind::List), storage_(std::move(storage)), inner_(std::move(req))
    {
    }

    RectF bounds() const noexcept override { return inner_.bounds(); }
    void build(DrawList& dl, const UiContext& ctx) const override
    {
        auto& in = const_cast<ListPopup&>(inner_);
        in.dx = dx;
        in.dy = dy;
        in.state = state;
        in.shell = shell;
        in.build(dl, ctx);
    }
    bool handle(const io::ControlEvent& e, UiContext& ctx) override
    {
        inner_.dx = dx;
        inner_.dy = dy;
        inner_.state = state;
        inner_.shell = shell;
        const bool consumed = inner_.handle(e, ctx);
        closed = inner_.closed;
        return consumed;
    }
    bool text(std::string_view utf8) override
    {
        const bool r = inner_.text(utf8);
        closed = inner_.closed;
        return r;
    }
    bool textKey(TextKey k) override
    {
        const bool r = inner_.textKey(k);
        closed = inner_.closed;
        return r;
    }
    void cancel() override
    {
        inner_.cancel();
        closed = inner_.closed;
    }
    bool dirty(double nowMs) const override { return inner_.dirty(nowMs); }

private:
    std::shared_ptr<Storage> storage_;
    ListPopup inner_;
};

std::unique_ptr<PopupWidget> makeSelectPlugin(const PopupSpec&, ShellApi& shell)
{
    auto& access = plugin::environment().pluginAccess();
    access.rescan();
    auto storage = std::make_shared<OwningListPopup::Storage>();
    storage->plugins = access.catalogue();

    const int scene = shell.state().scene;
    const int track = shell.state().selectedTrack;
    const std::string currentId(access.loadedId(scene, track));
    const std::string currentPath(access.loadedPath(scene, track));

    storage->labels.reserve(storage->plugins.size() + 2);
    storage->labels.emplace_back("NONE");
    for (const auto& entry : storage->plugins)
        storage->labels.emplace_back(entry.name.empty() ? entry.path : entry.name);

    ListRequest req;
    req.title = "SELECT PLUGIN";
    req.current = 0;
    req.items.push_back({storage->labels[0], {}, {}, -1, true});
    if (storage->plugins.empty())
    {
        storage->labels.emplace_back("NO .CLAP PLUGINS IN Plugins/");
        req.items.push_back({storage->labels.back(), {}, {}, -2, false});
    }
    else
    {
        for (std::size_t i = 0; i < storage->plugins.size(); ++i)
        {
            const auto& e = storage->plugins[i];
            req.items.push_back({storage->labels[i + 1], {}, {}, static_cast<int>(i), true});
            if ((! currentId.empty() && e.id == currentId) || (! currentPath.empty() && e.path == currentPath))
                req.current = static_cast<int>(i + 1);
        }
    }

    req.onChoose = [storage, &shell, scene, track](int, const ListItem& item)
    {
        auto& acc = plugin::environment().pluginAccess();
        if (item.value < 0)
        {
            acc.clearPlugin(scene, track);
            if (track >= 1 && track <= 16)
            {
                auto& s = shell.state().tracks[static_cast<std::size_t>(track - 1)];
                s.hasContent = false;
                s.name = {};
            }
            return;
        }
        const auto idx = static_cast<std::size_t>(item.value);
        if (idx >= storage->plugins.size())
            return;
        const auto& chosen = storage->plugins[idx];
        if (acc.selectPlugin(scene, track, chosen.path, chosen.id) && track >= 1 && track <= 16)
        {
            auto& s = shell.state().tracks[static_cast<std::size_t>(track - 1)];
            s.hasContent = true;
            s.name = acc.loadedName(scene, track);
        }
    };
    return std::make_unique<OwningListPopup>(storage, std::move(req));
}

std::unique_ptr<PopupWidget> makePluginPresets(const PopupSpec&, ShellApi& shell)
{
    auto& access = plugin::environment().pluginAccess();
    const int scene = shell.state().scene;
    const int track = shell.state().selectedTrack;
    auto storage = std::make_shared<OwningListPopup::Storage>();
    storage->presets = access.presets(scene, track);
    const std::string cur(access.currentPreset(scene, track));

    ListRequest req;
    req.title = "PRESETS";
    req.current = -1;
    if (storage->presets.empty())
    {
        storage->labels.emplace_back(access.hasLoaded(scene, track) ? "NO PRESETS" : "NO PLUGIN LOADED");
        req.items.push_back({storage->labels.back(), {}, {}, -1, false});
    }
    else
    {
        storage->labels.reserve(storage->presets.size());
        for (std::size_t i = 0; i < storage->presets.size(); ++i)
        {
            storage->labels.emplace_back(storage->presets[i].name.empty() ? storage->presets[i].key
                                                                          : storage->presets[i].name);
            req.items.push_back({storage->labels.back(), {}, {}, static_cast<int>(i), true});
            if (! cur.empty() && (storage->presets[i].key == cur || storage->presets[i].name == cur))
                req.current = static_cast<int>(i);
        }
    }
    req.onChoose = [storage, scene, track, &shell](int, const ListItem& item)
    {
        if (item.value < 0)
            return;
        const auto idx = static_cast<std::size_t>(item.value);
        if (idx < storage->presets.size())
        {
            const bool loaded =
                plugin::environment().pluginAccess().loadPreset(scene, track, storage->presets[idx].key);
            ToastRequest toast;
            toast.text = loaded ? storage->labels[idx] : "Could not load preset";
            shell.showToast(std::move(toast));
        }
    };
    return std::make_unique<OwningListPopup>(storage, std::move(req));
}

class PluginPanel final : public Widget, public StripOwner
{
public:
    explicit PluginPanel(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state) {}

    RectF bounds() const noexcept override { return kBody; }

    std::string_view stripTab() const noexcept override
    {
        syncBank();
        return bank_.tab.view().empty() ? std::string_view("PLUGIN") : bank_.tab.view();
    }

    void stripColumn(int col, ParamColumnModel& c) const override
    {
        if (col < 0 || col >= 6)
            return;
        if (col == 0)
            syncBank();
        const auto& k = bank_.columns[static_cast<std::size_t>(col)];
        if (! k.active)
        {
            c.spec = nullptr;
            return;
        }
        auto& sp = knobSpecs_[static_cast<std::size_t>(col)];
        sp = {};
        sp.id = kColumnIds[static_cast<std::size_t>(col)];
        sp.label = k.label.view();
        sp.shortLabel = k.label.view();
        sp.type = core::ParamType::Continuous;
        sp.minValue = 0.0f;
        sp.maxValue = 1.0f;
        sp.defaultValue = 0.5f;
        sp.bipolar = k.bipolar;
        c.spec = &sp;
        c.value = std::clamp(k.norm, 0.0f, 1.0f);
        c.valueText = k.valueText.view();
        c.stepped = k.stepped;
        c.disabled = false;
        c.readOnly = false;
    }

    bool stripTurnUnbound(int col, int detents, bool fine) override
    {
        if (state_ == nullptr || col < 0 || col >= 6 || detents == 0)
            return false;
        auto& access = plugin::environment().pluginAccess();
        const bool ok = access.turnKnob(state_->scene, state_->selectedTrack, col, detents, fine);
        if (ok)
        {
            bankDirty_ = true;
            syncBank();
        }
        return ok;
    }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        const auto& p = ctx.palette;
        auto& access = plugin::environment().pluginAccess();
        const int scene = state_->scene;
        const int track = state_->selectedTrack;
        const auto colour = theme::trackColour(m1::padColour(*shell_, track));
        lastChanges_ = access.changes();
        syncBank();

        if (! access.hasLoaded(scene, track))
        {
            dl.fill(kBody, p.surface1, tk::radiusM);
            dl.stroke(kBody, colour, tk::borderTrackFrame, tk::radiusM);
            dl.text(kBody.x + kBody.w * 0.5f, 296.0f, Align::Centre, TextStyleId::Display, p.text2, "NO PLUGIN LOADED");
            dl.text(kBody.x + kBody.w * 0.5f, 344.0f, Align::Centre, TextStyleId::Body, p.text3,
                    "Place .clap bundles in Plugins/ on USB storage, then select one for this track.");
            Button::draw(dl, ctx, kEmptyPickBtn, {"SELECT PLUGIN", ButtonStyle::Primary, false, false, true});
            return;
        }

        const bool bypassed = access.bypass(scene, track);
        if (access.hasCustomScreen(scene, track))
        {
            const auto nowMs = static_cast<std::uint32_t>(std::max(0.0, ctx.nowMs));
            const auto frame = access.screenRender(scene, track, nowMs);
            if (frame.argb != nullptr && frame.width > 0 && frame.height > 0)
            {
                dl.blit(kBody, frame.argb, frame.width, frame.height, frame.stride, frame.serial);
                if (bypassed)
                {
                    dl.fill(kBody, theme::withOpacity(p.bg, 0.55f));
                    dl.fill({kBody.right() - 128, kBody.y + 16, 112, 32}, p.warn, tk::radiusS);
                    dl.text(kBody.right() - 72, kBody.y + 32, Align::Centre, TextStyleId::Label, p.onAccent, "BYPASSED");
                }
                return;
            }
        }

        // Generic fallback UI (for any CLAP plugin without org.sixteen.screen/1)
        dl.fill(kBody, p.surface1, tk::radiusM);
        dl.stroke(kBody, colour, tk::borderTrackFrame, tk::radiusM);
        draw::hline(dl, kBody.x + 16, kBody.right() - 16, 238.0f, p.border);

        const auto name = access.loadedName(scene, track);
        const auto vendor = access.loadedVendor(scene, track);
        const auto preset = access.currentPreset(scene, track);
        dl.text(kBody.x + 24, 184.0f, Align::Left, TextStyleId::Title, p.text1,
                name.empty() ? std::string_view("CLAP PLUGIN") : name);
        subtitle_.format("%.*s%s%.*s", static_cast<int>(vendor.size()), vendor.data(),
                         (! vendor.empty() && ! preset.empty()) ? "  \xC2\xB7  " : "", static_cast<int>(preset.size()),
                         preset.data());
        if (! subtitle_.view().empty())
            dl.text(kBody.x + 24, 214.0f, Align::Left, TextStyleId::Caption, p.text2, subtitle_.view());

        const int pages = std::max(1, access.pageCount(scene, track));
        const int page = std::clamp(access.currentPage(scene, track), 0, pages - 1);
        Button::draw(dl, ctx, kBtnPrevPage, {"<", ButtonStyle::Plain, false, page <= 0, false});
        pageText_.format("PG %d/%d", page + 1, pages);
        dl.text(kBtnPrevPage.right() + (kBtnNextPage.x - kBtnPrevPage.right()) * 0.5f, 196.0f, Align::Centre,
                TextStyleId::Label, p.text1, pageText_.view());
        Button::draw(dl, ctx, kBtnNextPage, {">", ButtonStyle::Plain, false, page + 1 >= pages, false});

        Button::draw(dl, ctx, kBtnPresets, {"PRESETS", ButtonStyle::Plain, false, false, false});
        Button::draw(dl, ctx, kBtnSelect, {"SELECT PLUGIN", ButtonStyle::Plain, false, false, false});
        Toggle::draw(dl, ctx, kBtnBypass, {"BYPASS", bypassed, false, false, false, p.warn});

        for (int col = 0; col < 6; ++col)
        {
            const auto r = paramCardRect(col);
            const auto& k = bank_.columns[static_cast<std::size_t>(col)];
            if (! k.active)
            {
                dl.fill(r, p.bg, tk::radiusM);
                dl.stroke(r, p.border, 1.0f, tk::radiusM);
                dl.text(r.x + r.w * 0.5f, r.y + r.h * 0.5f, Align::Centre, TextStyleId::Caption, p.text3, "--");
                continue;
            }
            dl.fill(r, p.surface2, tk::radiusM);
            dl.stroke(r, p.borderStrong, 1.0f, tk::radiusM);
            knobNum_[static_cast<std::size_t>(col)].format("K%d", col + 1);
            dl.text(r.x + 14, r.y + 22, Align::Left, TextStyleId::Caption, colour,
                    knobNum_[static_cast<std::size_t>(col)].view());
            dl.text(r.x + r.w * 0.5f, r.y + 72, Align::Centre, TextStyleId::Label, p.text2, k.label.view());
            dl.text(r.x + r.w * 0.5f, r.y + 156, Align::Centre, TextStyleId::ValueL, bypassed ? p.text3 : p.text1,
                    k.valueText.view());

            const RectF bar{r.x + 20, r.y + 224, r.w - 40, 10};
            dl.fill(bar, p.surface1, 5.0f);
            const float norm = std::clamp(k.norm, 0.0f, 1.0f);
            if (k.bipolar)
            {
                const float mid = bar.x + bar.w * 0.5f;
                const float pos = bar.x + bar.w * norm;
                const float a = std::min(mid, pos), b = std::max(mid, pos);
                dl.fill({a, bar.y, std::max(4.0f, b - a), bar.h}, bypassed ? p.text3 : colour, 5.0f);
            }
            else
                dl.fill({bar.x, bar.y, std::max(4.0f, bar.w * norm), bar.h}, bypassed ? p.text3 : colour, 5.0f);

            dl.text(r.x + r.w * 0.5f, r.y + 286, Align::Centre, TextStyleId::Caption, p.text3, "DRAG / TURN");
        }
    }

    bool dirty(double) const override
    {
        if (state_ == nullptr)
            return false;
        const auto& access = plugin::environment().pluginAccess();
        if (access.changes() != lastChanges_)
            return true;
        return access.hasLoaded(state_->scene, state_->selectedTrack) &&
               access.hasCustomScreen(state_->scene, state_->selectedTrack);
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        auto& access = plugin::environment().pluginAccess();
        const int scene = state_->scene;
        const int track = state_->selectedTrack;

        if (! access.hasLoaded(scene, track))
        {
            if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
            {
                shell_->open("plugin.select");
                return true;
            }
            if (touch::isTouch(e) && e.value == touch::kDown && contains(kEmptyPickBtn, touch::x(e), touch::y(e)))
            {
                shell_->open("plugin.select");
                return true;
            }
            return false;
        }

        if (access.hasCustomScreen(scene, track))
        {
            if (e.control == io::Control::Selector && e.kind == Kind::Turn)
            {
                const bool ok = access.screenSelectorTurn(scene, track, e.value);
                bankDirty_ = true;
                return ok;
            }
            if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
            {
                const bool ok = access.screenSelectorPush(scene, track);
                bankDirty_ = true;
                return ok;
            }
            if (touch::isTouch(e))
            {
                const float cx = touch::x(e), cy = touch::y(e);
                const bool inside = contains(kBody, cx, cy);
                if (e.value == touch::kDown && ! inside)
                    return false;
                if (e.value == touch::kDown)
                    touchActive_ = true;
                if (! touchActive_)
                    return false;
                const int px = std::clamp(static_cast<int>(std::lround((cx - kBody.x) / touch::kToCanvas)), 0,
                                          plugin::kScreenWidth - 1);
                const int py = std::clamp(static_cast<int>(std::lround((cy - kBody.y) / touch::kToCanvas)), 0,
                                          plugin::kScreenHeight - 1);
                const int phase = e.value == touch::kDown ? 0 : (e.value == touch::kDrag ? 1 : 2);
                if (phase == 2)
                    touchActive_ = false;
                const bool ok = access.screenTouch(scene, track, phase, px, py);
                bankDirty_ = true;
                return ok || inside;
            }
            return false;
        }

        // Generic fallback input
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            const int pages = std::max(1, access.pageCount(scene, track));
            const int next = std::clamp(access.currentPage(scene, track) + (e.value > 0 ? 1 : -1), 0, pages - 1);
            access.setPage(scene, track, next);
            bankDirty_ = true;
            return true;
        }
        if (touch::isTouch(e) && e.value == touch::kDown)
        {
            const float x = touch::x(e), y = touch::y(e);
            if (contains(kBtnPresets, x, y))
            {
                shell_->open("plugin.presets");
                return true;
            }
            if (contains(kBtnSelect, x, y))
            {
                shell_->open("plugin.select");
                return true;
            }
            if (contains(kBtnBypass, x, y))
            {
                access.setBypass(scene, track, ! access.bypass(scene, track));
                return true;
            }
            if (contains(kBtnPrevPage, x, y))
            {
                access.setPage(scene, track, std::max(0, access.currentPage(scene, track) - 1));
                bankDirty_ = true;
                return true;
            }
            if (contains(kBtnNextPage, x, y))
            {
                const int pages = std::max(1, access.pageCount(scene, track));
                access.setPage(scene, track, std::min(pages - 1, access.currentPage(scene, track) + 1));
                bankDirty_ = true;
                return true;
            }
            for (int col = 0; col < 6; ++col)
                if (contains(paramCardRect(col), x, y) && bank_.columns[static_cast<std::size_t>(col)].active)
                {
                    shell_->dragColumn(col);
                    return true;
                }
            return contains(kBody, x, y);
        }
        return false;
    }

private:
    static constexpr std::array<std::string_view, 6> kColumnIds = {
        "plugin.k1", "plugin.k2", "plugin.k3", "plugin.k4", "plugin.k5", "plugin.k6",
    };

    void syncBank() const noexcept
    {
        if (state_ == nullptr)
            return;
        auto& access = plugin::environment().pluginAccess();
        const int s = state_->scene, t = state_->selectedTrack;
        const std::uint32_t ch = access.changes();
        if (! bankDirty_ && s == bankScene_ && t == bankTrack_ && ch == bankChanges_ &&
            ! access.hasCustomScreen(s, t))
            return;
        access.readKnobs(s, t, bank_);
        bankScene_ = s;
        bankTrack_ = t;
        bankChanges_ = ch;
        bankDirty_ = false;
    }

    ShellApi* shell_;
    UiState* state_;
    mutable plugin::PluginKnobBank bank_ {};
    mutable std::array<core::ParamSpec, 6> knobSpecs_ {};
    mutable int bankScene_ = -1, bankTrack_ = -1;
    mutable std::uint32_t bankChanges_ = 0xFFFFFFFFu;
    mutable std::uint32_t lastChanges_ = 0xFFFFFFFFu;
    mutable bool bankDirty_ = true;
    mutable FixedText<64> subtitle_ {};
    mutable FixedText<16> pageText_ {};
    mutable std::array<FixedText<4>, 6> knobNum_ {};
    bool touchActive_ = false;
};

std::unique_ptr<Widget> makePluginPanel(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<PluginPanel>(a);
}

constexpr ModuleSpec kPluginModule{"plugin", "PLUGIN", "PluginThumb", "plugin.strip", "track.plugin", 0b10000};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("PluginPanel", &ui::makePluginPanel)
APP_REGISTER_MODULE((ui::kPluginModule))
APP_REGISTER_POPUP((ui::PopupSpec{.kind = ui::PopupSpec::Kind::List, .id = "plugin.select", .title = "SELECT PLUGIN"}))
APP_REGISTER_POPUP((ui::PopupSpec{.kind = ui::PopupSpec::Kind::List, .id = "plugin.presets", .title = "PRESETS"}))
APP_REGISTER_POPUP_FACTORY("plugin.select", &ui::makeSelectPlugin)
APP_REGISTER_POPUP_FACTORY("plugin.presets", &ui::makePluginPresets)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.plugin",
                                    .title = "PLUGIN",
                                    .parent = "track.menu",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = {},
                                    .bodyComponent = "PluginPanel",
                                    .strip = {"PLUGIN", {}}}))
