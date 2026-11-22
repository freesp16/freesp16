// Shared set-up of the fxui unit tests: the test-only effect world (tests/snapshot/m3/FxFixture.h) installed as the
// effect environment, a Shell with the effect screens, fixed text metrics.
#pragma once

#include "tests/snapshot/m3/FxFixture.h"
#include "ui/widgets/GridPopup.h"
#include "ui/widgets/ListPopup.h"

#include <catch2/catch_test_macros.hpp>

namespace fxuitest
{
inline ui::TextMeasure fixedMeasure()
{
    return [](ui::TextStyleId, std::string_view t) { return 8.0f * static_cast<float>(t.size()); };
}

struct Env
{
    fxtest::World& world = fxtest::World::get();
    ui::fx::ScopedFxEnvironment env;
    fxtest::FxShell f{fixedMeasure()};

    explicit Env(bool extensions = false) : env(fxtest::World::get().environment(extensions))
    {
        world.slots.reset();
        f.shell->state().selectedTrack = 5;
    }
    ui::EffectPanel& open(std::string_view screen, std::string_view effect = {}, ui::fx::Slot slot = ui::fx::Slot::Fx1)
    {
        if (! effect.empty())
            world.slots.selectEffect(slot, 1, 5, effect);
        REQUIRE(f.shell->open(screen));
        f.settle();
        auto* p = f.panel();
        REQUIRE(p != nullptr);
        if (! ui::fx::isInsert(slot))
            p->bindSlot(slot == ui::fx::Slot::Send ? "send" : "master", 0);
        f.frame();
        return *p;
    }
    ui::GridPopup* grid() { return dynamic_cast<ui::GridPopup*>(f.shell->topPopup()); }
    ui::ListPopup* list() { return dynamic_cast<ui::ListPopup*>(f.shell->topPopup()); }
};
} // namespace fxuitest
