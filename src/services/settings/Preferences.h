// Application preferences (02-features/settings.md SET-010…013, 03-ui/screens.md UI-325):
// the device-wide values that are never saved in a project, kept in one versioned JSON file and saved "immediately"
// (≤ 1 s) after a change.
//
// Two settings files, two jobs:
//   <name>.conf          platform::Paths::settingsFile — read-only start-up settings of the machine (platform, render,
//                        audio_device, run_frames, diagnostics: key=value, environment <PREFIX>_<KEY> wins). Written by
//                        hand or a dev overlay, never by the app; read once by app/App.cpp before anything starts.
//   preferences.json     <data dir>/preferences.json (platform::Paths::dataDir: the device's app folder on internal
//                        storage, the host's XDG data dir) — the user's preferences, written only by this class through
//                        services::store::AtomicWriter::writeFileAtomically (temp → fsync → rename → fsync folder), so
//                        a kill -9 or a power cut during a save leaves the previous or the new file, never a torn one.
//
// What is a preference: every ParamSpec with scope Global whose storage key is not "session.*" / "internal.*" (UI
// session state and engine messages) and that is not a read-only display (no detents) — persists(spec) — plus the
// machine settings UTILITY shows that have no model field (METRONOME MONITOR / VOLUME, COUNT IN), plus the
// PrefSpecs registered with APP_REGISTER_PREF (values without a ParamSpec: feature flags ext.*, per-port MIDI IN, …).
// A ParamSpec preference is stored under its storage key (ParamSpec::storageKey()), "#<scope>" appended for a scope
// other than 0 (midi.portMode#2 = port 3). Strings and string lists (library locations) have their own maps.
//
// File (schema version 1; members sorted, so a save is reproducible):
//   {"format": "<name>-preferences", "version": 1,
//    "values": {"project.resume": 0, "midi.portMode#2": 3, …},
//    "strings": {"audio.deviceName": "…"}, "lists": {"library.locations": ["/media/…", …]}}
// Unknown keys are kept and written back (a newer build's preferences survive an older build). A file without
// "version" is schema 0 (a flat {key: number} object, the pre-release layout) and is migrated. A file that does not
// parse is moved aside to preferences.json.corrupt and the defaults are used (load() reports it).
//
// Threads: message thread only, except the write job, which runs on the executor (a library worker in the app) with
// an immutable copy of the text. Saves are debounced: tick() starts a write kDebounceMs after the last change, or
// kMaxDelayMs after the first unsaved one while changes keep coming (a turned rotary does not write per detent);
// one write is in flight at a time. flush() writes synchronously (quit, standby). Never on the audio thread: the
// engine sees preferences only as ParamChanges posted by the shell's ParamAccess.
#pragma once

