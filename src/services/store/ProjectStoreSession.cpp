#include "services/store/ProjectStoreSession.h"

#include "core/commands/CommandBusAccess.h"
#include "core/model/ModelOps.h"

namespace services::store
{
namespace model = ::core::model;

namespace
{
// RESTORE (PRJ-020 "Restore unsaved changes?"): the whole model replaced by an autosave copy, as one undoable edit so
// the project is dirty (the user's file is untouched until they save) and UNDO returns to the saved version.
class ReplaceProject final : public core::Command
{
public:
    explicit ReplaceProject(std::shared_ptr<const model::Project> state) : state_(std::move(state)) {}
    std::string_view label() const override { return "RESTORE"; }
    void apply(model::Project& p) override { p = *state_; }
    std::unique_ptr<core::Command> inverse(const model::Project& before) const override
    {
        return std::make_unique<ReplaceProject>(std::make_shared<const model::Project>(before));
    }
    void collectChanges(core::ChangeSet& c) const override
    {
        c.add("settings");
        for (int s = 0; s < model::kScenes; ++s)
        {
            c.add("scene[" + std::to_string(s) + "]");
            for (int t = 0; t < model::kTracks; ++t)
                c.add("scene[" + std::to_string(s) + "].track[" + std::to_string(t) + "].playback.sample");
        }
        for (const char* p : {"arrangement", "ribbon", "midi", "sampleRefs"})
            c.add(p);
    }
    std::size_t sizeBytes() const override { return sizeof(*this) + sizeof(model::Project); }

private:
    std::shared_ptr<const model::Project> state_;
};
} // namespace

ProjectSession::ProjectSession(core::CommandBus& bus, services::library::Workers& workers, engine::EngineBridge* bridge,
                               services::library::SamplePool* pool, SessionConfig config)
    : bus_(bus), workers_(workers), bridge_(bridge), config_(std::move(config)),
      resume_(std::make_shared<Resume>(Resume::Config {systemDir(), systemDir().getChildFile("autosave")})),
      store_(bus, workers,
             StoreConfig {.projectsDir = projectsDir(),
                          .backupsDir = backupsDir(),
                          .trashDir = config_.useOsTrash ? juce::File() : systemDir().getChildFile("trash"),
                          .externalFolders = config_.externalFolders,
                          .foreignFolders = config_.foreignFolders,
                          .sampleRoots = config_.sampleRoots,
                          .selection = config_.selection,
                          .resume = resume_}),
      autosave_(bus, workers, Autosave::Config {.folder = systemDir().getChildFile("autosave")}),
      missing_(bus, workers, [this] { return store_.roots(); })
{
    store_.setEngine(bridge_);
    store_.setPool(pool);
    autosave_.setEnabled(config_.autosaveEnabled);
    store_.onIdentity = [this](const juce::String& name, const juce::File& file) { autosave_.setTarget(name, file); };
    store_.onSaved = [this](const juce::File&) { autosave_.onSaved(); };
    store_.onReplaced = [this](const model::Project& p, const LoadReport& r)
    {
        if (onProjectReplaced)
            onProjectReplaced(p, r);
    };
}

ProjectSession::~ProjectSession() { *alive_ = false; }

std::optional<juce::File> ProjectSession::resolveSample(const std::string& modelPath) const
{
    const auto file = resolveLocation(splitLocation(modelPath), store_.roots());
    if (! file || ! file->existsAsFile())
        return std::nullopt;
    return file;
}

void ProjectSession::startup(std::function<void(const StartupResult&)> done)
{
    // Interrupted writes of the last run (AtomicWriter: staging deleted, sealed rolled forward, backups restored).
    projectsDir().createDirectory();
    systemDir().createDirectory();
    AtomicWriter::recoverLeftovers(projectsDir());
    AtomicWriter::recoverLeftovers(systemDir());
    restoreWithoutAsking_ = powerOffMarker().existsAsFile(); // read once: a later crash asks again
    powerOffMarker().deleteFile();
    const auto decision = resume_->atStartup(config_.resumeEnabled);
    StartupResult result;
    result.message = decision.message;
    if (decision.kind == StartupDecision::Kind::NewProject)
        startNew(std::move(result), std::move(done));
    else
        startupOpen(decision, std::move(done));
}

void ProjectSession::startNew(StartupResult result, std::function<void(const StartupResult&)> done)
{
    store_.createUntitled();
    // An untitled project that was autosaved but never saved (the app stopped before it got a name).
    const auto untitled = Autosave::newerThan(systemDir().getChildFile("autosave"), kUntitledName, store_.file());
    offerAutosave(untitled, std::move(result), std::move(done));
}

void ProjectSession::startupOpen(const StartupDecision& decision, std::function<void(const StartupResult&)> done)
{
    (void) resume_->beginLoad(); // PRJ-021 guard
    std::weak_ptr<bool> alive = alive_;
    store_.openAs(decision.source, decision.identity, startupProgress_,
                  [this, alive, decision, done = std::move(done)](Result<void> r, LoadReport report) mutable
                  {
                      if (alive.expired())
                          return;
                      resume_->endLoad(r.ok());
                      StartupResult result;
                      result.message = decision.message;
                      if (! r)
                      {
                          resume_->setAside(); // a damaged target is not retried at every start
                          if (result.message.isNotEmpty())
                              result.message << "\n";
                          result.message << "The last project could not be opened: " << r.error().message
                                         << ". A new project was started.";
                          startNew(std::move(result), std::move(done));
                          return;
                      }
                      settlePending_ = true;
                      result.resumed = true;
                      result.selection = decision.selection;
                      result.report = std::move(report);
                      offerAutosave(decision.newerAutosave, std::move(result), std::move(done));
                  });
}

void ProjectSession::offerAutosave(const std::optional<juce::File>& autosave, StartupResult result,
                                   std::function<void(const StartupResult&)> done)
{
    if (autosave && restoreWithoutAsking_) // switched off with the power key: back where it was, no question
    {
        restoreAutosave(*autosave,
                        [result, done](Result<void> r) mutable
                        {
                            if (! r)
                                result.message << "The unsaved changes could not be restored: " << r.error().message;
                            done(result);
                        });
        return;
    }
    if (! autosave || ! askRestore)
    {
        done(result);
        return;
    }
    const auto file = *autosave;
    std::weak_ptr<bool> alive = alive_;
    askRestore(file,
               [this, alive, file, result = std::move(result), done = std::move(done)](bool restore) mutable
               {
                   if (alive.expired())
                       return;
                   if (! restore)
                   {
                       // Kept one generation aside (never deleted silently), so it is not offered again.
                       const auto aside = file.getSiblingFile(file.getFileNameWithoutExtension() + ".discarded.json");
                       aside.deleteFile();
                       file.moveFileTo(aside);
                       done(result);
                       return;
                   }
                   restoreAutosave(file,
                                   [result, done](Result<void> r) mutable
                                   {
                                       if (! r)
                                           result.message << "The unsaved changes could not be restored: "
                                                          << r.error().message;
                                       done(result);
                                   });
               });
}

void ProjectSession::restoreAutosave(const juce::File& file, std::function<void(Result<void>)> done)
{
    struct Job
    {
        std::shared_ptr<model::Project> project = std::make_shared<model::Project>();
        std::optional<Result<void>> result;
        LoadReport report;
    };
    auto job = std::make_shared<Job>();
    std::weak_ptr<bool> alive = alive_;
    workers_.run(
        [job, file, roots = store_.roots()]
        {
            ImportReport ir;
            auto r = Autosave::readInto(file, *job->project, ir);
            if (r)
                resolvePool(job->project->sampleRefs, roots);
            job->report.warnings = std::move(ir.warnings);
            job->report.readOnly = ir.readOnly;
            job->result = std::move(r);
        },
        [this, alive, job, done = std::move(done)]
        {
            if (alive.expired())
                return;
            if (! *job->result)
                return done(*job->result);
            for (const auto& e : job->project->sampleRefs.entries)
                if (e.missing)
                    job->report.missing.push_back(e);
            if (store_.admitReplacement)
                if (auto accepted = store_.admitReplacement(); ! accepted)
                    return done(std::move(accepted));
            bus_.execute(std::make_unique<ReplaceProject>(job->project));
            if (onProjectReplaced)
                onProjectReplaced(core::commands::project(bus_), job->report);
            done({});
        });
}

void ProjectSession::frame(double nowMs)
{
    lastNowMs_ = nowMs;
    if (settlePending_)
    {
        settlePending_ = false;
        settleAtMs_ = nowMs + Resume::kSettleMs;
    }
    if (settleAtMs_ && nowMs >= *settleAtMs_)
    {
        settleAtMs_.reset();
        workers_.run([resume = resume_] { resume->settle(); });
    }
    const bool playing = deviceState_ == DeviceState::Running && bridge_ != nullptr && bridge_->playhead().playing;
    autosave_.tick(nowMs, playing);
}

void ProjectSession::shutdown(bool powerOff)
{
    if (powerOff)
        autosave_.onDeviceLost(); // the power goes next: unsaved work on disk, as for a lost device (NFR-012)
    workers_.drain(); // a save or autosave in flight completes; nothing is dropped
    if (powerOff)
        powerOffMarker().create(); // the next start-up restores the autosave without asking
    resume_->settle();
    if (config_.selection)
        resume_->updateSelection(config_.selection());
}

void ProjectSession::onAudioDeviceLost()
{
    if (deviceState_ == DeviceState::Lost)
        return;
    deviceState_ = DeviceState::Lost;
    if (bridge_ != nullptr)
    {
        const auto& ph = bridge_->playhead();
        wasPlaying_ = ph.playing;
        lostPpq_ = ph.ppq;
        bridge_->post(engine::TransportCmd {engine::TransportCmd::Stop, 0.0}); // pause, keep the position
    }
    autosave_.onDeviceLost(); // NFR-012: the unsaved work is on disk before anything else can fail
    if (onDeviceState)
        onDeviceState(deviceState_);
}

void ProjectSession::onAudioDeviceRestored()
{
    if (deviceState_ == DeviceState::Running)
        return;
    deviceState_ = DeviceState::Running;
    if (bridge_ != nullptr && wasPlaying_ && config_.resumePlaybackOnReconnect)
    {
        bridge_->post(engine::TransportCmd {engine::TransportCmd::Locate, lostPpq_});
        bridge_->post(engine::TransportCmd {engine::TransportCmd::Continue, 0.0});
    }
    wasPlaying_ = false;
    if (onDeviceState)
        onDeviceState(deviceState_);
}
} // namespace services::store
