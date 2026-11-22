// The sequencing-screen descriptors validate (parents, strip ids, body components, UID-207), replace the shell's
// PROJECT fallback, keep their rects inside their body templates on the 4 lpx grid; which interim parameter stubs are
// still in use (none once every catalogue is real); and every one of these screens builds its frames with a running
// playhead and takes hot input without allocating after warm-up.
#include "core/util/Realtime.h"
#include "tests/snapshot/m2/M2Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/StepGrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace
{
constexpr std::string_view kScreens[] = {"track.sequence", "sceneManager",   "project",
                                         "project.open",   "project.delete", "project.renderAudio"};
}

TEST_CASE("ui.m2.registry: the sequencing descriptors validate and replace the fallbacks", "[m2]")
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
    CHECK(screens.find("project.renderAudio")->featureFlag.empty()); // ui/screens/render, not flagged
    CHECK(screens.find("sceneManager")->parent == "home");
    CHECK(screens.find("track.sequence")->parent == "track.menu");
    const auto* bpm = ui::PopupRegistry::builtin().find("bpmQuantize");
    REQUIRE(bpm != nullptr);
    CHECK(bpm->kind == ui::PopupSpec::Kind::Modal);
    CHECK(ui::WidgetRegistry::builtin().find(bpm->bodyComponent) != nullptr);
    // SEQUENCE's cells, ruler and bottom row sit inside the Grid template on the 4 lpx grid (UID-103).
    namespace L = ui::layout;
    for (int n = 1; n <= 16; ++n)
    {
        CHECK(L::insideTemplate(ui::BodyTemplate::Grid, ui::StepGrid::cell(n)));
        CHECK(L::onGrid(ui::StepGrid::cell(n)));
    }
    CHECK(L::insideTemplate(ui::BodyTemplate::Grid, ui::StepGrid::kPanel));
}

TEST_CASE("ui.m2.registry: interim parameter stubs still in use (empty once every catalogue is real)", "[m2][!mayfail]")
{
    std::string stubs;
    for (const auto& p : core::builtinParams().all())
        if (p.featureFlag == ui::m2::kParamStubFlag)
            stubs += std::string(p.id) + " ";
    INFO("stubbed: " << stubs);
    CHECK(stubs.empty());
}

TEST_CASE("ui.m2.alloc: every sequencing screen builds frames and takes hot input without allocating", "[m2][alloc]")
{
    m2test::M2Fixture f(true, false);
    auto& s = f.session();
    s.execute(ui::m2::cmd::setPatternLength(s.project(), 0, 0, 32));
    for (int st = 0; st < 32; st += 3)
        f.trigger(1 + st % 16, st);
    f.lock(1, 3, ui::m2::kSlotRetrigCount, 3);
    for (int i = 0; i < 5; ++i)
    {
        ui::m2::ProjectEntry e;
        e.name = "A_long_project_name_" + std::to_string(i);
        e.modifiedKey = 10 - i;
        m2test::FakeProjectSource::instance().addEntry(std::move(e));
    }
    m2test::FakeProjectSource::instance().setCurrent("A_long_project_name_0", true);
    auto& st = f.shell->state();
    st.playing = true;
    engine::MeterFeed m{};
    engine::PlayheadFeed ph{};
    struct Visit
    {
        const char* open; // screen or popup id, "" = stay
        float tapX, tapY; // a tap before measuring (tabs), < 0 = none
    };
    static constexpr Visit visits[] = {{"track.sequence", -1, -1},
                                       {"sceneManager", -1, -1},
                                       {"", 112, 321},
                                       {"", 112, 439},
                                       {"", 112, 203},
                                       {"bpmQuantize", -1, -1},
                                       {"project", -1, -1},
                                       {"project.open", -1, -1},
                                       {"project.delete", -1, -1},
                                       {"project.renderAudio", -1, -1}};
    for (const auto& v : visits)
    {
        if (std::string_view(v.open) == "bpmQuantize")
            f.shell->home();
        if (*v.open != 0)
            REQUIRE(f.shell->open(v.open));
        if (v.tapX >= 0)
            f.tap(v.tapX, v.tapY);
        for (int i = 0; i < 6; ++i)
            f.frame();
        const std::size_t before = core::rt::violationCount();
        core::rt::setAbortOnViolation(false);
        {
            core::rt::ScopedRealtimeThread rt;
            for (int i = 0; i < 40; ++i)
            {
                ph.blockIndex = static_cast<std::uint64_t>(i + 1);
                ph.playing = true;
                ph.step = static_cast<std::uint8_t>(i % 32);
                ph.ppq = (i % 32) * 0.25;
                ph.pendingPattern = static_cast<std::int8_t>(i % 8 < 4 ? 2 : -1);
                f.shell->feeds().inject(m, ph);
                f.turn(io::Control::Selector, i % 2 ? 1 : -1);
                f.press(io::pad(1 + i % 16));
                f.frame();
            }
        }
        core::rt::setAbortOnViolation(true);
        INFO("screen " << v.open << " tap " << v.tapY);
        CHECK(core::rt::violationCount() - before == 0);
    }
}
