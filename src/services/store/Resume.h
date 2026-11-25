// Resume ("last memory", PRJ-020…023; native-format.md "System folder"; research docs/storage.md §7).
//
//   System/resume.json    { format, project: <bundle path>, selection: {scene, pattern, track, screen}, earlyExits }
//   System/resume.sixteen a copy of the project's JSON files as last saved or loaded (hard links where possible)
//   System/resuming/      crash guard: created before the boot load, removed after it (PRJ-021)
//   System/resumed        early-exit watch: created after a successful boot load, removed once the session
//                         has run Resume::kSettleMs (or shut down cleanly)
//   System/crashed/       quarantine: resume.json + resume.sixteen of a project that stopped the app
//
// Start-up (atStartup):
//   - the guard exists → the previous boot load never finished: resume is skipped at once, the resume file and copy
//     move to System/crashed/, the user is told, a new project starts (PRJ-021, as the reference);
//   - the early-exit marker exists → the app stopped within kSettleMs of reopening the project (a crash, a kill or a
//     power cut): earlyExits + 1; at kMaxEarlyExits (2) the project is treated as crashing — a project that brought
//     the app down twice after resuming is not reopened automatically (our extension of PRJ-021);
//   - otherwise, with resume on (project.resume, PRJ-022), the last project reopens with its selection (PRJ-023); when
//     the bundle is gone, from the resume copy (saved back to the original path). A newer autosave of it is offered
//     ("Restore unsaved changes?", PRJ-020).
// Any thread except audio; file operations are small. markLoaded() runs on a worker (the store calls it after each
// save/load); calls are serialised internally.
#pragma once

#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace services::store
{
using ::core::Result;

struct Selection // PRJ-023
{
    int scene = 0, pattern = 0, track = 0;
    std::string screen {}; // the UI's screen id ("" = home)
    friend bool operator== (const Selection&, const Selection&) = default;
};

struct StartupDecision
{
    enum class Kind : std::uint8_t { NewProject, Resume } kind = Kind::NewProject;
    juce::File source;   // the bundle to open (the project, or System/resume.sixteen when it is gone)
    juce::File identity; // the bundle it is saved to
    Selection selection {};
    std::optional<juce::File> newerAutosave; // ask "Restore unsaved changes?"
    juce::String message;                    // shown when not empty (resume skipped or fell back)
    bool crashed = false;                    // the guard or the early-exit watch tripped
};

class Resume
{
public:
    static constexpr int kMaxEarlyExits = 2;
    static constexpr double kSettleMs = 60000.0;

    struct Config
    {
        juce::File systemDir;   // <data>/System
        juce::File autosaveDir; // <data>/System/autosave (newer-autosave check)
    };
    explicit Resume(Config);

    StartupDecision atStartup(bool resumeEnabled);
    Result<void> beginLoad(); // creates the guard
    void endLoad(bool ok);    // removes the guard; ok → starts the early-exit watch
    void settle();            // ends the early-exit watch (session ran long enough, or a clean shutdown)
    // After a save or a load (worker): resume.json + the resume copy. Another project resets the early-exit count.
    Result<void> markLoaded(const juce::File& bundle, const Selection&);
    Result<void> persistSelection(const Selection&); // refresh the selection in the existing record
    void forget();                          // the resume target was deleted: nothing to resume
    void setAside();                        // the resume target failed to load: move it to System/crashed/

    struct Record
    {
        juce::File project;
        Selection selection {};
        int earlyExits = 0;
    };
    std::optional<Record> read() const;

    juce::File resumeFile() const { return config_.systemDir.getChildFile("resume.json"); }
    juce::File copyBundle() const { return config_.systemDir.getChildFile("resume.sixteen"); }
    juce::File guardDir() const { return config_.systemDir.getChildFile("resuming"); }
    juce::File watchMarker() const { return config_.systemDir.getChildFile("resumed"); }
    juce::File crashedDir() const { return config_.systemDir.getChildFile("crashed"); }

private:
    Result<void> write(const Record&) const;
    void quarantine();

    Config config_;
    mutable std::mutex mutex_;
};
} // namespace services::store
