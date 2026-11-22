// The per-item feature flags of the extensions as preferences (APP_REGISTER_PREF; settings.md group
// "Extensions"): every one off by default and effective only with the master switch `ext.enabled` ("SIXTEEN
// EXTENSIONS", services/settings/Prefs.cpp). UTILITY shows them (ui/screens/utility/ext/ExtSettings.cpp) only while
// the master switch is on, so with it off UTILITY shows the standard list.
#include "services/settings/Preferences.h"

namespace services::ext
{
namespace
{
constexpr std::string_view kOffOn[] = {"OFF", "ON"};
} // namespace
} // namespace services::ext

APP_REGISTER_PREF((services::settings::PrefSpec {"ext.trk.toggle", "TOGGLE TRIGGER MODE", "Extensions",
                                                 core::ParamType::Enum, 0, 1, 0, services::ext::kOffOn}))
APP_REGISTER_PREF((services::settings::PrefSpec {"ext.midi.learn", "MIDI LEARN", "Extensions", core::ParamType::Enum,
                                                 0, 1, 0, services::ext::kOffOn}))
APP_REGISTER_PREF((services::settings::PrefSpec {"ext.interop.export", "PROJECT EXPORT (.PRJ)", "Extensions",
                                                 core::ParamType::Enum, 0, 1, 0, services::ext::kOffOn}))
APP_REGISTER_PREF((services::settings::PrefSpec {"ext.strip.record", "RECORD STRIP MOVES", "Extensions",
                                                 core::ParamType::Enum, 0, 1, 0, services::ext::kOffOn}))
