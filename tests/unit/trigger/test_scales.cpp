// core::scales (PAD-010…013, PAD-020) and the velocity curves (PAD-002 / UI-114). Pad repeat (PAD-030) is the strip's:
// tests/unit/ribbon (ribbon.repeat).
#include "TriggerRig.h"

#include "core/model/enums/EnumStrings.h"
#include "core/model/enums/ValueLists.h"
#include "core/params/ParamRegistry.h"
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

TEST_CASE("trigger.scales: the 22 masks are the model's scale intervals, Chromatic first (PAD-010)", "[trigger]")
{
    REQUIRE(sc::kScales == 22);
    REQUIRE(core::model::enumCount<core::model::ScaleType>() == 22);
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

TEST_CASE("trigger.scales: the manual's SET SCALE list, in its order, names and notes", "[trigger]")
{
    namespace m = core::model;
    struct Want
    {
        const char* label;
        std::vector<int> notes;
    };
    // Addendum "Selection of scale" No. 1–22; the notes are the usual ones (the manual names the scales only).
    const Want want[] = {{"CHROMATIC", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
                         {"IONIAN", {0, 2, 4, 5, 7, 9, 11}},
                         {"DORIAN", {0, 2, 3, 5, 7, 9, 10}},
                         {"PHRYGIAN", {0, 1, 3, 5, 7, 8, 10}},
                         {"LYDIAN", {0, 2, 4, 6, 7, 9, 11}},
                         {"MIXOLYDIAN", {0, 2, 4, 5, 7, 9, 10}},
                         {"AEOLIAN", {0, 2, 3, 5, 7, 8, 10}},
                         {"LOCRIAN", {0, 1, 3, 5, 6, 8, 10}},
                         {"MAJOR PENTATONIC", {0, 2, 4, 7, 9}},
                         {"MINOR PENTATONIC", {0, 3, 5, 7, 10}},
                         {"WHOLE TONE", {0, 2, 4, 6, 8, 10}},
                         {"DIMINISH", {0, 2, 3, 5, 6, 8, 9, 11}},
                         {"COMBINATION DIMINISH", {0, 1, 3, 4, 6, 7, 9, 10}},
                         {"ALTERED", {0, 1, 3, 4, 6, 8, 10}},
                         {"MAJOR BLUES", {0, 2, 3, 4, 7, 9}},
                         {"MINOR BLUES", {0, 3, 5, 6, 7, 10}},
                         {"RAGA BHAIRAV", {0, 1, 4, 5, 7, 8, 11}},
                         {"RAGA GAMANASRAMA", {0, 1, 4, 6, 7, 9, 11}},
                         {"RAGA TODI", {0, 1, 3, 6, 7, 8, 11}},
                         {"HAWAIIAN", {0, 2, 3, 5, 7, 9, 11}},
                         {"RYUKYU", {0, 4, 5, 7, 11}},
                         {"JAPANESE MIYAKOBUSHI", {0, 1, 5, 7, 8}}};
    REQUIRE(std::size(want) == static_cast<std::size_t>(sc::kScales));
    const auto* type = core::builtinParams().find("scale.type");
    const auto* midiType = core::builtinParams().find("scaleMidi.type");
    REQUIRE(type != nullptr);
    REQUIRE(midiType != nullptr);
    REQUIRE(type->enumLabels.size() == std::size(want));
    CHECK(type->maxValue == 21.0f);
    CHECK(midiType->maxValue == 21.0f);
    for (int i = 0; i < sc::kScales; ++i)
    {
        const auto& w = want[static_cast<std::size_t>(i)];
        INFO(w.label);
        const auto st = static_cast<m::ScaleType>(i);
        CHECK(m::displayName(st) == w.label);
        CHECK(type->enumLabels[static_cast<std::size_t>(i)] == w.label);
        CHECK(midiType->enumLabels[static_cast<std::size_t>(i)] == w.label);
        std::string stored(w.label); // stored name = the manual's name without spaces, e.g. "MajorPentatonic"
        CHECK(m::enumFromStored<m::ScaleType>(m::storedName(st)) == st);
        std::uint16_t mask = 0;
        for (const int n : w.notes)
            mask = static_cast<std::uint16_t>(mask | 1u << n);
        CHECK(sc::scaleMask(i) == mask);
    }
    CHECK(m::storedName(m::ScaleType::MajorPentatonic) == "MajorPentatonic");
    CHECK(m::storedName(m::ScaleType::JapaneseMiyakobushi) == "JapaneseMiyakobushi");
    CHECK(m::enumFromStored<m::ScaleType>("CHROMA") == m::ScaleType::Chromatic); // the 1.60 spelling
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
    CHECK(cases == 22 * 25 * 5 * 16);
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

TEST_CASE("trigger.scales: degreeOf inverts degreeSemitones; a note outside the scale has no degree", "[trigger]")
{
    for (int s = 0; s < sc::kScales; ++s)
    {
        const auto mask = sc::scaleMask(s);
        for (int d = -40; d <= 40; ++d)
            CHECK(sc::degreeOf(sc::degreeSemitones(d, mask), mask) == d);
        for (int semis = -30; semis <= 30; ++semis)
        {
            const int r = ((semis % 12) + 12) % 12;
            CHECK((sc::degreeOf(semis, mask) == sc::kNoDegree) == (((mask >> r) & 1u) == 0));
        }
    }
}
