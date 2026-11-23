#include "ui/screens/plugin/PluginSlots.h"

#include <algorithm>
#include <cstdio>

namespace ui::plugin
{
LocalPluginAccess::LocalPluginAccess() = default;

bool LocalPluginAccess::hasLoaded(int scene, int track) const noexcept
{
    return slots_[idx(scene, track)].loaded;
}

std::string_view LocalPluginAccess::loadedName(int scene, int track) const noexcept
{
    const auto& s = slots_[idx(scene, track)];
    return s.loaded ? s.name.view() : std::string_view {};
}

std::string_view LocalPluginAccess::loadedVendor(int scene, int track) const noexcept
{
    const auto& s = slots_[idx(scene, track)];
    return s.loaded ? s.vendor.view() : std::string_view {};
}

std::string_view LocalPluginAccess::loadedId(int scene, int track) const noexcept
{
    const auto& s = slots_[idx(scene, track)];
    return s.loaded ? s.id.view() : std::string_view {};
}

std::string_view LocalPluginAccess::loadedPath(int scene, int track) const noexcept
{
    const auto& s = slots_[idx(scene, track)];
    return s.loaded ? s.path.view() : std::string_view {};
}

bool LocalPluginAccess::selectPlugin(int scene, int track, std::string_view path, std::string_view pluginId)
{
    auto& s = slots_[idx(scene, track)];
    if (path.empty() && pluginId.empty())
    {
        clearPlugin(scene, track);
        return true;
    }
    const PluginEntry* match = nullptr;
    for (const auto& e : catalogue_)
        if ((! pluginId.empty() && e.id == pluginId) || (! path.empty() && e.path == path))
        {
            match = &e;
            break;
        }
    s.loaded = true;
    s.bypassed = false;
    s.id.set(match != nullptr ? std::string_view(match->id) : pluginId);
    s.path.set(match != nullptr ? std::string_view(match->path) : path);
    s.name.set(match != nullptr ? std::string_view(match->name) : (! pluginId.empty() ? pluginId : path));
    s.vendor.set(match != nullptr ? std::string_view(match->vendor) : std::string_view {});
    s.preset.set("INIT");
    s.page = 0;
    s.params.fill(0.5f);
    ++changes_;
    return true;
}

void LocalPluginAccess::clearPlugin(int scene, int track)
{
    auto& s = slots_[idx(scene, track)];
    if (! s.loaded)
        return;
    s = {};
    ++changes_;
}

bool LocalPluginAccess::bypass(int scene, int track) const noexcept
{
    return slots_[idx(scene, track)].bypassed;
}

void LocalPluginAccess::setBypass(int scene, int track, bool on)
{
    auto& s = slots_[idx(scene, track)];
    if (s.bypassed == on)
        return;
    s.bypassed = on;
    ++changes_;
}

std::vector<PluginPresetEntry> LocalPluginAccess::presets(int scene, int track) const
{
    if (! slots_[idx(scene, track)].loaded)
        return {};
    return {{"init", "INIT"}};
}

std::string_view LocalPluginAccess::currentPreset(int scene, int track) const noexcept
{
    const auto& s = slots_[idx(scene, track)];
    return s.loaded ? s.preset.view() : std::string_view {};
}

bool LocalPluginAccess::loadPreset(int scene, int track, std::string_view key)
{
    auto& s = slots_[idx(scene, track)];
    if (! s.loaded)
        return false;
    s.preset.set(key);
    ++changes_;
    return true;
}

bool LocalPluginAccess::readKnobs(int scene, int track, PluginKnobBank& out) const noexcept
{
    const auto& s = slots_[idx(scene, track)];
    out = {};
    if (! s.loaded)
    {
        out.tab.set("PLUGIN");
        return false;
    }
    out.tab.set(s.name.view());
    for (int i = 0; i < 6; ++i)
    {
        auto& k = out.columns[static_cast<std::size_t>(i)];
        k.active = true;
        k.paramId = static_cast<std::uint32_t>(i);
        k.norm = s.params[static_cast<std::size_t>(i)];
        k.label.format("PARAM %d", i + 1);
        k.valueText.format("%d%%", static_cast<int>(k.norm * 100.0f + 0.5f));
    }
    return true;
}

bool LocalPluginAccess::turnKnob(int scene, int track, int column0, int detents, bool fine) noexcept
{
    auto& s = slots_[idx(scene, track)];
    if (! s.loaded || column0 < 0 || column0 >= 6 || detents == 0)
        return false;
    const float step = fine ? 0.005f : 0.025f;
    auto& v = s.params[static_cast<std::size_t>(column0)];
    v = std::clamp(v + static_cast<float>(detents) * step, 0.0f, 1.0f);
    ++changes_;
    return true;
}

bool LocalPluginAccess::pushKnob(int scene, int track, int column0) noexcept
{
    auto& s = slots_[idx(scene, track)];
    if (! s.loaded || column0 < 0 || column0 >= 6)
        return false;
    s.params[static_cast<std::size_t>(column0)] = 0.5f;
    ++changes_;
    return true;
}

int LocalPluginAccess::pageCount(int scene, int track) const noexcept
{
    return slots_[idx(scene, track)].loaded ? 1 : 0;
}

int LocalPluginAccess::currentPage(int scene, int track) const noexcept
{
    return slots_[idx(scene, track)].page;
}

void LocalPluginAccess::setPage(int scene, int track, int page) noexcept
{
    auto& s = slots_[idx(scene, track)];
    s.page = std::max(0, page);
}

PluginAccess& PluginEnvironment::pluginAccess() const noexcept
{
    static LocalPluginAccess fallback;
    return access != nullptr ? *access : fallback;
}

PluginEnvironment& environment() noexcept
{
    static PluginEnvironment env;
    return env;
}
} // namespace ui::plugin
