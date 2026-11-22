// Feature flags of the P3 extensions: the master switch `ext.enabled` and one `ext.<item>` flag per extension, every
// one off by default; with all of them off the app behaves as if they did not exist.
//
// The rule is services::settings::FeatureFlags::isOn's — "" is always on, every other flag needs the
// master switch and its own key ("ext.x" or "x"), a value >= 0.5 is on — over a replaceable source: in the app the
// composition root installs the preferences (app/ext/Extensions: Preferences::get, UTILITY's SIXTEEN EXTENSIONS /
// EXTENSIONS / MIDI LEARN rows, services/ext/ExtPrefs.cpp), with an environment value winning; elsewhere (tests,
// tools) the environment only. Explicit overrides (set(): tests, the scripted `cmd ext-flag`) win over both.
// Environment: <PREFIX>_EXT_ENABLED=1, <PREFIX>_EXT_TRK_TOGGLE=1, … (key upper case, '.' → '_').
//
// Message thread (and workers that only read after start-up). Not used on the audio thread: the engine gets the
// per-track values as ParamChanges only when a flag is on.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace services::ext
{
inline constexpr std::string_view kMaster = "ext.enabled";
inline constexpr std::string_view kToggle = "ext.trk.toggle";         // E11 TRK-012
inline constexpr std::string_view kMidiLearn = "ext.midi.learn";      // E4 MID-034 / FX-088
inline constexpr std::string_view kExport = "ext.interop.export";     // E8 FMT-044 / LIB-041
inline constexpr std::string_view kStripRecord = "ext.strip.record";  // E12 RIB-008

class Flags
{
public:
    // A value for `key` ("ext.enabled", "ext.trk.toggle", …) or nullopt = not set (off).
    using Source = std::function<std::optional<float>(std::string_view key)>;

    bool isOn(std::string_view flag) const;
    std::optional<float> value(std::string_view key) const; // overrides first, then the source
    void set(std::string_view key, bool on);                // override (tests, `cmd ext-flag`)
    void clearOverrides() { overrides_.clear(); }
    void setSource(Source s) { source_ = std::move(s); }    // null = no source (overrides only)

private:
    std::map<std::string, float, std::less<>> overrides_;
    Source source_;
};

// The process's flags (environment source installed). Message thread.
Flags& flags();
// The environment source: <prefix>_<KEY> with the key upper-cased and '.' → '_' (prefix = identity::envPrefix).
std::optional<float> environmentValue(std::string_view key);
} // namespace services::ext
