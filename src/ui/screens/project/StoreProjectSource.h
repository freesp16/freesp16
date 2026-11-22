// The PROJECT screens' ProjectSource over services::store::ProjectStore (PRJ-001…061). The composition
// root installs it next to its ProjectSession:
//     ui::m2::installProjectSession(session);            // ProjectSource::install(adapter over session.store())
//     ui::m2::Session::instance().install(&bus);          // the same CommandBus the session's store edits
// and keeps calling workers.pump() per frame (the store's completions run there). The store reports progress through
// a polled services::library::Progress; the screens call ProjectSource::poll() once per frame while a job runs, which
// forwards the fraction to the Progress popup. Message thread only.
#pragma once

#include "ui/screens/project/ProjectSource.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace services::store
{
class ProjectStore;
class ProjectSession;
} // namespace services::store

namespace ui::m2
{
class StoreProjectSource final : public ProjectSource
{
public:
    // `externalDir`: the folder SAVE TO USB writes to (PRJ-060), invalid = none mounted.
    explicit StoreProjectSource(services::store::ProjectStore&, std::function<juce::File()> externalDir = {});
    ~StoreProjectSource() override;

    std::vector<ProjectEntry> list(ProjectSort) override;
    const std::string& currentName() const override;
    bool dirty() const override;
    bool hasFile() const override;
    bool exists(std::string_view name) override;
    void create(const std::string& name, std::function<void(bool, std::string)> done) override;
    void open(const ProjectEntry&, std::function<void(float)> progress,
              std::function<void(bool, std::string, std::vector<std::string>)> done) override;
    void save(SaveTarget, const std::string& name, std::function<void(float)> progress,
              std::function<void(bool, std::string)> done) override;
    bool remove(const ProjectEntry&, std::string& error) override;
    bool rename(const ProjectEntry&, const std::string& name, std::string& error) override;
    bool duplicate(const ProjectEntry&, std::string& error) override;
    void cancel() override;
    void poll() override;

    // Display text of a modification time ("Today 14:12", "Yesterday 22:40", "18 Nov 2026").
    static std::string modifiedText(juce::Time when, juce::Time now);

private:
    struct Job;
    services::store::ProjectStore& store_;
    std::function<juce::File()> externalDir_;
    std::shared_ptr<Job> job_; // the running open / save (its Progress and the UI's callback)
    mutable juce::String seenName_;
    mutable std::string name_;
};

// Installs a StoreProjectSource over `session.store()` (kept for the session's lifetime) as ProjectSource::current().
void installProjectSession(services::store::ProjectSession& session, std::function<juce::File()> externalDir = {});
} // namespace ui::m2
