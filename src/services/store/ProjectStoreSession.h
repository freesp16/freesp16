// ProjectSession: the project services as the composition root (src/app/App.cpp) wires them — ProjectStore, Resume,
// Autosave and MissingMedia over one CommandBus, plus start-up, the per-frame tick, clean shutdown and the data-safety
// side of audio-device loss (NFR-012: detection is io's; this keeps the data safe and the transport state).
//
// Wiring (message thread; paths from platform::Paths, which services never include):
//     ProjectSession session(bus, workers, &engine->bridge(), &pool,
//                            {.dataDir = juce::File(paths.dataDir), .sampleRoots = [&] { return library.roots(); },
//                             .externalFolders = …, .selection = [&] { return ui.selection(); }});
//     SamplePool pool(workers, [&](const std::string& p) { return session.resolveSample(p); });  // $Bundle aware
//     session.onProjectReplaced = [&](const Project& p, const LoadReport&) { sync.syncAll(p); … };
//     session.startup([&](const StartupResult& r) { … restore r.selection, show r.message … });
//     per frame:            session.frame(nowMs);     (after workers.pump())
//     audio device lost:    session.onAudioDeviceLost();   … back: session.onAudioDeviceRestored();
//     quit:                 session.shutdown();      power key: session.shutdown(true);
// Directory layout under dataDir (LIB-001's root layout): Projects/, Backups/, System/ (resume.json,
// resume.sixteen, resuming/, resumed, crashed/, autosave/, trash/ on the device).
#pragma once

#include "core/commands/Command.h"
#include "engine/EngineApi.h"
#include "services/library/SamplePool.h"
#include "services/library/Workers.h"
#include "services/store/Autosave.h"
#include "services/store/MissingMedia.h"
#include "services/store/ProjectStore.h"
#include "services/store/Resume.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace services::store
{
struct SessionConfig
{
    juce::File dataDir;                                         // platform::Paths::dataDir
    std::function<SampleRoots()> sampleRoots;                   // SampleLibrary::roots()
    std::function<std::vector<juce::File>()> externalFolders;   // mounted volumes listed in OPEN (PRJ-005/060)
    std::function<std::vector<ForeignFolder>()> foreignFolders; // reference projects listed in OPEN (read-only)
    std::function<Selection()> selection;                       // the UI's scene/pattern/track/screen (PRJ-023)
    bool useOsTrash = true;    // host: delete → OS trash; device (false): <data>/System/trash
    bool resumeEnabled = true; // preference project.resume (PRJ-022)
    bool autosaveEnabled = true; // preference project.autosave
    bool resumePlaybackOnReconnect = true; // NFR-012: continue where it stopped when the device returns
};

struct StartupResult
{
    bool resumed = false;      // the last project was reopened
    Selection selection {};    // restore it (PRJ-023)
    juce::String message;      // show when not empty (crash guard, fallback, load error)
    LoadReport report {};      // missing samples etc. of the resumed project
};

enum class DeviceState : std::uint8_t { Running, Lost };

class ProjectSession
{
public:
    ProjectSession(core::CommandBus&, services::library::Workers&, engine::EngineBridge*,
                   services::library::SamplePool*, SessionConfig);
    ~ProjectSession();
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator= (const ProjectSession&) = delete;

    // Recovers interrupted writes, decides (Resume::atStartup), opens the last project or creates a new one; `done`
    // runs on the message thread once the bus holds a project. A newer autosave of the resumed project (or of an
    // untitled project that was never saved) is offered through askRestore before `done`.
    void startup(std::function<void(const StartupResult&)> done);
    void frame(double nowMs); // autosave tick (playing from the engine's playhead), resume settle
    void shutdown(bool powerOff = false); // clean exit: ends the early-exit watch, keeps the selection; waits for no
                                          // worker. powerOff (the power key): unsaved changes are autosaved first

    // NFR-012. Lost: remember the transport, pause it (position kept), autosave now when dirty, report the state.
    // Restored: locate back and continue when it was playing (resumePlaybackOnReconnect).
    void onAudioDeviceLost();
    void onAudioDeviceRestored();
    DeviceState deviceState() const noexcept { return deviceState_; }

    void setResumeEnabled(bool on) noexcept { config_.resumeEnabled = on; }
    void setAutosaveEnabled(bool on) noexcept { autosave_.setEnabled(on); }
    void setAutosaveIdleInterval(double ms) noexcept { autosave_.setIdleInterval(ms); } // UTILITY › AUTOSAVE

    // Restores an autosave copy as an undoable edit of the open project ("RESTORE"; the project becomes dirty).
    void restoreAutosave(const juce::File& autosave, std::function<void(Result<void>)> done);

    // The SamplePool resolver: "$Bundle/…" against the open project, the rest against sampleRoots().
    std::optional<juce::File> resolveSample(const std::string& modelPath) const;

    ProjectStore& store() noexcept { return store_; }
    Autosave& autosave() noexcept { return autosave_; }
    Resume& resume() noexcept { return *resume_; }
    MissingMedia& missingMedia() noexcept { return missing_; }
    juce::File projectsDir() const { return config_.dataDir.getChildFile("Projects"); }
    juce::File systemDir() const { return config_.dataDir.getChildFile("System"); }
    juce::File backupsDir() const { return config_.dataDir.getChildFile("Backups"); }

    // Composition-root hooks (message thread).
    std::function<void(const core::model::Project&, const LoadReport&)> onProjectReplaced;
    // "Restore unsaved changes?" (PRJ-020): call answer(true) to restore, answer(false) to keep the saved version
    // (the copy is then set aside as <name>.discarded.json). Unset: the copy is kept and not restored.
    std::function<void(const juce::File& autosave, std::function<void(bool restore)> answer)> askRestore;
    std::function<void(DeviceState)> onDeviceState;

private:
    void startupOpen(const StartupDecision&, std::function<void(const StartupResult&)> done);
    void startNew(StartupResult, std::function<void(const StartupResult&)> done);
    void offerAutosave(const std::optional<juce::File>& autosave, StartupResult,
                       std::function<void(const StartupResult&)> done);

    core::CommandBus& bus_;
    services::library::Workers& workers_;
    engine::EngineBridge* bridge_;
    SessionConfig config_;
    std::shared_ptr<Resume> resume_; // shared with worker jobs of the store
    ProjectStore store_;
    Autosave autosave_;
    MissingMedia missing_;
    Progress startupProgress_;
    std::optional<double> settleAtMs_;
    bool settlePending_ = false;
    double lastNowMs_ = 0;
    DeviceState deviceState_ = DeviceState::Running;
    bool wasPlaying_ = false;
    double lostPpq_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace services::store
