#pragma once

#include "core/model/Extras.h"

#include <string>

namespace core::model
{
struct PluginTrack // A hosted CLAP instance on an instrument track or in an FX slot
{
    std::string path{};        // relative to <data>/Plugins/ (e.g. "Anvil.clap"), or absolute
    std::string pluginId{};    // CLAP plugin descriptor id (e.g. "com.hdslave.anvil")
    std::string name{};        // cached display name (e.g. "Anvil")
    std::string presetKey{};   // last selected preset key / name
    std::string stateBase64{}; // base64 CLAP state, or tagged parameter values for plugins without native state
    bool bypass = false;
    Extras extras{};
    friend bool operator==(const PluginTrack&, const PluginTrack&) = default;
};
} // namespace core::model
