// The sample-playback descriptors validate (parents, opens, strip ids, rail ≤ 4 / compact 5, body components,
// UID-207), these screens replace the shell's fallbacks, no interim parameter stub (ui.m1.paramStub) is left, and
// every one of these screens builds its frames and takes hot input without allocating after warm-up.
#include "core/util/Realtime.h"
#include "tests/snapshot/m1/DemoSample.h"
#include "tests/snapshot/m1/FakeLibrary.h"
#include "tests/snapshot/m1/M1Fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace
{
constexpr std::string_view kScreens[] = {"home",
                                         "trackSetting",
                                         "mixer",
                                         "mixer.output",
                                         "track.menu",
                                         "track.playback",
                                         "track.playback.setScale",
                                         "track.ampEnvelope",
                                         "track.browse"};
}

TEST_CASE("ui.m1.registry: the sample-playback descriptors validate and replace the fallbacks", "[m1]")
{
    auto& screens = ui::ScreenRegistry::builtin();
    const auto problems = screens.validate(ui::WidgetRegistry::builtin().dispatcher(), core::builtinParams());
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
    for (const auto id : kScreens)
    {
        const auto* s = screens.find(id);
        REQUIRE(s != nullptr);
        CHECK(s->bodyComponent.rfind("shell.", 0) != 0);
        CHECK(ui::WidgetRegistry::builtin().find(s->bodyComponent) != nullptr);
    }
    REQUIRE(screens.find("track.playback.setSlice") != nullptr);
    CHECK(screens.find("track.playback.setSlice")->featureFlag.empty()); // ui.setSlice is on
    CHECK(ui::PopupRegistry::builtin().find("track.type") != nullptr);
    for (const auto* w :
         {"TrackTile", "ModuleCard", "WaveformEditor", "Meter", "ChannelStrip", "ListTable", "EnvelopeEditor"})
        CHECK(ui::WidgetRegistry::builtin().find(w) != nullptr);
}

TEST_CASE("ui.m1.registry: no interim parameter stub is left (every catalogue is real)", "[m1]")
{
    for (const auto& p : core::builtinParams().all())
    {
        INFO(p.id);
        CHECK(p.featureFlag != "ui.m1.paramStub");
    }
}

TEST_CASE("ui.m1.alloc: every sample-playback screen builds frames and takes hot input without allocating",
          "[m1][alloc]")
{
    m1test::DemoSample beat{"beat"};
    m1test::FakeLibrary lib;
    ui::m1::SampleViews::instance().setProvider([&](int t)
                                                { return t == 5 ? beat.view("Beat") : ui::m1::SampleView{}; });
    ui::m1::BrowseSource::install(&lib);
    {
        m1test::M1Fixture f(true, false);
        f.shell->state().selectedTrack = 5;
        engine::MeterFeed m{};
        engine::PlayheadFeed ph{};
        ph.trackPosition.fill(-1);
        for (const auto id : kScreens)
        {
            if (id != "home")
                REQUIRE(f.shell->open(id));
            for (int i = 0; i < 4; ++i)
                f.frame();
            std::size_t before = core::rt::violationCount();
            core::rt::setAbortOnViolation(false);
            {
                core::rt::ScopedRealtimeThread rt;
                for (int i = 0; i < 30; ++i)
                {
                    for (int t = 0; t < 16; ++t)
                        m.trackPeak[static_cast<std::size_t>(t)] = {0.5f * std::fabs(std::sin(i * 0.3f + t)), 0.4f};
                    m.masterPeak = {0.6f, 0.5f};
                    m.sendPeak = {0.2f, 0.2f};
                    ph.trackPosition[4] = static_cast<float>(i % 30) / 30.0f;
                    f.shell->feeds().inject(m, ph);
                    f.turn(io::knob(1 + i % 6), i % 2 ? 1 : -1);
                    f.press(io::pad(1 + i % 16));
                    f.frame();
                }
            }
            core::rt::setAbortOnViolation(true);
            INFO("screen " << id);
            CHECK(core::rt::violationCount() - before == 0);
        }
    }
    ui::m1::BrowseSource::install(nullptr);
    ui::m1::SampleViews::instance().setProvider({});
}
