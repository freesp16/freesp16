// The preferences that are not ParamSpecs (APP_REGISTER_PREF; services/settings/Preferences.h): no engine binding, no
// knob-strip column, read through services::settings::Preferences.
//   ext.enabled      "Sixteen extensions" (FX-001): the master switch of every ext.* feature flag (UID-024/225)
//   midi.portIn#p    MIDI PORTS › IN: input p (0-based) on / off (MID-002) — the MIDI service reads it
#include "services/settings/Preferences.h"

namespace services::settings
{
namespace
{
constexpr std::string_view kOffOn[] = {"OFF", "ON"};
} // namespace
} // namespace services::settings

APP_REGISTER_PREF((services::settings::PrefSpec {"ext.enabled", "SIXTEEN EXTENSIONS", "Extensions",
                                                 core::ParamType::Enum, 0, 1, 0, services::settings::kOffOn}))
APP_REGISTER_PREF((services::settings::PrefSpec {"midi.portIn", "IN", "MIDI", core::ParamType::Enum, 0, 1, 1,
                                                 services::settings::kOffOn}))
