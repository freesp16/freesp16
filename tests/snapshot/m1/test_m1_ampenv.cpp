// AMP ENVELOPE (amp-envelope.svg: track 1, attack 0.5 ms, hold 120 ms, release 300 ms CURVE, H active) and its
// SETTING popup (amp-envelope-setting.svg, Release Curve focused); window re-fit after 600 ms; rotary handles.
#include "core/util/ampenv/EnvTime.h"
#include "tests/snapshot/m1/M1Fixture.h"
#include "ui/shell/Layout.h"
#include "ui/widgets/EnvelopeEditor.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace snapshot;
using m1test::M1Fixture;
namespace L = ui::layout;
namespace env = core::ampenv;

namespace
{
float indexOf(env::EnvStage s, float seconds)
{
    int best = 0;
    float bestD = 1e9f;
    for (int i = 0; i <= 127; ++i)
    {
        const float d = std::fabs(env::envSeconds(s, static_cast<std::uint8_t>(i), false, 120) - seconds);
        if (d < bestD)
            bestD = d, best = i;
    }
    return static_cast<float>(best);
}

void percussive(M1Fixture& f)
{
    f.set("env.attack", 1, indexOf(env::EnvStage::Attack, 0.0005f));
    f.set("env.hold", 1, indexOf(env::EnvStage::Hold, 0.120f));
    f.set("env.release", 1, indexOf(env::EnvStage::Release, 0.300f));
    f.set("env.releaseCurve", 1, 1);
    REQUIRE(f.shell->open("track.menu"));
    f.settle();
    REQUIRE(f.shell->open("track.ampEnvelope"));
    f.settle(3); // fit + the base animation
}
} // namespace

TEST_CASE("ui.m1.ampEnvelope: AMP ENVELOPE matches amp-envelope.svg", "[snapshot][m1]")
{
    Session session;
    M1Fixture f;
    percussive(f);
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("amp-envelope");
    CHECK(compare(ours, mock, L::canvas, "m1-amp-envelope").match >= kGate);
    CHECK(compare(ours, mock, ui::EnvelopeEditor::kPanel, "m1-amp-envelope-graph").match >= kGate);
    CHECK(compare(ours, mock, {16, 152, 192, 456}, "m1-amp-envelope-rail").match >= kGate);
    CHECK(compare(ours, mock, L::knobStrip, "m1-amp-envelope-strip").match >= kGate);
}

TEST_CASE("ui.m1.ampEnvelope: push opens SETTING, matching amp-envelope-setting.svg", "[snapshot][m1]")
{
    Session session;
    M1Fixture f;
    percussive(f);
    f.press(io::Control::SelectorPush);
    f.turn(io::Control::Selector, 1); // focus Release Curve
    REQUIRE(f.shell->popupDepth() == 1);
    const auto ours = f.image();
    const auto& mock = m1test::m1mockup("amp-envelope-setting");
    CHECK(compare(ours, mock, {320, 294, 640, 180}, "m1-amp-envelope-setting-popup").match >= kGate);
    CHECK(compare(ours, mock, L::canvas, "m1-amp-envelope-setting").match >= kGate);
}

namespace
{
bool shows(M1Fixture& f, std::string_view text)
{
    for (const auto& c : f.frame(100).commands())
        if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c) == text)
            return true;
    return false;
}
} // namespace

TEST_CASE("ui.m1.ampEnvelope: AMP ENV AUTO ZOOM OFF (default) widens the window at once, never shrinks it", "[m1]")
{
    M1Fixture f;
    percussive(f);
    REQUIRE(shows(f, "500 ms"));
    f.turn(io::knob(4), 40); // RELEASE much longer: the axis widens on the next frame
    CHECK_FALSE(shows(f, "500 ms"));
    f.turn(io::knob(4), -40); // back: the wider axis stays (no re-fit)
    f.frame(700);
    f.frame(300);
    CHECK_FALSE(shows(f, "500 ms"));
}

TEST_CASE("ui.m1.ampEnvelope: the window re-fits 600 ms after the last change; the rotary walks A-H-R", "[m1]")
{
    M1Fixture f;
    const auto* autoZoom = f.shell->params().find("ui.envAutoZoom");
    REQUIRE(autoZoom != nullptr);
    f.shell->values().set(*autoZoom, 0, 1.0f); // AMP ENV AUTO ZOOM ON
    percussive(f);
    auto* body = f.shell->body();
    REQUIRE(body != nullptr);
    f.turn(io::knob(4), 40); // RELEASE much longer
    f.frame(100);
    // Still the old window right after the change (the handle under the finger does not jump) …
    bool oldWindow = false;
    for (const auto& c : f.frame(100).commands())
        if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c) == "500 ms")
            oldWindow = true;
    CHECK(oldWindow);
    f.frame(700);
    f.frame(300);
    bool refit = true;
    for (const auto& c : f.frame(100).commands())
        if (c.op == ui::DrawCmd::Op::Text && f.dl.textOf(c) == "500 ms")
            refit = false;
    CHECK(refit);
    CHECK(body->dirty(f.now) == false);
}
