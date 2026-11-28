#pragma once

#include "core/util/Result.h"
#include <juce_core/juce_core.h>
#include <string>
#include <vector>

namespace services::plugin
{
// One library on the data drive. Descriptor IDs identify sounds across projects, slots and binary upgrades.
class UserPresets
{
public:
    explicit UserPresets(juce::File root) : root_(std::move(root)) {}
    std::vector<std::string> names(const std::string& pluginId) const;
    core::Result<std::string> read(const std::string& pluginId, const std::string& name) const;
    // false asks the caller to confirm replacement of an existing name.
    core::Result<bool> save(const std::string& pluginId, const std::string& name, const std::string& state,
                            bool replace) const;
    static std::string nameError(const std::string& name);

private:
    juce::File folder(const std::string& pluginId) const;
    juce::File root_;
};
} // namespace services::plugin
