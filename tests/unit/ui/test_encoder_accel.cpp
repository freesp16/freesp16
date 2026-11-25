// Panel encoder acceleration (ui/input/panel/EncoderAccel.h) and the drift-free parameter step it relies on.
#include "core/params/ParamRegistry.h"
#include "tests/unit/ui/ShellHarness.h"
#include "ui/input/panel/EncoderAccel.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using ui::EncoderAccel;

namespace
{
// One detent every `intervalMs` from t0: the applied detents per event, capped at `cap`.
std::vector<int> spin(EncoderAccel& a, int count, double intervalMs, int cap, int dir = 1, double t0 = 1000, int n0 = 0)
{
    std::vector<int> out;
    for (int i = 0; i < count; ++i)
        out.push_back(a.turn(n0, dir, t0 + i * intervalMs).scaled(cap));
    return out;
}

bool sameBits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof a) == 0;
}
} // namespace

TEST_CASE("encoder accel: a slow turn is one step per detent", "[ui][encoder]")
{
    EncoderAccel a;
    for (double interval : {200.0, 100.0, 60.0, 45.0})
    {
        a.reset();
        for (int d : spin(a, 64, interval, 16))
            CHECK(d == 1);
    }
}

TEST_CASE("encoder accel: the first detents of a run are exact however fast", "[ui][encoder]")
{
    EncoderAccel a;
    const auto v = spin(a, EncoderAccel::kWarmDetents, 1.0, 16);
    for (int d : v)
        CHECK(d == 1);
    // A burst of detents in one event: only those past the warm-up may scale.
    a.reset();
    const auto t = a.turn(0, 5, 1000);
    CHECK(t.warm == 5 - EncoderAccel::kWarmDetents);
    CHECK(t.scaled(16) == 5); // level 0 on a run's first event
}

TEST_CASE("encoder accel: a fast steady turn ramps up one level at a time and holds", "[ui][encoder]")
{
    EncoderAccel a;
    const auto v = spin(a, 80, 5.0, 16);
    int last = 1;
    for (int d : v)
    {
        CHECK(d >= last);                    // never falls during a steady turn
        CHECK((d == last || d == last * 2)); // never skips a level
        last = d;
    }
    CHECK(v.back() == 16);
    // Held at the top speed, a long column (a sample position) boosts on to ×64, still one level at a time.
    a.reset();
    last = 1;
    for (int d : spin(a, 120, 5.0, 64))
    {
        CHECK((d == last || d == last * 2));
        last = d;
    }
    CHECK(last == 64);
    // Below the top speed there is no boost: a quick but not hard turn tops out at ×4.
    a.reset();
    for (int d : spin(a, 200, 20.0, 64))
        CHECK(d <= 4);
    // Capped by the column: the same gesture on a short range stops at its cap.
    a.reset();
    for (int d : spin(a, 80, 5.0, 4))
        CHECK(d <= 4);
}

TEST_CASE("encoder accel: the same gesture always gives the same result", "[ui][encoder]")
{
    // An uneven hand: intervals from a fixed pseudo-random sequence.
    auto gesture = [](EncoderAccel& a, int cap)
    {
        std::vector<int> out;
        double t = 5000;
        unsigned x = 12345;
        for (int i = 0; i < 200; ++i)
        {
            x = x * 1103515245u + 12345u;
            t += 4.0 + static_cast<double>((x >> 16) % 30);
            const int dir = i < 150 ? 1 : -1;
            out.push_back(a.turn(2, dir, t).scaled(cap));
        }
        return out;
    };
    EncoderAccel a, b;
    const auto first = gesture(a, 16);
    a.reset();
    CHECK(gesture(a, 16) == first);
    CHECK(gesture(b, 16) == first);
}

TEST_CASE("encoder accel: uneven detents at a steady speed never flicker between multipliers", "[ui][encoder]")
{
    EncoderAccel a;
    std::vector<int> v;
    double t = 1000;
    for (int i = 0; i < 200; ++i)
    {
        t += (i % 2 == 0) ? 8.0 : 14.0; // a mean of 11 ms, alternating ±27 %
        v.push_back(a.turn(0, 1, t).scaled(16));
    }
    // Once settled, one multiplier for the rest of the turn.
    for (std::size_t i = 60; i < v.size(); ++i)
        CHECK(v[i] == v[59]);
    CHECK(v[59] > 1);
}

