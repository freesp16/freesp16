// core::scales (PAD-010…013, PAD-020) and the velocity curves (PAD-002 / UI-114). Pad repeat (PAD-030) is the strip's:
// tests/unit/ribbon (ribbon.repeat).
#include "TriggerRig.h"

#include "core/model/enums/ValueLists.h"
#include "core/util/scales/Scales.h"
#include "ui/input/pads/VelocityCurve.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace sc = core::scales;

namespace
{
// Independent reference: the scale's semitones laid out over enough octaves both ways, degree 0 = root.
int reference(int scale, int pad, int keyShift, int window)
{
    const auto iv = core::model::scaleIntervals(static_cast<core::model::ScaleType>(scale));
    std::vector<int> below, above;
    for (int oct = 0; oct < 12; ++oct)
        for (const auto i : iv)
            above.push_back(oct * 12 + i);
    for (int oct = 1; oct < 13; ++oct)
        for (auto it = iv.rbegin(); it != iv.rend(); ++it)
            below.push_back(-oct * 12 + *it); // degree −1, −2, …
    const int d = pad + 16 * window;
    const int semis = d >= 0 ? above[static_cast<std::size_t>(d)] : below[static_cast<std::size_t>(-d - 1)];
    return semis + keyShift;
}
} // namespace

TEST_CASE("trigger.scales: the 14 masks are the model's scale intervals, Chromatic first (PAD-010)", "[trigger]")
{
    for (int s = 0; s < sc::kScales; ++s)
    {
        std::uint16_t mask = 0;
        for (const auto i : core::model::scaleIntervals(static_cast<core::model::ScaleType>(s)))
            mask = static_cast<std::uint16_t>(mask | 1u << i);
        CHECK(sc::kScaleMasks[static_cast<std::size_t>(s)] == mask);
    }
    CHECK(sc::kScaleMasks[0] == 0xFFF);
    CHECK(sc::scaleMask(99) == 0xFFF);
}

TEST_CASE("trigger.scales: every scale x key shift x window matches the reference table (PAD-011…013)", "[trigger]")
{
    int cases = 0;
    for (int s = 0; s < sc::kScales; ++s)
        for (int k = sc::kMinKeyShift; k <= sc::kMaxKeyShift; ++k)
            for (int w = sc::kMinWindow; w <= sc::kMaxWindow; ++w)
            {
                const auto table = sc::padTable(sc::scaleMask(s), k, w);
                for (int pad = 0; pad < 16; ++pad, ++cases)
                {
                    const int want = reference(s, pad, k, w);
                    if (sc::padSemitones(pad, sc::scaleMask(s), k, w) != want
                        || table[static_cast<std::size_t>(pad)] != want)
                    {
                        INFO("scale " << s << " key " << k << " window " << w << " pad " << pad);
                        CHECK(sc::padSemitones(pad, sc::scaleMask(s), k, w) == want);
                    }
                }
            }
    CHECK(cases == 14 * 25 * 5 * 16);
    // Pad 1 (bottom-left) is the root = the sample's pitch; chromatic rises a semitone per pad (M p.13).
    for (int pad = 0; pad < 16; ++pad)
        CHECK(sc::padSemitones(pad, sc::kScaleMasks[0], 0, 0) == pad);
    CHECK(sc::padSemitones(7, sc::kScaleMasks[1], 0, 0) == 12);  // major: pad 8 = the octave
    CHECK(sc::padSemitones(0, sc::kScaleMasks[1], 3, 0) == 3);   // key shift moves the root
    CHECK(sc::padSemitones(0, sc::kScaleMasks[0], 0, 1) == 16);  // next window: 16 degrees up
    CHECK(sc::padSemitones(15, sc::kScaleMasks[0], 0, -1) == -1);
    // Out-of-range inputs are clamped, never undefined.
    CHECK(sc::padSemitones(99, sc::kScaleMasks[0], 99, 99) == sc::padSemitones(15, sc::kScaleMasks[0], 12, 2));
}

TEST_CASE("trigger.scales: root pads and MIDI-track notes (PAD-013, PAD-020)", "[trigger]")
{
    CHECK(sc::isRootPad(0, sc::kScaleMasks[1], 0));
    CHECK(sc::isRootPad(7, sc::kScaleMasks[1], 0));
    CHECK(sc::isRootPad(14, sc::kScaleMasks[1], 0));
    CHECK_FALSE(sc::isRootPad(1, sc::kScaleMasks[1], 0));
    CHECK(sc::isRootPad(0, sc::kScaleMasks[0], 0));
    CHECK(sc::isRootPad(12, sc::kScaleMasks[0], 0));
    CHECK(sc::isRootPad(8, sc::kScaleMasks[0], 1)); // degree 24
    for (int s = 0; s < sc::kScales; ++s)
        for (int w = sc::kMinWindow; w <= sc::kMaxWindow; ++w)
            for (int pad = 0; pad < 16; ++pad)
            {
                const int want = reference(s, pad, 0, w) + 60;
                CHECK(sc::midiPadNote(pad, sc::scaleMask(s), 60, w) == (want < 0 ? 0 : (want > 127 ? 127 : want)));
            }
    CHECK(sc::midiPadNote(15, sc::kScaleMasks[0], 127, 2) == 127);
    CHECK(sc::midiPadNote(0, sc::kScaleMasks[0], 0, -2) == 0);
}

TEST_CASE("trigger.velocity: curves are monotonic, LINEAR is the identity, FIXED is fixed (PAD-002)", "[trigger]")
{
    using ui::input::pads::VelocityCurve;
    using ui::input::pads::applyVelocityCurve;
    for (int raw = 1; raw <= 127; ++raw)
        CHECK(applyVelocityCurve(raw, VelocityCurve::Linear, 0) == raw);
    for (int c = 0; c < 7; ++c)
    {
        const auto curve = static_cast<VelocityCurve>(c);
        int prev = 0;
        for (int raw = 1; raw <= 127; ++raw)
        {
            const int v = applyVelocityCurve(raw, curve, 0);
            CHECK(v >= 1);
            CHECK(v <= 127);
            CHECK(v >= prev);
            prev = v;
        }
        CHECK(applyVelocityCurve(127, curve, 0) == 127);
        CHECK(applyVelocityCurve(0, curve, 0) == 0); // no hit
    }
    // Harder curves need harder hits, softer ones less (at mid velocity, strictly ordered).
    for (int c = 1; c < 7; ++c)
        CHECK(applyVelocityCurve(64, static_cast<VelocityCurve>(c), 0)
              > applyVelocityCurve(64, static_cast<VelocityCurve>(c - 1), 0));
    CHECK(applyVelocityCurve(5, VelocityCurve::Fixed, 100) == 100);
    CHECK(applyVelocityCurve(127, VelocityCurve::Fixed, 0) == 1);
    CHECK(applyVelocityCurve(0, VelocityCurve::Fixed, 100) == 0);
}
