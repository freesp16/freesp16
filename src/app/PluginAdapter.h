// Adapter bridging services::plugin::ClapHostService + core::CommandBus into ui::plugin::PluginAccess.
// Installed into ui::plugin::environment().access by app/Document.cpp. Message thread only.
#pragma once

#include "core/commands/CommandBusAccess.h"
#include "core/commands/track/TrackCommands.h"
#include "core/model/Project.h"
#include "services/plugin/ClapHost.h"
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
    PluginUiAdapter(core::CommandBus& bus, services::plugin::ClapHostService& host) noexcept
        : bus_(bus), host_(host)
    {
    }

    std::vector<ui::plugin::PluginEntry> catalogue() const override
    {
        std::vector<ui::plugin::PluginEntry> out;
        const auto& src = host_.catalogue();
        out.reserve(src.size());
        for (const auto& e : src)
            out.push_back({e.id, e.name, e.vendor, e.path, e.hasCustomScreen});
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
        core::model::PluginTrack spec {};
        spec.path = std::string(path);
        spec.pluginId = std::string(pluginId);
        for (const auto& e : host_.catalogue())
            if ((! pluginId.empty() && e.id == pluginId) || (! path.empty() && e.path == path))
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

    void clearPlugin(int scene, int track) override
    {
        const int s0 = (scene - 1) & 15;
        const int t0 = (track - 1) & 15;
        if (core::commands::hasProject(bus_))
            bus_.execute(core::commands::track::setPlugin(s0, t0, core::model::PluginTrack {}));
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
            auto spec = core::commands::project(bus_).scenes[static_cast<std::size_t>(s0)].tracks[static_cast<std::size_t>(t0)].plugin;
            if (spec.bypass != on)
            {
                spec.bypass = on;
                bus_.execute(core::commands::track::setPlugin(s0, t0, std::move(spec)));
            }
        }
    }

    std::vector<ui::plugin::PluginPresetEntry> presets(int scene, int track) const override
    {
        std::vector<ui::plugin::PluginPresetEntry> entries;
        if (auto* node = host_.mounted(scene - 1, track - 1))
        {
            for (const auto& preset : node->bundledPresets())
                entries.push_back({preset.key, preset.name});
            if (entries.empty() && node->paramCount() > 0)
                entries.push_back({"init", "INIT (parameter defaults)"});
        }
        return entries;
    }

    std::string_view currentPreset(int scene, int track) const noexcept override
    {
        return host_.mountedSpec(scene - 1, track - 1).presetKey;
    }

    bool loadPreset(int scene, int track, std::string_view key) override
    {
        const int s0 = (scene - 1) & 15;
        const int t0 = (track - 1) & 15;
        auto* n = host_.mounted(s0, t0);
        if (n == nullptr)
            return false;
        if (key == "init" && n->bundledPresets().empty())
        {
            for (std::size_t i = 0; i < n->paramCount(); ++i)
                if (const auto* meta = n->paramByIndex(i))
                    if (! meta->readOnly)
                        n->setParamValue(meta->id, meta->defaultValue);
            host_.bumpChanges();
            return true;
        }
        const bool hasProject = core::commands::hasProject(bus_);
        const auto before = hasProject ? n->saveStateBase64() : std::string{};
        if (! host_.loadPreset(s0, t0, std::string(key)))
            return false;
        if (hasProject)
        {
            // Capture live knob edits in the inverse of the preset change.
            auto& old = core::commands::project(bus_).scenes[s0].tracks[t0].plugin;
            if (! before.empty())
                old.stateBase64 = before;
            bus_.execute(core::commands::track::setPlugin(s0, t0, host_.mountedSpec(s0, t0)));
        }
        return true;
    }

    bool readKnobs(int scene, int track, ui::plugin::PluginKnobBank& out) const noexcept override
    {
        out = {};
        const auto* n = host_.mounted(scene - 1, track - 1);
        if (n == nullptr)
        {
            out.tab.set("PLUGIN");
            return false;
        }
        std::array<services::plugin::KnobReadout, 6> raw {};
        char tabBuf[SIXTEEN_TAB_NAME_SIZE] {};
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

    bool turnKnob(int scene, int track, int col0, int detents, bool fine) noexcept override
    {
        auto* n = host_.mounted(scene - 1, track - 1);
        if (n == nullptr)
            return false;
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

    bool hasCustomScreen(int scene, int track) const noexcept override
    {
        const auto* n = host_.mounted(scene - 1, track - 1);
        return n != nullptr && n->hasCustomScreen();
    }

    ui::plugin::PluginScreenFrame screenRender(int scene, int track, std::uint32_t nowMs) noexcept override
    {
        auto* n = host_.mounted(scene - 1, track - 1);
        if (n == nullptr)
            return {};
        const auto bmp = n->renderScreen(nowMs);
        return {bmp.argb, bmp.width, bmp.height, bmp.stride, bmp.serial};
    }

    bool screenTouch(int scene, int track, int phase, int x, int y) noexcept override
    {
        auto* n = host_.mounted(scene - 1, track - 1);
        if (n == nullptr)
            return false;
        const bool ok = n->touchScreen(phase, x, y);
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    bool screenSelectorTurn(int scene, int track, int delta) noexcept override
    {
        auto* n = host_.mounted(scene - 1, track - 1);
        if (n == nullptr)
            return false;
        const bool ok = n->selectorTurn(delta);
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    bool screenSelectorPush(int scene, int track) noexcept override
    {
        auto* n = host_.mounted(scene - 1, track - 1);
        if (n == nullptr)
            return false;
        const bool ok = n->selectorPush();
        if (ok)
            host_.bumpChanges();
        return ok;
    }

    std::uint32_t changes() const noexcept override { return host_.changes(); }

private:
    core::CommandBus& bus_;
    services::plugin::ClapHostService& host_;
};
} // namespace app
