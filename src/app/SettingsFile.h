// The app's settings (platform::Settings, platform/Platform.h): defaults, then the key=value lines of the platform's settings
// file, then every <PREFIX>_<KEY> environment variable. Keys are normalised to lower case, so any key the
// app or a platform reads can come from either source — not only the keys of the defaults table:
//   file line   `Input_Trace = 0`            → input_trace=0   (key trimmed + lower case, value trimmed, '#' comments)
//   environment `<PREFIX>_LAMP_LOG=1`        → lamp_log=1
//   environment `<PREFIX>_LIVE__INPUTLATENCY=96` → live.inputlatency=96   ("__" = "."; a shell variable name
//               cannot hold a dot, and the device's USB-stick launcher exports `<key>=<value>` lines of its own
//               settings file as <PREFIX>_<KEY> only for keys of [a-z0-9_])
// The environment wins over the file. describeSettings() is the one `SETTINGS …` log line of the start-up.
#pragma once

#include "platform/Platform.h"

#include <string>
#include <string_view>

namespace app
{
using Settings = platform::Settings;

struct SettingsOrigin
{
    std::string file;          // the settings file that was looked at
    bool fileFound = false;    // it existed and was read
    int fileKeys = 0;          // keys set by it
    int envKeys = 0;           // keys set by <PREFIX>_<KEY> variables
};

std::string normaliseSettingsKey(std::string_view raw); // trim, lower case, "__" → "."

// One `key=value` per line; '#' (after leading blanks) starts a comment line; lines without '=' are ignored.
int applySettingsText(Settings&, std::string_view text);
// Every variable `<prefix>_<KEY>=<value>` of `envp` (null-terminated, like environ); returns how many were applied.
int applySettingsEnvironment(Settings&, std::string_view prefix, const char* const* envp);

// defaults → file (when it exists) → environment (envp null = the process environment).
Settings loadSettings(const Settings& defaults, const std::string& file, std::string_view prefix,
                      const char* const* envp = nullptr, SettingsOrigin* origin = nullptr);

// The other direction, for code that reads <PREFIX>_<KEY> variables itself (AlsaOutput's period, the MIDI sources):
// every key of `effective` that differs from `defaults` and has no variable yet is exported as <PREFIX>_<KEY> ("." →
// "__", upper case; setenv without overwrite). Start-up only, before any thread starts. Returns how many.
int exportSettingsToEnvironment(const Settings& effective, const Settings& defaults, std::string_view prefix);
std::string settingsEnvironmentName(std::string_view prefix, std::string_view key); // "<PREFIX>_LIVE__X"

// "file=<path> (read|missing) file_keys=N env_keys=N | k=v k=v …": every key whose value differs from `defaults`
// (or is not in it), sorted; values with blanks are quoted. `(defaults)` when nothing differs.
std::string describeSettings(const Settings& effective, const Settings& defaults, const SettingsOrigin& origin);
} // namespace app
