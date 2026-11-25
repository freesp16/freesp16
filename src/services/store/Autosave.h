// Autosave (PRJ-030/031, NFR-010, NFR-012; native-format.md "System folder"): a copy of the edited project in
// System/autosave/<name>.json, written on a worker from an immutable snapshot — never the user's project file.
//
// When: tick() (once per UI frame) writes when the project has unsaved changes that are not autosaved yet and the
// first of them is 60 s old while the transport is stopped, 5 min old while it plays. onDeviceLost() writes at once
// (NFR-012). A user save makes the copy stale: onSaved() removes it.
//
// Cost on the message thread: one copy of the model into a reused heap snapshot (bench_project `snapshot`, budget
// 4 ms on the A9; ≈ 0.1 ms on the host). Encoding and the atomic write (AtomicWriter::writeFileAtomically: temp →
// fsync → rename → fsync folder) run on the worker. A write in flight is never started twice.
//
// File: one JSON object; its first line is the header (read without parsing the rest), then the migration root of
// the native codec (Migrations.h), so reading it back shares the codec's migrations and decoder:
//     {"autosave": {"format": {"name": "sixteen-autosave", "version": 1}, "name", "project", "savedAt"},
//      "root": {"meta": <meta.json>, "project": <project.json>, "patterns": [<scene-01.json> … <scene-16.json>]}}
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/util/Result.h"
#include "services/library/Workers.h"
#include "services/store/ProjectCodec.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace services::store
{
class Autosave
{
public:
    static constexpr double kIdleIntervalMs = 60000.0;     // PRJ-030: stopped
    static constexpr double kPlayingIntervalMs = 300000.0; // PRJ-030: playing

    struct Config
    {
        juce::File folder; // <data>/System/autosave
        double idleIntervalMs = kIdleIntervalMs, playingIntervalMs = kPlayingIntervalMs;
    };

    Autosave(core::CommandBus&, services::library::Workers&, Config);
    ~Autosave();
    Autosave(const Autosave&) = delete;
    Autosave& operator= (const Autosave&) = delete;

    void setEnabled(bool on) noexcept { enabled_ = on; } // preference project.autosave (default on)
    bool enabled() const noexcept { return enabled_; }
    // UTILITY › AUTOSAVE (settings.autosave): the interval of a stopped transport; playing stays the longer of the two.
    void setIdleInterval(double ms) noexcept { config_.idleIntervalMs = ms; }
    double idleIntervalMs() const noexcept { return config_.idleIntervalMs; }
    double playingIntervalMs() const noexcept { return config_.playingIntervalMs; }
    // The project being edited (after create / open / save as / rename); resets the pending state.
    void setTarget(const juce::String& name, const juce::File& bundle);

    void tick(double nowMs, bool playing); // message thread, once per frame (a few compares when idle)
    void onDeviceLost();                   // NFR-012: write now when there are unsaved changes
    bool writeNow();                       // false when nothing is unsaved or a write runs
    void onSaved();                        // the user saved: the autosave copy is stale and removed

    bool busy() const noexcept { return busy_; }
    int writes() const noexcept { return writes_; } // completed writes (tests, log)
    juce::File file() const { return fileFor(config_.folder, name_); }
    std::function<void(Result<void>)> onWritten; // message thread, after each write
    std::function<void(core::model::Project&)>
        captureRuntimeState; // message thread, before sending the copy to the worker

    // ---- the file (any thread but audio) ----
    static juce::File fileFor(const juce::File& folder, const juce::String& name);
    struct Header
    {
        juce::String name;
        juce::File project; // the bundle the copy belongs to ("" for an unsaved project)
        juce::Time savedAt;
    };
    static std::optional<Header> readHeader(const juce::File&);
    // The autosave of `bundle` (named `name`) when it is newer than the bundle's last save.
    static std::optional<juce::File> newerThan(const juce::File& folder, const juce::String& name,
                                               const juce::File& bundle);
    static std::string encode(const core::model::Project&, const juce::String& name, const juce::File& bundle,
                              const std::string& timestamp = {});
    static Result<void> readInto(const juce::File&, core::model::Project& out, ImportReport&);
    static Result<void> decodeText(const juce::String& text, core::model::Project& out, ImportReport&);

private:
    bool unsavedSinceWrite() const;
    void start();

    core::CommandBus& bus_;
    services::library::Workers& workers_;
    Config config_;
    core::Subscription changesSub_;
    std::uint64_t changes_ = 0, writtenAt_ = 0;
    std::optional<double> pendingSinceMs_;
    double lastNowMs_ = 0;
    juce::String name_;
    juce::File bundle_;
    std::shared_ptr<core::model::Project> snapshot_; // reused (shared with a running write)
    bool enabled_ = true, busy_ = false, staleAfterWrite_ = false, againWhenDone_ = false;
    int writes_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace services::store