#include "core/commands/Command.h"
#include "core/params/ParamRegistry.h"
#include "core/util/Registrar.h"
#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace services::settings
{
using ::core::Result;

// A preference that is not a ParamSpec. `values` = enum labels from minValue; `section` = the
// settings.md group (Audio, MIDI, Controllers, Behaviour, Display, Library, Recording, Extensions).
struct PrefSpec
{
    std::string_view key, label, section;
    core::ParamType type = core::ParamType::Integer;
    float minValue = 0, maxValue = 1, def = 0;
    std::span<const std::string_view> values {};
    std::string_view featureFlag {};
};

class PrefRegistry
{
public:
    static PrefRegistry& builtin() noexcept;
    void add(const PrefSpec&);
    const PrefSpec* find(std::string_view key) const noexcept;
    // The PrefSpec as a ParamSpec (scope Global), so lists, formatting and stepping reuse ParamSpec's code.
    const core::ParamSpec* paramOf(std::string_view key) const noexcept;
    std::vector<const PrefSpec*> all() const;

private:
    std::deque<PrefSpec> specs_;
    std::deque<core::ParamSpec> params_;
};

// True when `spec`'s value is a preference this class saves (see the header).
bool persists(const core::ParamSpec& spec) noexcept;
// The file key of `spec` at `scope`: its storage key, "#<scope>" appended for scope ≠ 0.
std::string keyOf(const core::ParamSpec& spec, std::uint16_t scope = 0);
// The machine settings UTILITY shows that live in preferences although their ParamSpec scope is Project (our choice).
std::span<const std::string_view> machineSettingIds() noexcept;

class Preferences
{
public:
    static constexpr int kSchemaVersion = 1;
    static constexpr double kDebounceMs = 250, kMaxDelayMs = 1000;
    // Runs one write job off the message thread (the app: a library worker). Empty: the job runs synchronously.
    using Executor = std::function<void(std::function<void()> job)>;

    explicit Preferences(const core::ParamRegistry& = core::builtinParams(), Executor = {});
    ~Preferences(); // waits for a write in flight (does not start one: call flush() first)
    Preferences(const Preferences&) = delete;
    Preferences& operator= (const Preferences&) = delete;

    // Reads `file` (and remembers it as the save target). Missing = the defaults (ok). Migrates older schemas; a
    // corrupt file is moved aside and reported (the defaults are used either way).
    Result<void> load(const juce::File& file);
    const juce::File& file() const noexcept { return file_; }
    void setFile(const juce::File& file) { file_ = file; }

    // The stored value, else the default of the key's ParamSpec / PrefSpec ("#n" ignored for the lookup), else 0.
    float get(std::string_view key) const;
    bool has(std::string_view key) const;
    // Clamps to the spec's range when the key is known; nothing happens (no save, no observer) when unchanged.
    void set(std::string_view key, float value);
    void reset(std::string_view key); // back to the default (the key is removed from the file)
    std::string getString(std::string_view key) const;
    void setString(std::string_view key, std::string value);
    std::vector<std::string> getList(std::string_view key) const;
    void setList(std::string_view key, std::vector<std::string> values);
    // Every stored number (key, value), sorted by key.
    const std::map<std::string, float, std::less<>>& values() const noexcept { return values_; }

    // fn(key) after every change of a key that starts with `keyPrefix` ("" = every key).
    core::Subscription observe(std::string_view keyPrefix, std::function<void(std::string_view key)> fn);

    void tick(double nowMs); // message thread, once per frame: starts the debounced write
    void flush();            // writes now on this thread when anything is unsaved (waits for a write in flight)
    bool dirty() const noexcept { return dirty_; }
    bool writing() const noexcept { return inFlight_->load(); }
    int writes() const noexcept { return writes_->load(); } // completed writes
    std::string lastError() const;                           // of the last write ("" = ok)

    // SET-012: the preferences plus every user controller profile (*.json of the profiles folder) in one file;
    // import applies the values (observers run) and writes the profiles back.
    void setProfilesFolder(const juce::File& folder) { profiles_ = folder; }
    Result<void> exportTo(const juce::File&) const;
    Result<void> importFrom(const juce::File&);

    std::string toJson() const;
    Result<void> fromJson(const juce::String& text); // replaces every value (no observers; load() uses it)

private:
    struct Observer
    {
        int id;
        std::string prefix;
        std::function<void(std::string_view)> fn;
    };
    const core::ParamSpec* specOf(std::string_view key) const;
    void changed(std::string_view key);
    void startWrite();
    void waitIdle() const;

    const core::ParamRegistry& params_;
    Executor executor_;
    juce::File file_, profiles_;
    std::map<std::string, float, std::less<>> values_;
    std::map<std::string, std::string, std::less<>> strings_;
    std::map<std::string, std::vector<std::string>, std::less<>> lists_;
    juce::var extra_; // unknown top-level members, written back
    std::vector<Observer> observers_;
    int nextObserver_ = 1;
    bool dirty_ = false;
    double now_ = 0, firstDirtyMs_ = 0, lastChangeMs_ = 0;
    // Shared with the write job (it may outlive a moved-from state only through these).
    std::shared_ptr<std::atomic<bool>> inFlight_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> writes_ = std::make_shared<std::atomic<int>>(0);
    struct ErrorBox
    {
        std::mutex mutex;
        std::string text;
    };
    std::shared_ptr<ErrorBox> error_ = std::make_shared<ErrorBox>();
};

// Feature flags of the extensions (FX-001, UID-024/225): preference "ext.<flag>" under the master switch
// "ext.enabled" ("Sixteen extensions"). With the master off every flag is off: the UI is the reference set.
class FeatureFlags
{
public:
    static constexpr std::string_view kMaster = "ext.enabled";
    explicit FeatureFlags(const Preferences& prefs) noexcept : prefs_(prefs) {}
    bool isOn(std::string_view flag) const noexcept; // "" = always on; "ext.x" or "x"
    std::vector<std::string> enabled() const;         // the flags that are on (without "ext.")

private:
    const Preferences& prefs_;
};

// The app's preferences for screens that are not handed them (UTILITY): installed by the composition root, null in
// tests that do not install one. Message thread.
Preferences* installed() noexcept;
void install(Preferences*) noexcept;
} // namespace services::settings

// APP_REGISTER_PREF((services::settings::PrefSpec{...})): static registrar, one .cpp per group.
#define APP_REGISTER_PREF(...) APP_STATIC_REGISTRAR(::services::settings::PrefRegistry::builtin().add(__VA_ARGS__))