TEST_CASE("encoder accel: a reversal or a pause starts over at x1", "[ui][encoder]")
{
    EncoderAccel a;
    const auto v = spin(a, 60, 5.0, 16);
    REQUIRE(v.back() == 16);
    double t = 1000 + 60 * 5.0;
    // Backing off an overshoot: exact.
    for (int i = 0; i < EncoderAccel::kWarmDetents; ++i)
        CHECK(a.turn(0, -1, t += 5.0).scaled(16) == -1);
    // Spin, pause, nudge: exact.
    spin(a, 60, 5.0, 16, 1, t += 200);
    t += 60 * 5.0;
    CHECK(a.turn(0, 1, t + EncoderAccel::kIdleUs / 1000.0 + 1).scaled(16) == 1);
}

TEST_CASE("encoder accel: slowing down gives the precision back on the first slow detent", "[ui][encoder]")
{
    EncoderAccel a;
    REQUIRE(spin(a, 120, 5.0, 64).back() == 64);
    double t = 1000 + 120 * 5.0;
    // A detent at 60 ms (no pause, same direction): exact at once, and stays exact.
    for (int i = 0; i < 10; ++i)
        CHECK(a.turn(0, 1, t += 60).scaled(64) == 1);
    // From a quick turn (×4) slowing to a deliberate pace: ×1 on the first slower detent.
    a.reset();
    t = 5000;
    for (int i = 0; i < 60; ++i)
        a.turn(0, 1, t += 18);
    REQUIRE(a.turn(0, 1, t += 18).scaled(64) == 4);
    CHECK(a.turn(0, 1, t += 90).scaled(64) == 1);
}

TEST_CASE("encoder accel: the six encoders keep separate runs", "[ui][encoder]")
{
    EncoderAccel a;
    spin(a, 60, 5.0, 16, 1, 1000, 0);
    CHECK(a.turn(1, 1, 1000 + 60 * 5.0).scaled(16) == 1);
    CHECK(a.turn(-1, 1, 0).scaled(16) == 1); // out of range: untouched ×1
    CHECK(a.turn(6, -2, 0).scaled(16) == -2);
}

TEST_CASE("encoder accel: the cap follows the column's span", "[ui][encoder]")
{
    CHECK(EncoderAccel::maxMultiplier(0) == 1);
    CHECK(EncoderAccel::maxMultiplier(24) == 1); // PITCH ±12 and smaller: never
    CHECK(EncoderAccel::maxMultiplier(47) == 1);
    CHECK(EncoderAccel::maxMultiplier(48) == 2);  // PITCH ±24
    CHECK(EncoderAccel::maxMultiplier(127) == 4); // VOLUME 0–127
    CHECK(EncoderAccel::maxMultiplier(280) == 8); // BPM 20–300
    CHECK(EncoderAccel::maxMultiplier(384) == 16);
    CHECK(EncoderAccel::maxMultiplier(1536) == 64);
    CHECK(EncoderAccel::maxMultiplier(1LL << 40) == 64);

    const auto& r = core::builtinParams();
    CHECK(EncoderAccel::stepsOf(*r.find("track.volume"), false) == 127);
    CHECK(EncoderAccel::stepsOf(*r.find("track.volume"), true) == 0); // SHIFT: always exact
    CHECK(EncoderAccel::stepsOf(*r.find("track.triggerMode"), false) == 0);
    CHECK(EncoderAccel::stepsOf(*r.find("playback.timeStretch"), false) == 0);
    CHECK(EncoderAccel::stepsOf(*r.find("playback.reverse"), false) == 0);
}

TEST_CASE("param step: no float drift, the same detents land on the same value", "[params][encoder]")
{
    const auto& pitch = *core::builtinParams().find("playback.pitch"); // fine 0.1
    // 0.1 a detent, 173 times up then back: exactly the start.
    float v = 0;
    for (int i = 0; i < 173; ++i)
        v = pitch.step(v, 1, true);
    CHECK(sameBits(v, pitch.step(0, 173, true)));
    for (int i = 0; i < 173; ++i)
        v = pitch.step(v, -1, true);
    CHECK(sameBits(v, 0.0f));
    // Any path to +12.3 is the one float the format reads as 12.3.
    float a = 0, b = 0;
    for (int i = 0; i < 123; ++i)
        a = pitch.step(a, 1, true);
    b = pitch.step(pitch.step(b, 12, false), 3, true);
    CHECK(sameBits(a, b));
    char buf[16];
    buf[pitch.formatValue(a, buf, sizeof buf)] = 0;
    CHECK(std::string(buf) == "+12.3");
    // A value off the grid stays off it (only rounding error is absorbed).
    CHECK(pitch.step(0.25f, 1, false) == 1.25f);
    // A fractional lower bound must also cancel exactly when the grid reaches zero.
    auto fractional = pitch;
    fractional.minValue = -0.3f;
    CHECK(sameBits(fractional.step(0.1f, -1, true), 0.0f));
}

