// Adapter bridging services::plugin::ClapHostService + core::CommandBus into ui::plugin::PluginAccess.
// Installed into ui::plugin::environment().access by app/Document.cpp. Message thread only.
#pragma once

#include "core/commands/CommandBusAccess.h"
#include "core/commands/SetParam.h"
#include "core/commands/fx/FxCommands.h"
#include "core/commands/track/TrackCommands.h"
#include "core/model/Project.h"
#include "services/plugin/ClapHost.h"
#include "services/plugin/UserPresets.h"
#include "ui/screens/effects/ModelFxSlots.h"
#include "ui/screens/plugin/PluginSlots.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace app
{
class PluginUiAdapter final : public ui::plugin::PluginAccess
{
public:
    PluginUiAdapter(core::CommandBus& bus, services::plugin::ClapHostService& host)
        : bus_(bus), host_(host),
          library_(juce::File(host.pluginsDir()).getSiblingFile("Presets").getChildFile("Plugins"))
    {
    }

    std::vector<ui::plugin::PluginEntry> catalogue() const override
    {
        std::vector<ui::plugin::PluginEntry> out;
        const auto& src = host_.catalogue();
        out.reserve(src.size());
        for (const auto& e : src)
            out.push_back({e.id, e.name, e.vendor, e.path, e.hasCustomScreen, e.trackCompatible, e.effectCompatible});
        return out;
    }

    void rescan() override { host_.rescan(); }

    bool hasLoaded(int scene, int track) const noexcept override
    {
        return host_.mounted(scene - 1, track - 1) != nullptr;
    }

    std::string_view loadedName(int scene, int track) const noexcept override
    {
        if (const auto* n = host_.mounted(scene - 1, track - 1))
            return n->name();
        return {};
    }

    std::string_view loadedVendor(int scene, int track) const noexcept override
    {
        if (const auto* n = host_.mounted(scene - 1, track - 1))
            return n->vendor();
        return {};
    }

    std::string_view loadedId(int scene, int track) const noexcept override
    {
        if (const auto* n = host_.mounted(scene - 1, track - 1))
            return n->id();
        return {};
    }

    std::string_view loadedPath(int scene, int track) const noexcept override
    {
        return host_.mountedSpec(scene - 1, track - 1).path;
    }

    bool selectPlugin(int scene, int track, std::string_view path, std::string_view pluginId) override
    {
        const int s0 = (scene - 1) & 15;
        const int t0 = (track - 1) & 15;
        core::model::PluginTrack spec{};
        spec.path = std::string(path);
        spec.pluginId = std::string(pluginId);
        for (const auto& e : host_.catalogue())
            if ((pluginId.empty() || e.id == pluginId) && (path.empty() || e.path == path))
            {
                spec.name = e.name;
                if (spec.path.empty())
                    spec.path = e.path;
                if (spec.pluginId.empty())
                    spec.pluginId = e.id;
                break;
            }
        if (core::commands::hasProject(bus_))
            bus_.execute(core::commands::track::setPlugin(s0, t0, spec));
        else
            host_.mount(s0, t0, spec);
        return host_.mounted(s0, t0) != nullptr;
    }

    ui::plugin::EffectPluginView effectPlugin(ui::fx::Slot slot, int scene, int track) const override
    {
        if (! core::commands::hasProject(bus_))
            return {};
        const auto at = ui::fx::pathOf(slot, scene, track);
        const auto& fx = core::fxSlotAt(core::commands::project(bus_), at);
        return {fx.plugin.path, fx.plugin.pluginId, fx.plugin.name,
                host_.hasEffect(scene - 1, ui::fx::EngineFxMeters::slotIndex(slot, track))};
    }

    bool selectEffectPlugin(ui::fx::Slot slot, int scene, int track, const ui::plugin::PluginEntry& entry) override
    {
        if (! entry.effectCompatible || ! core::commands::hasProject(bus_))
            return false;
        const auto at = ui::fx::pathOf(slot, scene, track);
        const auto& current = core::fxSlotAt(core::commands::project(bus_), at);
        const bool same = current.type == core::model::EffectType::Plugin && current.plugin.path == entry.path &&
                          current.plugin.pluginId == entry.id;
        const core::model::PluginTrack spec =
            same ? current.plugin
                 : core::model::PluginTrack{.path = entry.path, .pluginId = entry.id, .name = entry.name};
        if (! host_.mountEffect(scene - 1, ui::fx::EngineFxMeters::slotIndex(slot, track), spec))
            return false;
        if (! same)
            bus_.execute(std::make_unique<core::SelectFxType>(at, core::model::EffectType::Plugin, spec));
        return true;
    }

    void clearPlugin(int scene, int track) override
    {
        const int s0 = (scene - 1) & 15;
        const int t0 = (track - 1) & 15;
        if (core::commands::hasProject(bus_))
            bus_.execute(core::commands::track::setPlugin(s0, t0, core::model::PluginTrack{}));
        else
            host_.unmount(s0, t0);
    }

    bool bypass(int scene, int track) const noexcept override
    {
        if (const auto* n = host_.mounted(scene - 1, track - 1))
            return n->bypassed();
        return false;
    }

    void setBypass(int scene, int track, bool on) override
    {
        const int s0 = (scene - 1) & 15;
        const int t0 = (track - 1) & 15;
        if (auto* n = host_.mounted(s0, t0))
        {
            n->setBypassed(on);
            host_.bumpChanges();
        }
        if (core::commands::hasProject(bus_))
        {
            auto spec = core::commands::project(bus_)
                            .scenes[static_cast<std::size_t>(s0)]
                            .tracks[static_cast<std::size_t>(t0)]
                            .plugin;
            if (spec.bypass != on)
            {
                spec.bypass = on;
                bus_.execute(core::commands::track::setPlugin(s0, t0, std::move(spec)));
            }
        }
    }

    std::vector<ui::plugin::PluginPresetEntry> presets(int scene, int track,
                                                       std::optional<ui::fx::Slot> slot = {}) const override
    {
        std::vector<ui::plugin::PluginPresetEntry> entries;
        if (auto* node = editorNode(scene, track, slot))
        {
            for (auto& name : library_.names(node->id()))
                entries.push_back({name, name, true});
            const auto& factory = node->bundledPresets();
            for (const auto& preset : factory)
                entries.push_back({preset.key, preset.name});
            if (factory.empty() && node->paramCount() > 0)
                entries.push_back({"init", "INIT (parameter defaults)"});
        }
        return entries;
    }

    std::string_view currentPreset(int scene, int track, std::optional<ui::fx::Slot> slot = {}) const noexcept override
    {
        return host_.mountedSpec(scene - 1, track - 1, effectIndex(slot, track)).presetKey;
    }

    std::string presetNameError(const std::string& name) const override { return library_.nameError(name); }

    core::Result<bool> savePreset(int scene, int track, std::optional<ui::fx::Slot> slot, const std::string& name,
                                  bool replace) override
    {
        auto* node = editorNode(scene, track, slot);
        if (! node)
            return core::Error{"No plugin loaded"};
        return library_.save(node->id(), name, durableState(*node, slot.has_value()), replace);
    }

    bool loadPreset(int scene, int track, const ui::plugin::PluginPresetEntry& preset,
                    std::optional<ui::fx::Slot> slot = {}) override
    {
        auto* node = editorNode(scene, track, slot);
        if (! node)
            return false;
        const int s = scene - 1, t = track - 1;
        const auto fx = effectIndex(slot, track);
        auto next = host_.mountedSpec(s, t, fx);
        next.presetKey.clear();
        next.stateBase64.clear();
        if (preset.user)
        {
            auto state = library_.read(node->id(), preset.key);
            if (! state)
                return false;
            next.stateBase64 = std::move(state.value());
        }
        else if (preset.key != "init" || ! node->bundledPresets().empty())
            next.presetKey = preset.key;
        const auto before = durableState(*node, slot.has_value());
        if (! host_.recallPreset(s, t, fx, next))
            return false;
        if (core::commands::hasProject(bus_))
        {
            auto& project = core::commands::project(bus_);
            const auto& applied = host_.mountedSpec(s, t, fx);
            if (slot)
            {
                const auto at = ui::fx::pathOf(*slot, scene, track);
                core::fxSlotAt(project, at).plugin.stateBase64 = before;
                bus_.execute(std::make_unique<core::SelectFxType>(at, core::model::EffectType::Plugin, applied));
            }
            else
            {
                project.scenes[s].tracks[t].plugin.stateBase64 = before;
                bus_.execute(core::commands::track::setPlugin(s, t, applied));
            }
        }
        return true;
    }

    bool readKnobs(int scene, int track, ui::plugin::PluginKnobBank& out,
                   std::optional<ui::fx::Slot> slot = {}) const noexcept override
    {
        out = {};
        const auto* n = editorNode(scene, track, slot);
        if (n == nullptr)
        {
            out.tab.set("PLUGIN");
            return false;
        }
        std::array<services::plugin::KnobReadout, 6> raw{};
        char tabBuf[SIXTEEN_TAB_NAME_SIZE]{};
        const bool ok = n->readKnobs(raw, tabBuf);
        out.tab.set(tabBuf);
        for (std::size_t i = 0; i < 6; ++i)
        {
            auto& dst = out.columns[i];
            dst.active = raw[i].active;
            dst.stepped = raw[i].stepped;
            dst.bipolar = raw[i].bipolar;
            dst.paramId = raw[i].paramId;
            dst.norm = raw[i].norm;
            dst.label.set(raw[i].label);
            dst.valueText.set(raw[i].valueText);
        }
        return ok;
    }

    bool turnKnob(int scene, int track, int col0, int detents, bool fine,
                  std::optional<ui::fx::Slot> slot = {}) noexcept override
    {
        auto* n = editorNode(scene, track, slot);
        if (n == nullptr)
            return false;
        if (slot && core::commands::hasProject(bus_))
        {
            const auto at = ui::fx::pathOf(*slot, scene, track);
            const auto& saved = core::fxSlotAt(core::commands::project(bus_), at).plugin.stateBase64;
            const auto before = n->parameterEditPending() && ! saved.empty() ? saved : n->saveStateBase64();
            if (! before.empty())
            {
                prepareStateEditor(*n);
                if (stateEditor_ && (mirrorState_ == before || stateEditor_->loadStateBase64(before)))
                {
                    if (! n->turnKnob(col0, detents, fine, stateEditor_.get()))
                        return false;
                    mirrorState_ = stateEditor_->saveStateBase64();
                    commitEffectState(*slot, scene, track, before, mirrorState_);
                    host_.bumpChanges();
                    return true;
                }
            }
        }
        const bool ok = n->turnKnob(col0, detents, fine);
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    bool pushKnob(int scene, int track, int col0) noexcept override
    {
        auto* n = host_.mounted(scene - 1, track - 1);
        if (n == nullptr)
            return false;
        const bool ok = n->pushKnob(col0);
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    int pageCount(int scene, int track) const noexcept override
    {
        const auto* n = host_.mounted(scene - 1, track - 1);
        return n != nullptr ? n->pageCount() : 0;
    }

    int currentPage(int scene, int track) const noexcept override
    {
        const auto* n = host_.mounted(scene - 1, track - 1);
        return n != nullptr ? n->currentPage() : 0;
    }

    void setPage(int scene, int track, int page) noexcept override
    {
        if (auto* n = host_.mounted(scene - 1, track - 1))
        {
            n->setPage(page);
            host_.bumpChanges();
        }
    }

    bool hasCustomScreen(int scene, int track, std::optional<ui::fx::Slot> slot = {}) const noexcept override
    {
        const auto* n = editorNode(scene, track, slot);
        return n != nullptr && n->hasCustomScreen();
    }

    ui::plugin::PluginScreenFrame screenRender(int scene, int track, std::uint32_t nowMs,
                                               std::optional<ui::fx::Slot> slot = {}) noexcept override
    {
        auto* n = editorNode(scene, track, slot);
        if (n == nullptr)
            return {};
        const auto bmp = n->renderScreen(nowMs);
        return {bmp.argb, bmp.width, bmp.height, bmp.stride, bmp.serial};
    }

    bool screenTouch(int scene, int track, int phase, int x, int y,
                     std::optional<ui::fx::Slot> slot = {}) noexcept override
    {
        auto* n = editorNode(scene, track, slot);
        if (n == nullptr)
            return false;
        if (slot && phase == SIXTEEN_TOUCH_DOWN)
            gestureStart_ = durableState(*n);
        n->beforeScreenInput();
        const bool ok = n->touchScreen(phase, x, y);
        n->afterScreenInput();
        if (slot && phase == SIXTEEN_TOUCH_UP)
        {
            commitEffectState(*slot, scene, track, gestureStart_, durableState(*n));
            gestureStart_.clear();
        }
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    bool screenSelectorTurn(int scene, int track, int delta, std::optional<ui::fx::Slot> slot = {}) noexcept override
    {
        auto* n = editorNode(scene, track, slot);
        if (n == nullptr)
            return false;
        const auto before = slot ? durableState(*n) : std::string{};
        n->beforeScreenInput();
        const bool ok = n->selectorTurn(delta);
        n->afterScreenInput();
        if (slot && ok)
            commitEffectState(*slot, scene, track, before, durableState(*n));
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    bool screenSelectorPush(int scene, int track, std::optional<ui::fx::Slot> slot = {}) noexcept override
    {
        auto* n = editorNode(scene, track, slot);
        if (n == nullptr)
            return false;
        const auto before = slot ? durableState(*n) : std::string{};
        n->beforeScreenInput();
        const bool ok = n->selectorPush();
        n->afterScreenInput();
        if (slot && ok)
            commitEffectState(*slot, scene, track, before, durableState(*n));
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    std::uint32_t changes() const noexcept override { return host_.changes(); }

private:
    static std::optional<int> effectIndex(std::optional<ui::fx::Slot> slot, int track)
    {
        return slot ? std::optional<int>(ui::fx::EngineFxMeters::slotIndex(*slot, track)) : std::nullopt;
    }

    services::plugin::ClapAudioNode* editorNode(int scene, int track, std::optional<ui::fx::Slot> slot) const noexcept
    {
        if (slot)
            return host_.mountedEffect(scene - 1, ui::fx::EngineFxMeters::slotIndex(*slot, track));
        return host_.mounted(scene - 1, track - 1);
    }

    void prepareStateEditor(const services::plugin::ClapAudioNode& node, bool effect = true)
    {
        if (stateEditor_ && stateEditor_->id() == node.id() && stateEditor_->absPath() == node.absPath())
            return;
        std::string error;
        stateEditor_.reset(
            services::plugin::ClapAudioNode::create(node.absPath(), node.id(), node.sampleRate(), error, effect, true));
        mirrorState_.clear();
    }

    std::string durableState(services::plugin::ClapAudioNode& node, bool effect = true)
    {
        if (! node.parameterEditPending())
            return node.saveStateBase64();
        prepareStateEditor(node, effect);
        if (! stateEditor_)
            return {};
        mirrorState_.clear();
        return node.editorState(stateEditor_.get());
    }

    void commitEffectState(ui::fx::Slot slot, int scene, int track, const std::string& before, const std::string& after)
    {
        if (after.empty() || before == after || ! core::commands::hasProject(bus_))
            return;
        const auto at = ui::fx::pathOf(slot, scene, track);
        host_.acknowledgeEffectEdit(scene - 1, ui::fx::EngineFxMeters::slotIndex(slot, track), after);
        bus_.execute(core::makeSetParam<std::string>(
            core::fxSlotPath(at), "PLUGIN EDIT",
            [](core::model::Project& project, const core::model::ParamPath& path) -> std::string&
            { return core::fxSlotAt(project, path).plugin.stateBase64; }, at, after));
    }

    // One inactive instance shared across the editor; never connected to an audio slot.
    std::unique_ptr<services::plugin::ClapAudioNode> stateEditor_;
    std::string mirrorState_;
    std::string gestureStart_;
    core::CommandBus& bus_;
    services::plugin::ClapHostService& host_;
    services::plugin::UserPresets library_;
};
} // namespace app
