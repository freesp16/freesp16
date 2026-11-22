#include "services/store/Migrations.h"

namespace services::store
{
MigrationRunner::MigrationRunner(std::span<const Migration> migrations, int currentVersion)
    : migrations_(migrations), current_(currentVersion)
{
}

const MigrationRunner& MigrationRunner::builtin()
{
    // Registration is complete before main(); the list is read-only afterwards (core/util/Registrar.h).
    static const MigrationRunner runner(registeredMigrations(), kSchemaVersion);
    return runner;
}

const Migration* MigrationRunner::step(int from) const
{
    for (const auto& m : migrations_)
        if (m.from == from && m.to <= current_)
            return &m;
    return nullptr;
}

std::vector<std::string> MigrationRunner::validate() const
{
    std::vector<std::string> problems;
    for (const auto& m : migrations_)
    {
        if (m.to > current_)
            continue;
        if (m.to <= m.from || m.from < 1 || m.apply == nullptr)
            problems.push_back("migration " + std::to_string(m.from) + " -> " + std::to_string(m.to) + " (" +
                               std::string(m.summary) + ") is malformed");
        int count = 0;
        for (const auto& other : migrations_)
            count += other.from == m.from && other.to <= current_ ? 1 : 0;
        if (count > 1 && step(m.from) == &m)
            problems.push_back("more than one migration starts from version " + std::to_string(m.from));
    }
    for (int v = 1; v < current_;)
    {
        const auto* m = step(v);
        if (m == nullptr)
        {
            problems.push_back("no migration from version " + std::to_string(v));
            break;
        }
        if (m->to <= v)
            break; // reported above
        v = m->to;
    }
    return problems;
}

Result<int> MigrationRunner::run(juce::var& root, int fromVersion, std::vector<std::string>* applied) const
{
    if (fromVersion < 1)
        return Result<int>::failure("The project's format version is missing or invalid.",
                                    "schema version " + std::to_string(fromVersion));
    int v = fromVersion;
    while (v < current_)
    {
        const auto* m = step(v);
        if (m == nullptr || m->apply == nullptr || m->to <= v)
            return Result<int>::failure("This project was saved in a format version this app cannot convert (" +
                                            std::to_string(fromVersion) + ").",
                                        "no migration from schema version " + std::to_string(v));
        m->apply(root);
        v = m->to;
        setSchemaVersion(root, v);
        if (applied != nullptr)
            applied->emplace_back(m->summary);
    }
    return v;
}

int schemaVersionOf(const juce::var& root)
{
    const auto& version = root["meta"]["format"]["version"];
    if (version.isInt() || version.isInt64() || version.isDouble())
        return static_cast<int>(version);
    return 0;
}

void setSchemaVersion(juce::var& root, int version)
{
    auto* rootObject = root.getDynamicObject();
    if (rootObject == nullptr)
        return;
    auto meta = rootObject->getProperty("meta");
    if (! meta.isObject())
    {
        meta = juce::var(new juce::DynamicObject());
        rootObject->setProperty("meta", meta);
    }
    auto format = meta["format"];
    if (! format.isObject())
    {
        format = juce::var(new juce::DynamicObject());
        meta.getDynamicObject()->setProperty("format", format);
    }
    format.getDynamicObject()->setProperty("version", version);
}
} // namespace services::store
