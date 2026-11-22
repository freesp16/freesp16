// The ANALOG FILTER sub-page (mixer.afs; mixer.md "Popups" PROCESSOR › MODEL / EXTERNAL; AFS-005): the
// processor and slope buttons write afs.processor / afs.slope, MEASURE posts afs.ping only with EXTERNAL selected,
// and the latency readout follows afs.latency (the app's per-device preference).
#include "tests/unit/ui/ShellHarness.h"

#include "ui/descriptors/ScreenSpec.h"
#include "ui/widgets/Widget.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("afs.ui: the PROCESSOR page — buttons, MEASURE and the latency readout", "[afs][ui]")
{
    const auto* spec = ui::ScreenRegistry::builtin().find("mixer.afs");
    REQUIRE(spec != nullptr);
    CHECK(spec->parent == "mixer");
    CHECK(ui::WidgetRegistry::builtin().find(spec->bodyComponent) != nullptr);
    for (const auto col : spec->strip.columns)
        CHECK(core::builtinParams().find(col) != nullptr);

    uitest::Harness h({*spec});
    REQUIRE(h.shell->open("mixer.afs"));
    h.frame();
    CHECK(h.drewText("PROCESSOR"));
    CHECK(h.drewText("--")); // not measured yet
    CHECK(h.value("afs.processor") == 1.0f); // default EXTERNAL (the unit's own filter; MODEL without the loop)

    h.touch(240 + 120, 184 + 36); // MODEL
    CHECK(h.value("afs.processor") == 0.0f);
    h.touch(496 + 120, 184 + 36); // EXTERNAL
    CHECK(h.value("afs.processor") == 1.0f);
    h.touch(752 + 120, 392 + 36); // MEASURE
    CHECK(h.value("afs.ping") == 1.0f);
    h.touch(752 + 120, 392 + 36);
    CHECK(h.value("afs.ping") == 2.0f);
    h.touch(240 + 120, 288 + 36); // SLOPE is the model's: inactive while EXTERNAL
    CHECK(h.value("afs.slope") == 0.0f);

    h.touch(240 + 120, 184 + 36); // MODEL
    CHECK(h.value("afs.processor") == 0.0f);
    h.touch(752 + 120, 392 + 36); // MEASURE does nothing with the model
    CHECK(h.value("afs.ping") == 2.0f);
    h.touch(496 + 120, 288 + 36); // 12 dB
    CHECK(h.value("afs.slope") == 1.0f);

    h.shell->values().set(*core::builtinParams().find("afs.latency"), 0, 1234.0f);
    h.frame();
    CHECK(h.drewText("1234 smp  28.0 ms"));
    h.shell->values().set(*core::builtinParams().find("afs.latency"), 0, -1.0f);
    h.frame();
    CHECK(h.drewText("FAILED"));
}
