// ui::theme::ledColour (requested: a distinct colour per pad on the panel): the 16 track colours (base and dim)
// map to 16 distinct saturated LED colours in track order; every other colour passes unchanged.
#include "ui/theme/Theme.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

TEST_CASE("theme.led: track colours map to 16 distinct LED colours; others pass", "[ui][theme]")
{
    std::set<std::uint32_t> seen;
    for (int t = 1; t <= 16; ++t)
    {
        const auto led = ui::theme::ledColour(ui::theme::trackColour(t));
        CHECK(led == ui::theme::kLedColours[t - 1]);
        CHECK(ui::theme::ledColour(ui::theme::trackColour(t, true)) == led); // the dim variant: the same hue
        seen.insert(led);
    }
    CHECK(seen.size() == 16);
    for (const std::uint32_t other : {0xFFFFFFu, 0xFF1E28u, 0x2050FFu, 0x000000u, 0x123456u})
        CHECK(ui::theme::ledColour(other) == other);
}
