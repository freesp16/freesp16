// The UTILITY screens through the real shell: M4Fixture (built-in registries, a fresh m2::Session = the project the
// PROJECT SETTING rows edit) plus installed preferences, UTILITY's providers and the controller pages from tests/fakes
// when they are not registered.
#pragma once

#include "services/settings/Preferences.h"
#include "tests/fakes/FakeCtrlScreens.h"
#include "tests/snapshot/m4/M4Fixture.h"
#include "ui/screens/utility/SettingsList.h"
#include "ui/screens/utility/UtilityEnv.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace m6test
{
struct M6Setup
{
    M6Setup()
    {
        fakes::registerCtrlScreensIfMissing(); // before any shell resolves screens
        ui::utility::forgetUtilityFocus();
        ui::utility::utilityEnv() = {};
    }
    ~M6Setup()
    {
        ui::utility::forgetUtilityFocus();
        ui::utility::utilityEnv() = {};
    }
};

struct M6Fixture
{
    M6Setup setup;
    services::settings::Preferences prefs;
    m4test::M4Fixture m;

    explicit M6Fixture(bool logging = true) : m(logging) { services::settings::install(&prefs); }
    ~M6Fixture() { services::settings::install(nullptr); }

    ui::Shell& shell() { return m.shell(); }
    const std::vector<ui::utility::SettingsRow>& rows() { return *ui::utility::settingsRows(shell().body()); }
    int focus() { return ui::utility::settingsFocus(shell().body()); }
    bool editing() { return ui::utility::settingsEditing(shell().body()); }
    std::string value(int row) { return ui::utility::settingsValueText(shell().body(), row); }
    std::string focusedLabel() { return rows()[static_cast<std::size_t>(focus())].label; }
    int rowOf(std::string_view label, int nth = 0)
    {
        const auto& r = rows();
        for (std::size_t i = 0; i < r.size(); ++i)
            if (r[i].label == label && nth-- == 0)
                return static_cast<int>(i);
        return -1;
    }
    // Moves the rotary to the row `label` (the nth one) by panel turns only.
    void focusRow(std::string_view label, int nth = 0)
    {
        const int want = rowOf(label, nth);
        REQUIRE(want >= 0);
        for (int guard = 0; guard < 200 && focus() != want; ++guard)
            m.f.turn(io::Control::Selector, focus() < want ? 1 : -1);
        REQUIRE(focus() == want);
    }
    void push() { m.click(io::Control::SelectorPush); m.f.settle(); }
    void back() { m.click(io::Control::Back); m.f.settle(); }
    void turn(int d) { m.f.turn(io::Control::Selector, d); m.f.settle(); }
};

inline const snapshot::Image& m6mockup(std::string_view name)
{
    static std::vector<std::string> keys;
    keys.reserve(8);
    keys.push_back("../../../tests/snapshot/m6/mockups/" + std::string(name));
    return snapshot::mockup(keys.back());
}
} // namespace m6test
