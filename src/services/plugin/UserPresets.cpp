#include "services/plugin/UserPresets.h"
#include "services/store/AtomicWriter.h"
#include "services/store/ProjectStore.h"
#include "services/store/SampleRefs.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace services::plugin
{
namespace
{
constexpr auto extension = ".preset";
juce::File existingFile(const juce::File& dir, const juce::String& filename)
{
    // Match FAT's case-insensitive names on the desktop as well.
    for (const auto& file : dir.findChildFiles(juce::File::findFiles, false, "p-*.preset"))
        if (file.getFileName().equalsIgnoreCase(filename))
            return file;
    return dir.getChildFile(filename);
}
} // namespace

juce::File UserPresets::folder(const std::string& pluginId) const
{
    store::Sha256 hash;
    hash.update(pluginId.data(), pluginId.size());
    return root_.getChildFile(store::Sha256::hex(hash.finish()));
}

std::string UserPresets::nameError(const std::string& name)
{
    if (name.find('\0') != name.npos ||
        ! juce::CharPointer_UTF8::isValidString(name.data(), static_cast<int>(name.size())))
        return "Invalid preset name";
    if (auto error = store::validateProjectName(juce::String::fromUTF8(name.c_str())))
        return std::string(*error);
    // Reserve room for the extension and the atomic writer's temporary suffix.
    if (name.size() > 220)
        return "Preset name is too long";
    return {};
}

std::vector<std::string> UserPresets::names(const std::string& pluginId) const
{
    auto files = folder(pluginId).findChildFiles(juce::File::findFiles, false, "p-*.preset");
    files.sort();
    std::vector<std::string> out;
    for (const auto& file : files)
        out.push_back(file.getFileNameWithoutExtension().substring(2).toStdString());
    return out;
}

core::Result<std::string> UserPresets::read(const std::string& pluginId, const std::string& name) const
{
    if (auto error = nameError(name); ! error.empty())
        return core::Error{error};
    auto file = existingFile(folder(pluginId), "p-" + juce::String(name) + extension);
    const auto data = juce::JSON::parse(file);
    if (! data.isObject() || ! data["version"].isInt() || static_cast<int>(data["version"]) != 1 ||
        ! data["pluginId"].isString() || data["pluginId"].toString() != juce::String(pluginId) ||
        ! data["state"].isString() || data["state"].toString().isEmpty())
        return core::Error{"Preset is missing, incompatible or damaged"};
    return data["state"].toString().toStdString();
}

core::Result<bool> UserPresets::save(const std::string& pluginId, const std::string& name, const std::string& state,
                                     bool replace) const
{
    if (auto error = nameError(name); ! error.empty())
        return core::Error{error};
    if (pluginId.empty() || state.empty())
        return core::Error{"Plugin did not provide a preset state"};
    const auto dir = folder(pluginId);
    if (dir.createDirectory().failed())
        return core::Error{"Cannot create preset folder on the data drive"};
    // Serialize the case-insensitive name check and rename without creating lock files outside the drive.
    const int fd = ::open(dir.getFullPathName().toRawUTF8(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return core::Error{"Cannot open preset folder on the data drive"};
    const juce::ScopeGuard closeDirectory{[fd] { ::close(fd); }};
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        return core::Error{"Cannot lock preset folder on the data drive"};
    const auto file = existingFile(dir, "p-" + juce::String(name) + extension);
    if (file.exists() && ! replace)
        return false;
    juce::var data(new juce::DynamicObject());
    auto& object = *data.getDynamicObject();
    object.setProperty("version", 1);
    object.setProperty("pluginId", juce::String(pluginId));
    object.setProperty("state", juce::String(state));
    const auto json = juce::JSON::toString(data);
    auto write = store::AtomicWriter::writeFileAtomically(file, json.toRawUTF8(), json.getNumBytesAsUTF8());
    if (! write)
        return write.error();
    return true;
}
} // namespace services::plugin
