#include "ui/screens/project/ProjectSource.h"

#include "services/store/ProjectStore.h"

#include <cctype>
#include <cstdio>

namespace ui::m2
{
namespace
{
// No store installed: nothing to list, every action explains why.
class UnavailableProjectSource final : public ProjectSource
{
public:
    std::vector<ProjectEntry> list(ProjectSort) override { return {}; }
    const std::string& currentName() const override { return name_; }
    bool dirty() const override { return false; }
    bool hasFile() const override { return false; }
    bool exists(std::string_view) override { return false; }
    void create(const std::string&, std::function<void(bool, std::string)> done) override { done(false, kWhy); }
    void open(const ProjectEntry&, std::function<void(float)>,
              std::function<void(bool, std::string, std::vector<std::string>)> done) override
    {
        done(false, kWhy, {});
    }
    void save(SaveTarget, const std::string&, std::function<void(float)>,
              std::function<void(bool, std::string)> done) override
    {
        done(false, kWhy);
    }
    bool remove(const ProjectEntry&, std::string& error) override { return fail(error); }
    bool rename(const ProjectEntry&, const std::string&, std::string& error) override { return fail(error); }
    bool duplicate(const ProjectEntry&, std::string& error) override { return fail(error); }

private:
    static constexpr const char* kWhy = "Project storage is not available";
    static bool fail(std::string& error)
    {
        error = kWhy;
        return false;
    }
    std::string name_ = services::store::kUntitledName;
};

ProjectSource*& installed() noexcept
{
    static ProjectSource* source = nullptr;
    return source;
}
} // namespace

std::optional<std::string_view> validateProjectName(std::string_view utf8)
{
    return services::store::validateProjectName(juce::String::fromUTF8(utf8.data(), static_cast<int>(utf8.size())));
}

std::string nextName(std::string_view name)
{
    std::size_t digits = 0;
    while (digits < name.size() && std::isdigit(static_cast<unsigned char>(name[name.size() - 1 - digits])) != 0)
        ++digits;
    if (digits == 0 || digits > 9)
        return std::string(name) + "_2";
    const auto stem = name.substr(0, name.size() - digits);
    const long value = std::stol(std::string(name.substr(name.size() - digits))) + 1; // ≤ 10^9: 10 digits
    std::string number = std::to_string(value);
    if (number.size() < digits)
        number.insert(0, digits - number.size(), '0'); // keep the zero padding ("_009" → "_010")
    return std::string(stem) + number;
}

std::string newProjectName(ProjectSource& source)
{
    for (int i = 1; i < 1000; ++i)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "Project_%03d", i);
        if (! source.exists(buf))
            return buf;
    }
    return "Project";
}

ProjectSource& ProjectSource::current()
{
    static UnavailableProjectSource unavailable;
    return installed() != nullptr ? *installed() : unavailable;
}

void ProjectSource::install(ProjectSource* s) noexcept
{
    installed() = s;
}
} // namespace ui::m2