namespace
{
void panelTurn(uitest::Harness& h, int knob, int d, double t)
{
    h.shell->dispatch(
        {io::ControlEvent::Kind::Turn, io::knob(knob), static_cast<std::int16_t>(d), io::kSourcePanel, t});
}

bool logged(const uitest::Harness& h, std::string_view text)
{
    for (const auto& l : h.log)
        if (l.find(text) != std::string::npos)
            return true;
    return false;
}
} // namespace

TEST_CASE("shell: a fast panel encoder accelerates the column, slow stays exact", "[ui][shell][encoder]")
{
    uitest::Harness h;
    REQUIRE(h.shell->boundColumn(0)->id == "track.volume");
    // Slow: one step a detent.
    double t = h.now;
    for (int i = 0; i < 10; ++i)
        panelTurn(h, 1, -1, t += 150);
    CHECK(h.value("track.volume") == 90.0f);
    // Fast: well over one step a detent, capped at ×4 for 0–127, and the log says so.
    t += 500;
    for (int i = 0; i < 20; ++i)
        panelTurn(h, 1, -1, t += 6);
    CHECK(h.value("track.volume") < 90.0f - 20.0f);
    CHECK(h.value("track.volume") >= 90.0f - 20.0f * 4);
    CHECK(logged(h, "accel -4"));
    // The same turn from the keyboard / desktop panel: one step a detent.
    const float before = h.value("track.volume");
    for (int i = 0; i < 20; ++i)
    {
        h.now = t += 6;
        h.turn(io::knob(1), 1);
    }
    CHECK(h.value("track.volume") == before + 20.0f);
}

TEST_CASE("shell: enums and SHIFT never accelerate", "[ui][shell][encoder]")
{
    uitest::Harness h;
    double t = h.now;
    for (int i = 0; i < 30; ++i)
        panelTurn(h, 6, i % 2 == 0 ? 1 : -1, t += 5); // TRIGGER MODE: ON/OFF entry by entry
    CHECK(h.value("track.triggerMode") == 0.0f);
    for (int i = 0; i < 30; ++i)
        panelTurn(h, 6, 1, t += 5);
    CHECK_FALSE(logged(h, "accel"));
    h.press(io::Control::Shift);
    const float start = h.value("playback.pitch");
    for (int i = 0; i < 30; ++i)
        panelTurn(h, 3, 1, t += 5);
    CHECK(sameBits(h.value("playback.pitch"), core::builtinParams().find("playback.pitch")->step(start, 30, true)));
}

TEST_CASE("param step: every catalogue parameter steps without drift", "[params][encoder]")
{
    // From the default, N detents one at a time land on the same float as N at once (acceleration only changes how
    // many detents a turn carries, never where they land), and back down returns to the default exactly.
    const auto& r = core::builtinParams();
    int checked = 0;
    for (std::size_t i = 0; i < r.size(); ++i)
    {
        const auto& s = r[core::ParamId{static_cast<std::uint16_t>(i)}];
        for (bool fine : {false, true})
            for (int n : {1, 7, 37, 300})
            {
                float one = s.defaultValue;
                for (int k = 0; k < n; ++k)
                    one = s.step(one, 1, fine);
                INFO(s.id << (fine ? " fine " : " coarse ") << n);
                float back = one;
                for (int k = 0; k < n; ++k)
                    back = s.step(back, -1, fine);
                // A default between grid points (reverb TIME: 0.5 on a 1/101 grid) has no grid to land on: there the
                // paths may differ by float rounding, never by a step.
                const double grid = std::min(s.fineStep, s.coarseStep);
                const double q = (static_cast<double>(s.defaultValue) - static_cast<double>(s.minValue)) / grid;
                if (s.type == core::ParamType::Continuous && std::fabs(q - std::round(q)) > 0.01)
                {
                    const double off = std::fabs(static_cast<double>(one - s.step(s.defaultValue, n, fine)));
                    CHECK(off < 1e-4 * grid);
                    ++checked;
                    continue;
                }
                CHECK(sameBits(one, s.step(s.defaultValue, n, fine)));
                if (one < s.maxValue) // not clamped on the way up: the way down retraces it
                    CHECK(sameBits(back, s.step(one, -n, fine)));
                ++checked;
            }
    }
    CHECK(checked > 100);
}
