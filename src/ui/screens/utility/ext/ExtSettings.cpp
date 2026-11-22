// UTILITY rows of the P3 extensions (07-ui-design/screens/utility.md "Extension points": a new section
// before INFORMATION, UID-220; registered through APP_REGISTER_SETTINGS, no list edited). Every row carries the
// feature flag `ext.enabled`, so with SIXTEEN EXTENSIONS off both sections are empty and not shown (the plain UTILITY).
//   EXTENSIONS   the per-item switches (preferences ext.trk.toggle / ext.interop.export / ext.strip.record)
//   MIDI LEARN   its switch (ext.midi.learn, MID-034). Arming a target and listing / clearing bindings need an
//                action row or a page; SettingsList's actions and info sources are a fixed set, so
//                those stay on the scripted `cmd ext-learn` / `ext-unlearn` until the list offers an action hook.
#include "ui/descriptors/SettingsSpec.h"

namespace
{
using ui::SettingsRowEntry;
using ui::SettingsSectionSpec;
constexpr auto kValue = ui::SettingsRowSpec::Kind::Value;
} // namespace

APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "extensions", "EXTENSIONS", 900}))
APP_REGISTER_SETTINGS((SettingsSectionSpec {"utility", "midiLearn", "MIDI LEARN", 910}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "extensions", 10,
                                         {kValue, "TOGGLE TRIGGER MODE", "ext.trk.toggle", {}, {}, "ext.enabled"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "extensions", 20,
                                         {kValue, "PROJECT EXPORT (.PRJ)", "ext.interop.export", {}, {}, "ext.enabled"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "extensions", 30,
                                         {kValue, "RECORD STRIP MOVES", "ext.strip.record", {}, {}, "ext.enabled"}}))
APP_REGISTER_SETTINGS((SettingsRowEntry {"utility", "midiLearn", 10,
                                         {kValue, "MIDI LEARN", "ext.midi.learn", {}, {}, "ext.enabled"}}))
