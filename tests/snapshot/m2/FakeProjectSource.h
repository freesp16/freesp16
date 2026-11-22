// In-memory ProjectSource for the sequencing-screen tests and the bench (the former LocalProjectSource): projects are
// snapshots of the m2::Session's model kept in memory, nothing touches the disk; completions run at once. The product
// uses StoreProjectSource over services::store::ProjectStore.
#pragma once

#include "core/commands/CommandBusAccess.h"
#include "core/model/ModelOps.h"
#include "ui/screens/project/ProjectSource.h"
#include "ui/screens/sequence/M2Session.h"

#include <algorithm>
#include <ctime>
#include <memory>

namespace m2test
{
using ui::m2::ProjectEntry;
using ui::m2::ProjectSort;
using ui::m2::ProjectSource;
using ui::m2::SaveTarget;
using ui::m2::validateProjectName;

class FakeProjectSource final : public ProjectSource
{
public:
    static FakeProjectSource& instance()
    {
        static FakeProjectSource source;
        return source;
    }
    void reset(); // tests: no projects, "Untitled", unsaved
    // Tests / demo: an entry that exists only in the list (no model snapshot: opening it gives a default project).
    void addEntry(ProjectEntry);
    void setCurrent(std::string name, bool hasFile);

    std::vector<ProjectEntry> list(ProjectSort) override;
    const std::string& currentName() const override { return current_; }
    bool dirty() const override;
    bool hasFile() const override { return hasFile_; }
    bool exists(std::string_view name) override;
    void create(const std::string& name, std::function<void(bool, std::string)> done) override;
    void open(const ProjectEntry&, std::function<void(float)> progress,
              std::function<void(bool, std::string, std::vector<std::string>)> done) override;
    void save(SaveTarget, const std::string& name, std::function<void(float)> progress,
              std::function<void(bool, std::string)> done) override;
    bool remove(const ProjectEntry&, std::string& error) override;
    bool rename(const ProjectEntry&, const std::string& name, std::string& error) override;
    bool duplicate(const ProjectEntry&, std::string& error) override;

private:
    struct Stored
    {
        ProjectEntry entry;
        std::shared_ptr<const core::model::Project> model; // null = list-only entry
    };
    Stored* find(std::string_view name);
    std::vector<Stored> stored_;
    std::string current_ = "Untitled";
    bool hasFile_ = false;
    std::int64_t clock_ = 0;
};

inline void FakeProjectSource::reset()
{
    stored_.clear();
    current_ = "Untitled";
    hasFile_ = false;
    clock_ = 0;
}

inline void FakeProjectSource::addEntry(ProjectEntry e)
{
    stored_.push_back({std::move(e), nullptr});
}

inline void FakeProjectSource::setCurrent(std::string name, bool hasFile)
{
    current_ = std::move(name);
    hasFile_ = hasFile;
}

inline FakeProjectSource::Stored* FakeProjectSource::find(std::string_view name)
{
    for (auto& s : stored_)
        if (s.entry.name == name)
            return &s;
    return nullptr;
}

inline std::vector<ProjectEntry> FakeProjectSource::list(ProjectSort sort)
{
    std::vector<ProjectEntry> out;
    out.reserve(stored_.size());
    for (const auto& s : stored_)
        out.push_back(s.entry);
    if (sort == ProjectSort::Name)
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    else
        std::stable_sort(out.begin(), out.end(),
                         [](const auto& a, const auto& b) { return a.modifiedKey > b.modifiedKey; });
    return out;
}

inline bool FakeProjectSource::dirty() const
{
    return ui::m2::Session::instance().bus().dirty();
}

inline bool FakeProjectSource::exists(std::string_view name)
{
    return find(name) != nullptr;
}

inline void FakeProjectSource::create(const std::string& name, std::function<void(bool, std::string)> done)
{
    if (const auto why = validateProjectName(name))
    {
        done(false, std::string(*why));
        return;
    }
    if (exists(name))
    {
        done(false, "A project with this name exists");
        return;
    }
    auto& session = ui::m2::Session::instance();
    core::commands::replaceProject(session.bus(), core::model::newDefaultProject());
    session.touch();
    current_ = name;
    hasFile_ = false;
    done(true, {});
}

inline void FakeProjectSource::open(const ProjectEntry& e, std::function<void(float)> progress,
                                    std::function<void(bool, std::string, std::vector<std::string>)> done)
{
    auto* s = find(e.name);
    if (s == nullptr)
    {
        done(false, "Project not found: " + e.name, {});
        return;
    }
    if (progress)
        progress(0.5f);
    auto& session = ui::m2::Session::instance();
    core::commands::replaceProject(session.bus(), s->model ? std::make_unique<core::model::Project>(*s->model)
                                                           : core::model::newDefaultProject());
    session.touch();
    current_ = e.name;
    hasFile_ = true;
    if (progress)
        progress(1.0f);
    done(true, {}, {});
}

inline void FakeProjectSource::save(SaveTarget target, const std::string& name, std::function<void(float)> progress,
                                    std::function<void(bool, std::string)> done)
{
    const std::string n = target == SaveTarget::Save && hasFile_ ? current_ : name;
    if (const auto why = validateProjectName(n))
    {
        done(false, std::string(*why));
        return;
    }
    auto& session = ui::m2::Session::instance();
    if (progress)
        progress(0.5f);
    auto* s = find(n);
    if (s == nullptr)
    {
        ProjectEntry e;
        e.name = n;
        stored_.push_back({std::move(e), nullptr});
        s = &stored_.back();
    }
    s->model = std::make_shared<const core::model::Project>(session.project());
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char when[32];
    std::strftime(when, sizeof when, "Today %H:%M", &local);
    s->entry.modified = when;
    s->entry.modifiedKey = static_cast<std::int64_t>(now) * 1000 + ++clock_;
    int scenes = 0;
    for (int i = 0; i < 16; ++i)
        scenes += session.sceneHasContent(i) ? 1 : 0;
    s->entry.scenes = scenes;
    s->entry.external = target == SaveTarget::SaveTo;
    s->entry.location = target == SaveTarget::SaveTo ? "USB" : "";
    current_ = n;
    hasFile_ = true;
    session.bus().checkpoint();
    session.touch();
    if (progress)
        progress(1.0f);
    done(true, {});
}

inline bool FakeProjectSource::remove(const ProjectEntry& e, std::string& error)
{
    if (e.name == current_ && hasFile_)
    {
        error = "The open project cannot be deleted";
        return false;
    }
    const auto it = std::find_if(stored_.begin(), stored_.end(), [&](const auto& s) { return s.entry.name == e.name; });
    if (it == stored_.end())
    {
        error = "Project not found: " + e.name;
        return false;
    }
    stored_.erase(it);
    return true;
}

inline bool FakeProjectSource::rename(const ProjectEntry& e, const std::string& name, std::string& error)
{
    if (const auto why = validateProjectName(name))
    {
        error = std::string(*why);
        return false;
    }
    if (exists(name))
    {
        error = "A project with this name exists";
        return false;
    }
    auto* s = find(e.name);
    if (s == nullptr)
    {
        error = "Project not found: " + e.name;
        return false;
    }
    if (current_ == e.name)
        current_ = name;
    s->entry.name = name;
    return true;
}

inline bool FakeProjectSource::duplicate(const ProjectEntry& e, std::string& error)
{
    auto* s = find(e.name);
    if (s == nullptr)
    {
        error = "Project not found: " + e.name;
        return false;
    }
    std::string name = e.name + " COPY";
    for (int i = 2; exists(name); ++i)
        name = e.name + " COPY " + std::to_string(i);
    Stored copy = *s;
    copy.entry.name = name;
    copy.entry.modifiedKey = s->entry.modifiedKey + 1;
    stored_.push_back(std::move(copy));
    return true;
}
} // namespace m2test
