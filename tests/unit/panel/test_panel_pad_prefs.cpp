// UTILITY's pad preferences on the reference panel (io/controllers/PanelPads.h): PAD CURVE / FIXED,
// PAD THRESHOLD and PAD SENSITIVITY reach the panel decoder; the defaults (5 = "(Default)") keep the calibration, so
// the emulator's velocities still round-trip.
#include "tests/unit/panel/PanelTestFrames.h"

#include "io/controllers/PanelPads.h"
#include "platform/imx6/panel/PanelCodec.h"

#include <catch2/catch_test_macros.hpp>

using platform::imx6::panel::EventBatch;
using platform::imx6::panel::PanelDecoder;

namespace
{
int hit(PanelDecoder& d, int word) // velocity of a hit with pressure word `word` on pad 1 (0 = no hit)
{
    paneltest::RxFrame f;
    EventBatch batch;
    d.reset();
    REQUIRE(platform::imx6::panel::rxFrameOk(f.finish()));
    d.decode(f.b.data(), 0, batch); // first frame: the reference
    f.pad(1, word, 512);
    batch.count = 0;
    REQUIRE(platform::imx6::panel::rxFrameOk(f.finish()));
    d.decode(f.b.data(), 1, batch);
    return batch.count == 1 ? batch.events[0].value : 0;
}

PanelDecoder with(const io::PanelPadPrefs& p)
{
    io::PadCalibration c;
    io::PadResponse r;
    io::applyPanelPadPrefs(p, {}, c, r);
    PanelDecoder d;
    d.setPads(c, r);
    return d;
}
} // namespace

TEST_CASE("panel pads: the default preferences keep the calibration and the linear curve")
{
    io::PadCalibration c;
    io::PadResponse r;
    io::applyPanelPadPrefs({}, {}, c, r);
    CHECK(c.threshold == io::PadCalibration {}.threshold);
    CHECK(c.floor == io::PadCalibration {}.floor);
    CHECK(c.ceiling == io::PadCalibration {}.ceiling);
    CHECK(r.shape == io::PadCurveShape::Linear);
    CHECK(r.threshold == 1);
    CHECK(r.sensitivity == 8);
    auto d = with({});
    for (int v : {1, 64, 127})
        CHECK(hit(d, io::PadCurve::emulatorWord(v)) == v);
}

TEST_CASE("panel pads: PAD THRESHOLD moves the hit threshold 8 words per step around 5")
{
    io::PadCalibration c;
    io::PadResponse r;
    io::applyPanelPadPrefs({.threshold = 16}, {}, c, r);
    CHECK(c.threshold == 169 + 88);
    io::applyPanelPadPrefs({.threshold = 1}, {}, c, r);
    CHECK(c.threshold == 169 - 32);
    auto high = with({.threshold = 16});
    CHECK(hit(high, 200) == 0); // a light hit no longer registers
    CHECK(hit(high, 300) > 0);
    auto low = with({.threshold = 1});
    CHECK(hit(low, 150) > 0); // below the calibration's 169
}

TEST_CASE("panel pads: PAD SENSITIVITY scales the velocity of the same hit, 5 neutral")
{
    const int word = io::PadCurve::emulatorWord(64);
    auto neutral = with({});
    auto more = with({.sensitivity = 16});
    auto less = with({.sensitivity = 1});
    CHECK(hit(neutral, word) == 64);
    CHECK(hit(more, word) > 64);
    CHECK(hit(less, word) < 64);
    CHECK(hit(more, 523) == 127); // gain 2: the ceiling is halfway
    int previous = 0;
    for (int s = 1; s <= 16; ++s) // monotonic in the setting
    {
        auto d = with({.sensitivity = s});
        const int v = hit(d, word);
        CHECK(v >= previous);
        previous = v;
    }
}

TEST_CASE("panel pads: PAD CURVE FIXED plays the fixed velocity; Hard / Soft bend the curve")
{
    auto fixed = with({.curve = 7, .fixedVelocity = 90});
    CHECK(hit(fixed, io::PadCurve::emulatorWord(10)) == 90);
    CHECK(hit(fixed, io::PadCurve::emulatorWord(127)) == 90);
    const int word = io::PadCurve::emulatorWord(64);
    auto hard = with({.curve = 0}), soft = with({.curve = 6});
    CHECK(hit(hard, word) < 64);
    CHECK(hit(soft, word) > 64);
}

TEST_CASE("panel pads: the message thread's publish reaches the reader once per change")
{
    std::uint32_t seen = 0;
    io::PanelPadPrefs out;
    while (io::takePanelPads(seen, out)) // whatever earlier cases published
    {
    }
    CHECK_FALSE(io::takePanelPads(seen, out));
    io::publishPanelPads({.curve = 2, .fixedVelocity = 77, .threshold = 9, .sensitivity = 3});
    REQUIRE(io::takePanelPads(seen, out));
    CHECK(out == io::PanelPadPrefs {.curve = 2, .fixedVelocity = 77, .threshold = 9, .sensitivity = 3});
    CHECK_FALSE(io::takePanelPads(seen, out));
}
