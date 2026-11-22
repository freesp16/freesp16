// Stand-ins for the controller UTILITY pages: SETTINGS registers rows that open utility.controllers and
// utility.keyboardMap, which the controller screens register; tests that run without those screens register these
// from tests/fakes/. Registered at run time and only when missing, so the real screens win when they are built in
// (no duplicate ids). Header-only: a test calls it before opening UTILITY.
#pragma once

#include "ui/descriptors/ScreenSpec.h"

namespace fakes
{
inline void registerCtrlScreensIfMissing()
{
    auto& reg = ui::ScreenRegistry::builtin();
    static constexpr ui::ScreenSpec controllers {.id = "utility.controllers",
                                                 .title = "CONTROLLERS",
                                                 .parent = "utility",
                                                 .body = ui::BodyTemplate::Full,
                                                 .contextBar = ui::ContextBarKind::Menu,
                                                 .rail = {},
                                                 .bodyComponent = "SettingsList",
                                                 .strip = {"CONTROLLERS", {}}};
    static constexpr ui::ScreenSpec keyboard {.id = "utility.keyboardMap",
                                              .title = "KEYBOARD MAP",
                                              .parent = "utility",
                                              .body = ui::BodyTemplate::Full,
                                              .contextBar = ui::ContextBarKind::Menu,
                                              .rail = {},
                                              .bodyComponent = "SettingsList",
                                              .strip = {"KEYBOARD MAP", {}}};
    if (reg.find(controllers.id) == nullptr)
        reg.add(controllers);
    if (reg.find(keyboard.id) == nullptr)
        reg.add(keyboard);
}
} // namespace fakes
