// PatternData.Steps / ArrangementData.Sections codecs (research docs/project-format.md "PatternData.Steps",
// "ArrangementData.Sections"; spec sp16-interop.md "Step matrix").
#include "services/interop/sp16/StepMatrix.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace services::interop::sp16;

TEST_CASE("prj steps: the lock word layout [vct:2 | value:22 | idx:8] and the no-lock sentinel", "[interop]")
{
    const auto none = LockWord::unpack(0x3f800000u | 6u);
    CHECK(none.empty);
    CHECK(none.value22 == -32768);
    CHECK(none.idx == 6);
    CHECK(none.vct == 0);
    // The research's cross-check: slot-6 locks 8/127 … 11/127 pack to upper fields 8..11 × 16513.
    for (int k = 8; k <= 11; ++k)
    {
        const std::uint32_t w = static_cast<std::uint32_t>(k * kK127) << 8 | 6u;
        const auto l = LockWord::unpack(w);
        CHECK_FALSE(l.empty);
        CHECK(l.value22 == k * kK127);
        CHECK(l.pack() == w);
    }
    LockWord neg;
    neg.value22 = -1200;
    neg.idx = 4;
    neg.vct = 2;
    const auto back = LockWord::unpack(neg.pack());
    CHECK(back.value22 == -1200);
    CHECK(back.vct == 2);
    CHECK(back.idx == 4);
    CHECK_FALSE(back.empty);
    CHECK(kValueFull == 127 * kK127);
}

TEST_CASE("prj steps: 64 x 16 x 152-byte step-major records round-trip at level 6", "[interop]")
{
    std::vector<StepRecord> recs(1024, StepRecord::emptyRecord());
    auto& r = recs[static_cast<std::size_t>(5 * 16 + 3)]; // step 6, track 4
    r.trigger = 100;
    r.value2 = 99;
    r.typeFlags = 11;
    r.slots[4] = LockWord::unpack(static_cast<std::uint32_t>(-1200 & 0x3fffff) << 8 | 4u);
    const auto raw = encodeStepsRaw(recs);
    REQUIRE(raw.size() == kStepsBytes);
    // offset = step × 2432 + track × 152; trigger LE at +0, type at +8, slot 4 at +12 + 16
    const std::size_t at = 5 * 2432 + 3 * 152;
    CHECK(static_cast<int>(raw[at]) == 100);
    CHECK(static_cast<int>(raw[at + 8]) == 11);
    CHECK(static_cast<int>(raw[at + 12 + 16]) == 4);
    const auto blob = encodeSteps(recs);
    CHECK(static_cast<int>(blob[0]) == 0x78);
    CHECK(static_cast<int>(blob[1]) == 0x9c);
    auto back = decodeSteps(blob);
    REQUIRE(back);
    CHECK(back.value() == recs);
    CHECK_FALSE(decodeStepsRaw(std::span(raw).first(raw.size() - 1)));
    CHECK_FALSE(decodeSteps(deflateZlib(std::span(raw).first(1000), 6)));
    std::vector<std::byte> bigger(raw);
    bigger.push_back(std::byte {0});
    CHECK_FALSE(decodeSteps(deflateZlib(bigger, 6))); // inflates beyond 155,648 bytes
}

TEST_CASE("prj sections: 999 default records; other sizes decode but are not the default", "[interop]")
{
    std::vector<SectionRecord> recs(999);
    CHECK(std::all_of(recs.begin(), recs.end(), [](const SectionRecord& s) { return s.isDefault(); }));
    const auto blob = encodeSections(recs);
    auto back = decodeSections(blob);
    REQUIRE(back);
    CHECK(back.value() == recs);
    recs[3] = {7, 8};
    auto changed = decodeSections(encodeSections(recs));
    REQUIRE(changed);
    CHECK_FALSE(changed.value()[3].isDefault());
    CHECK_FALSE(decodeSections(deflateZlib(std::vector<std::byte>(13), 6)));
}
