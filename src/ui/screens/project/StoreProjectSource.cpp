#include "ui/screens/project/StoreProjectSource.h"

#include "services/store/ProjectStoreSession.h"
#include "ui/screens/sequence/M2Session.h"

#include <optional>

namespace ui::m2
{
namespace st = services::store;

struct StoreProjectSource::Job
{
    st::Progress progress;                 // polled by poll()
    std::function<void(float)> onProgress; // the UI's Progress popup
    float shown = -1;
};

namespace
{
juce::String toJuce(std::string_view s)
{
    return juce::String::fromUTF8(s.data(), static_cast<int>(s.size()));
}

// The native project of that name (CREATE / SAVE AS collisions; foreign entries are not projects of this app).
std::optional<st::ProjectInfo> find(st::ProjectStore& store, std::string_view name)
{
    const auto n = toJuce(name);
    for (auto& info : store.list(st::SortBy::Name))
        if (info.name == n && ! info.foreign)
            return info;
    return std::nullopt;
}

// The listed entry itself: by its path (the same name may exist in two locations), by name for entries without one.
std::optional<st::ProjectInfo> find(st::ProjectStore& store, const ProjectEntry& entry)
{
    if (entry.path.empty())
        return find(store, entry.name);
    const auto p = toJuce(entry.path);
    for (auto& info : store.list(st::SortBy::Name))
        if (info.path.getFullPathName() == p)
            return info;
    return std::nullopt;
}

core::Result<void> readOnly(const st::ProjectInfo& info)
{
    return core::Result<void>::failure(info.name.toStdString() + " is on " + info.location.toStdString()
                                       + " (read-only): OPEN imports it as a new project");
}

std::string why(const core::Result<void>& r)
{
    return r.ok() ? std::string() : r.error().message;
}
} // namespace

StoreProjectSource::StoreProjectSource(st::ProjectStore& store, std::function<juce::File()> externalDir)
    : store_(store), externalDir_(std::move(externalDir))
{
}

StoreProjectSource::~StoreProjectSource() = default;

std::string StoreProjectSource::modifiedText(juce::Time when, juce::Time now)
{
    const auto day = [](juce::Time t) { return t.getYear() * 400 + t.getDayOfYear(); };
    if (day(when) == day(now))
        return when.formatted("Today %H:%M").toStdString();
    if (day(when) == day(now - juce::RelativeTime::days(1)))
        return when.formatted("Yesterday %H:%M").toStdString();
    return when.formatted("%d %b %Y").toStdString();
}

std::vector<ProjectEntry> StoreProjectSource::list(ProjectSort sort)
{
    const auto now = juce::Time::getCurrentTime();
    std::vector<ProjectEntry> out;
    for (const auto& info : store_.list(sort == ProjectSort::Name ? st::SortBy::Name : st::SortBy::Modified))
    {
        ProjectEntry e;
        e.name = info.name.toStdString();
        e.modified = modifiedText(info.modified, now);
        e.modifiedKey = info.modified.toMilliseconds();
        e.scenes = info.scenesWithContent;
        e.external = info.external;
        e.location = info.foreign ? info.location.toStdString() : info.external ? "USB" : "";
        e.path = info.path.getFullPathName().toStdString();
        e.foreign = info.foreign;
        out.push_back(std::move(e));
    }
    return out;
}

const std::string& StoreProjectSource::currentName() const
{
    if (store_.name() != seenName_) // read every frame by the title band: converts only on a change
    {
        seenName_ = store_.name();
        name_ = seenName_.toStdString();
    }
    return name_;
}

std::string StoreProjectSource::replacementError() const
{
    if (store_.admitReplacement)
        if (auto accepted = store_.admitReplacement(); ! accepted)
            return accepted.error().message;
    return {};
}

bool StoreProjectSource::dirty() const
{
    return store_.dirty();
}

bool StoreProjectSource::hasFile() const
{
    return store_.hasFile();
}

bool StoreProjectSource::exists(std::string_view name)
{
    return find(store_, name).has_value();
}

void StoreProjectSource::create(const std::string& name, std::function<void(bool, std::string)> done)
{
    const auto r = store_.create(toJuce(name));
    Session::instance().touch();
    done(r.ok(), why(r));
}

void StoreProjectSource::open(const ProjectEntry& entry, std::function<void(float)> progress,
                              std::function<void(bool, std::string, std::vector<std::string>)> done)
{
    const auto info = find(store_, entry);
    if (! info)
    {
        done(false, "Project not found: " + entry.name, {});
        return;
    }
    auto job = std::make_shared<Job>();
    job->onProgress = std::move(progress);
    job_ = job;
    auto finished = [this, job, done = std::move(done)](core::Result<void> r, st::LoadReport report)
                {
                    if (job_ == job)
                        job_.reset();
                    Session::instance().touch();
                    std::vector<std::string> missing;
                    for (const auto& ref : report.missing)
                        missing.push_back(ref.displayName.empty() ? ref.path : ref.displayName);
                    if (report.cancelled)
                        done(false, "Loading cancelled", {});
                    else
                        done(r.ok(), why(r), std::move(missing));
                };
    if (info->foreign) // a reference project: imported as a new native project (its file is never written)
        store_.importForeign(*info, job->progress, std::move(finished));
    else
        store_.open(info->path, job->progress, std::move(finished));
}

void StoreProjectSource::save(SaveTarget target, const std::string& name, std::function<void(float)> progress,
                              std::function<void(bool, std::string)> done)
{
    juce::File dir;
    if (target == SaveTarget::SaveTo)
    {
        dir = externalDir_ ? externalDir_() : juce::File();
        if (dir == juce::File())
        {
            done(false, "No external location is available");
            return;
        }
    }
    auto job = std::make_shared<Job>();
    job->onProgress = std::move(progress);
    job_ = job;
    const auto kind = target == SaveTarget::Save     ? st::SaveKind::Save
                      : target == SaveTarget::SaveAs ? st::SaveKind::SaveAs
                                                     : st::SaveKind::SaveTo;
    store_.save(kind, toJuce(name), dir, job->progress,
                [this, job, done = std::move(done)](core::Result<void> r)
                {
                    if (job_ == job)
                        job_.reset();
                    Session::instance().touch();
                    done(r.ok(), why(r));
                });
}

bool StoreProjectSource::remove(const ProjectEntry& entry, std::string& error)
{
    const auto info = find(store_, entry);
    const auto r = ! info         ? core::Result<void>::failure("Project not found: " + entry.name)
                   : info->foreign ? readOnly(*info)
                                   : store_.remove(*info);
    error = why(r);
    return r.ok();
}

bool StoreProjectSource::rename(const ProjectEntry& entry, const std::string& name, std::string& error)
{
    const auto info = find(store_, entry);
    const auto r = ! info         ? core::Result<void>::failure("Project not found: " + entry.name)
                   : info->foreign ? readOnly(*info)
                                   : store_.rename(*info, toJuce(name));
    error = why(r);
    return r.ok();
}

bool StoreProjectSource::duplicate(const ProjectEntry& entry, std::string& error)
{
    const auto info = find(store_, entry);
    if (! info)
    {
        error = "Project not found: " + entry.name;
        return false;
    }
    if (info->foreign)
    {
        error = why(readOnly(*info));
        return false;
    }
    const auto r = store_.duplicate(*info);
    error = r.ok() ? std::string() : r.error().message;
    return r.ok();
}

void StoreProjectSource::cancel()
{
    if (job_)
        job_->progress.cancel = true;
}

void StoreProjectSource::poll()
{
    if (! job_ || ! job_->onProgress)
        return;
    const float f = job_->progress.fraction.load();
    if (f != job_->shown)
    {
        job_->shown = f;
        job_->onProgress(f);
    }
}

void installProjectSession(st::ProjectSession& session, std::function<juce::File()> externalDir)
{
    static std::unique_ptr<StoreProjectSource> adapter;
    ProjectSource::install(nullptr);
    adapter = std::make_unique<StoreProjectSource>(session.store(), std::move(externalDir));
    ProjectSource::install(adapter.get());
}
} // namespace ui::m2
