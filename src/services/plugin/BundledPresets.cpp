// Presets stored inside a CLAP binary. The browser calls this on the message thread.
#include "services/plugin/ClapHost.h"

#include <clap/clap.h>

#include <algorithm>

namespace services::plugin
{
namespace
{
using Indexer = clap_preset_discovery_indexer_t;
using Receiver = clap_preset_discovery_metadata_receiver_t;

struct PresetMetadata
{
    BundledPreset preset;
    bool hasPluginIds = false;
    bool matchesPlugin = false;
};

struct ProviderMetadata
{
    std::string_view pluginId;
    std::vector<PresetMetadata> presets;
    bool failed = false;
};

Receiver receiveMetadata(ProviderMetadata& data)
{
    Receiver r{};
    r.receiver_data = &data;
    r.on_error = [](const Receiver* self, int32_t, const char*)
    { static_cast<ProviderMetadata*>(self->receiver_data)->failed = true; };
    r.begin_preset = [](const Receiver* self, const char* name, const char* key)
    {
        auto& presets = static_cast<ProviderMetadata*>(self->receiver_data)->presets;
        presets.push_back({{key ? key : "", name ? name : ""}});
        return true;
    };
    r.add_plugin_id = [](const Receiver* self, const clap_universal_plugin_id_t* id)
    {
        auto& metadata = *static_cast<ProviderMetadata*>(self->receiver_data);
        if (metadata.presets.empty() || id == nullptr)
            return;
        auto& p = metadata.presets.back();
        p.hasPluginIds = true;
        if (id->abi && std::string_view(id->abi) == "clap" && id->id && id->id == metadata.pluginId)
            p.matchesPlugin = true;
    };
    r.set_soundpack_id = r.add_creator = r.set_description = r.add_feature = [](const Receiver*, const char*) {};
    r.set_flags = [](const Receiver*, uint32_t) {};
    r.set_timestamps = [](const Receiver*, clap_timestamp, clap_timestamp) {};
    r.add_extra_info = [](const Receiver*, const char*, const char*) {};
    return r;
}
} // namespace

const std::vector<BundledPreset>& ClapAudioNode::bundledPresets()
{
    if (presets_)
        return *presets_;
    auto& result = presets_.emplace();
    if (! presetLoad_ || ! presetLoad_->from_location)
        return result;

    const auto* factory =
        static_cast<const clap_preset_discovery_factory_t*>(entry_->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (! factory)
        factory = static_cast<const clap_preset_discovery_factory_t*>(
            entry_->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
    if (! factory || ! factory->count || ! factory->create || ! factory->get_descriptor)
        return result;
    const auto* plugins = static_cast<const clap_plugin_factory_t*>(entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const bool singlePlugin = plugins->get_plugin_count(plugins) == 1;

    Indexer indexer{};
    indexer.clap_version = CLAP_VERSION;
    indexer.name = hostStorage_->name;
    indexer.vendor = hostStorage_->vendor;
    indexer.version = hostStorage_->version;
    indexer.declare_filetype = [](const Indexer*, const clap_preset_discovery_filetype_t*) { return true; };
    indexer.declare_soundpack = [](const Indexer*, const clap_preset_discovery_soundpack_t*) { return true; };
    indexer.get_extension = [](const Indexer*, const char*) -> const void* { return nullptr; };
    indexer.declare_location = [](const Indexer* self, const clap_preset_discovery_location_t* location)
    {
        if (location && location->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
            *static_cast<bool*>(self->indexer_data) = true;
        return location != nullptr;
    };

    const auto count = factory->count(factory);
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* desc = factory->get_descriptor(factory, i);
        if (! desc || ! desc->id || ! clap_version_is_compatible(desc->clap_version))
            continue;
        bool bundled = false;
        indexer.indexer_data = &bundled;
        const auto* provider = factory->create(factory, &indexer, desc->id);
        if (! provider)
            continue;
        ProviderMetadata data{id_, {}};
        auto receiver = receiveMetadata(data);
        if (provider->init(provider) && bundled &&
            provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver) &&
            ! data.failed)
        {
            for (auto& p : data.presets)
                if (! p.preset.key.empty() && (p.hasPluginIds ? p.matchesPlugin : singlePlugin) &&
                    std::none_of(result.begin(), result.end(),
                                 [&](const auto& existing) { return existing.key == p.preset.key; }))
                    result.push_back(std::move(p.preset));
        }
        provider->destroy(provider);
    }
    return result;
}

bool ClapAudioNode::loadBundledPreset(const std::string& key)
{
    if (key.empty() || ! presetLoad_ || ! presetLoad_->from_location ||
        ! presetLoad_->from_location(plugin_, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str()))
        return false;
    refreshParamMeta();
    screenInitialized_ = false;
    wakeAudio_.store(true, std::memory_order_release);
    return true;
}
} // namespace services::plugin
