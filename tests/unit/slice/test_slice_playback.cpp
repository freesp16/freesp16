// Slice playback through the trigger router and the track sources (SLC-003/004/005, PLY-042): SLICE pad
// mode pads play the selected track's slices (a slice-number lock, the pad adapter), sample-accurately and bit for bit
// at pitch 0 with a neutral envelope; N = 64 pages with the window; moved points move the region; sequencer steps
// with a slice lock and repeat hits play slices; pads without a slice are silent; a retrigger inside a slice hands the
// old slice to the 2 ms tail.
#include "tests/unit/trigger/TriggerRig.h"

#include "core/commands/slice/SlicePoints.h"
#include "engine/voice/SliceMapPads.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace trigtest;
using engine::TriggerNow;

namespace
{
constexpr int kSliceFrames = 640; // 10 blocks: every slice ends at a block end
constexpr int kFrames = 16 * kSliceFrames;

std::vector<float> noise(int frames, std::uint32_t seed = 12345)
{
    std::vector<float> v(static_cast<std::size_t>(frames));
    for (auto& x : v)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        x = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
    }
    return v;
}

struct SliceRig
{
    std::unique_ptr<Rig> r = makeRig();
    engine::voice::SlicePadLocks pads;
    core::slice::SliceTable table {};

    explicit SliceRig(int track = 3)
    {
        r->router.setSlicePads(&pads);
        // The envelope out of the way: attack 0, hold INFINITY, level 100 = 0 dB, velocity off — unity from the first
        // frame. (BYPASS itself is not neutral: ENV-009 ramps it in over 2 ms at every start.)
        engine::modulation::AmpEnvParams env;
        env.velocity = 100;
        env.velocitySensitive = false;
        r->env(track, env);
        r->router.setPadParam(PadField::Selected, static_cast<float>(track));
        r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Slice));
    }
    void install(int track, const core::model::SliceSettings& s, std::int64_t frames)
    {
        table = core::slice::buildTable(s, frames);
        r->src[static_cast<std::size_t>(track)].setSliceMap(&table);
        pads.setMap(track, &table);
    }
    void hitPad(int pad) { REQUIRE(r->router.pushLive(TriggerNow {static_cast<std::uint8_t>(pad), 127, 60, static_cast<std::int8_t>(pad)})); }
};
} // namespace

TEST_CASE("slice.playback: 16 pads in SLICE mode play the sample's equal sixteenths bit for bit", "[slice]")
{
    const auto audio = noise(kFrames);
    TestBuffer buf(audio);
    SliceRig s;
    s.r->src[3].setSample(buf.get());
    s.install(3, core::model::SliceSettings {}, kFrames); // a new sample: 16 equal slices
    // Pads in reverse order, each hit when the previous slice has just ended.
    for (int n = 0; n < 16; ++n)
    {
        s.hitPad(15 - n);
        s.r->blocks(kSliceFrames / kBlockFrames);
    }
    s.r->blocks(2);
    const auto& out = s.r->history[3];
    REQUIRE(out.size() >= static_cast<std::size_t>(kFrames));
    int mismatches = 0;
    for (int n = 0; n < 16; ++n)
        for (int i = 0; i < kSliceFrames; ++i)
            if (out[static_cast<std::size_t>(n * kSliceFrames + i)] != audio[static_cast<std::size_t>((15 - n) * kSliceFrames + i)])
                ++mismatches;
    CHECK(mismatches == 0);
    for (std::size_t i = kFrames; i < out.size(); ++i)
        REQUIRE(out[i] == 0.0f); // slice 1 played last: nothing after its end
    for (int t = 0; t < kTracks; ++t)
        if (t != 3)
            CHECK(s.r->src[static_cast<std::size_t>(t)].voice().active() == false); // only the selected track
}

TEST_CASE("slice.playback: without a table the pads play 16 equal slices; a hit selects no other track", "[slice]")
{
    const auto audio = noise(kFrames, 7);
    TestBuffer buf(audio);
    SliceRig s;
    s.r->src[3].setSample(buf.get());
    s.hitPad(4);
    s.r->blocks(12);
    const auto& out = s.r->history[3];
    for (int i = 0; i < kSliceFrames; ++i)
        REQUIRE(out[static_cast<std::size_t>(i)] == audio[static_cast<std::size_t>(4 * kSliceFrames + i)]);
    CHECK(out[static_cast<std::size_t>(kSliceFrames + 5)] == 0.0f); // the region ends at slice 6's start
    CHECK_FALSE(s.r->src[4].voice().active()); // the pad's own track stays silent
}

TEST_CASE("slice.playback: N = 64 pages with the window; pads beyond an 8-slice grid are silent", "[slice]")
{
    constexpr int frames = 64 * 128;
    const auto audio = noise(frames, 99);
    TestBuffer buf(audio);
    SliceRig s;
    s.r->src[3].setSample(buf.get());
    core::model::SliceSettings set;
    set.gridCount = 64;
    set.windowOffset = 3; // pads = slices 49–64
    s.install(3, set, frames);
    s.hitPad(0);
    s.r->blocks(3);
    for (int i = 0; i < 128; ++i)
        REQUIRE(s.r->history[3][static_cast<std::size_t>(i)] == audio[static_cast<std::size_t>(48 * 128 + i)]);
    CHECK(s.r->history[3][130] == 0.0f);

    set.gridCount = 8;
    set.windowOffset = 0;
    s.install(3, set, frames);
    s.hitPad(9);
    s.r->block();
    CHECK(s.r->router.blockEvents().empty()); // no slice on pad 10: nothing routed
    CHECK_FALSE(s.r->src[3].voice().active());
}

TEST_CASE("slice.playback: a moved point moves the slice's region; loop runs inside the slice (SLC-002/004)", "[slice]")
{
    const auto audio = noise(kFrames, 3);
    TestBuffer buf(audio);
    SliceRig s;
    s.r->src[3].setSample(buf.get());
    core::model::SliceSettings set; // 16 × 640
    set.grids = {2, 2 * kSliceFrames + 64}; // slice 3 starts 64 frames later; slice 2 is 64 frames longer
    s.install(3, set, kFrames);
    s.hitPad(1);
    s.r->blocks(12);
    const auto& out = s.r->history[3];
    for (int i = 0; i < kSliceFrames + 64; ++i)
        REQUIRE(out[static_cast<std::size_t>(i)] == audio[static_cast<std::size_t>(kSliceFrames + i)]);
    CHECK(out[static_cast<std::size_t>(kSliceFrames + 64)] == 0.0f);

    // LOOP on: the slice loops (its own start → its end), never reading the next slice.
    engine::voice::PlaybackParams p;
    p.region.loop = true;
    s.r->src[3].setPlayback(p);
    s.hitPad(5);
    s.r->blocks(30);
    CHECK(s.r->src[3].voice().mainActive());
    const auto pos = s.r->src[3].voice().position();
    CHECK(pos >= 5 * kSliceFrames);
    CHECK(pos < 6 * kSliceFrames);
    CHECK(s.r->src[3].playingSlice() == 5);
    // A knob change of the region while the slice sounds keeps the slice's region.
    p.region.start = 0;
    p.region.length = 100;
    s.r->src[3].setPlayback(p);
    s.r->blocks(3);
    CHECK(s.r->src[3].voice().position() >= 5 * kSliceFrames);
}

TEST_CASE("slice.playback: a sequencer step's slice lock plays that slice at its frame; Hold keeps it", "[slice]")
{
    const auto audio = noise(kFrames, 5);
    TestBuffer buf(audio);
    SliceRig s;
    s.r->router.setPadParam(PadField::Mode, static_cast<float>(PadMode::Play)); // SLICE mode is not needed to replay
    s.r->src[3].setSample(buf.get());
    float values[35] {};
    std::uint8_t change[35] {};
    values[22] = 9.0f;
    LockSet locks {std::uint64_t {1} << 22, values, change};
    auto e = Rig::event(3, TriggerKind::Full, 17);
    e.locks = &locks;
    REQUIRE(s.r->router.push(e));
    s.r->blocks(12);
    const auto& out = s.r->history[3];
    for (int i = 0; i < 17; ++i)
        REQUIRE(out[static_cast<std::size_t>(i)] == 0.0f);
    for (int i = 0; i < kSliceFrames; ++i)
        REQUIRE(out[static_cast<std::size_t>(17 + i)] == audio[static_cast<std::size_t>(9 * kSliceFrames + i)]);

    // Revert: the next unlocked step plays the whole region again.
    REQUIRE(s.r->router.push(Rig::event(3, TriggerKind::Full, 0)));
    s.r->block();
    const std::size_t b = out.size() - kBlockFrames;
    CHECK(out[b] == audio[0]);
    // Hold: the slice stays for the following unlocked steps (SEQ-021).
    change[22] = 1;
    values[22] = 2.0f;
    REQUIRE(s.r->router.push(e));
    s.r->block();
    REQUIRE(s.r->router.push(Rig::event(3, TriggerKind::Full, 0)));
    s.r->block();
    CHECK(s.r->src[3].playingSlice() == 2);
    CHECK(s.r->src[3].voice().position() == 2 * kSliceFrames + kBlockFrames);
}

TEST_CASE("slice.playback: the live hit carries the slice lock the recorder sees; it replays the same slice", "[slice]")
{
    struct Sink final : LiveHitSink
    {
        int hits = 0;
        float slice = -1.0f;
        void onLiveHit(const TriggerEvent& e, bool) noexcept override
        {
            ++hits;
            if (e.locks != nullptr)
                e.locks->get(22, slice);
        }
    } sink;
    const auto audio = noise(kFrames, 11);
    TestBuffer buf(audio);
    SliceRig s;
    s.r->router.setLiveHitSink(&sink);
    s.r->src[3].setSample(buf.get());
    s.install(3, core::model::SliceSettings {}, kFrames);
    s.hitPad(12);
    s.r->block();
    CHECK(sink.hits == 1);
    CHECK(sink.slice == 12.0f);
    s.r->blocks(12);
    // Replay as a recorded step: the slice-number lock the recorder wrote.
    float values[35] {};
    values[22] = sink.slice;
    const LockSet locks {std::uint64_t {1} << 22, values, nullptr};
    auto e = Rig::event(3, TriggerKind::Full, 0);
    e.locks = &locks;
    const std::size_t from = s.r->history[3].size();
    REQUIRE(s.r->router.push(e));
    s.r->blocks(11);
    for (int i = 0; i < kSliceFrames; ++i)
        REQUIRE(s.r->history[3][from + static_cast<std::size_t>(i)] == s.r->history[3][static_cast<std::size_t>(i)]);
}

TEST_CASE("slice.playback: a retrigger inside a slice hands the old slice to the 2 ms tail (PLY-042)", "[slice]")
{
    TestBuffer buf(dc(kFrames, 0.5f));
    SliceRig s;
    s.r->src[3].setSample(buf.get());
    s.hitPad(0);
    s.r->blocks(2);
    s.hitPad(8);
    s.r->block();
    // The new slice starts at frame 0 at full level; the old one fades out on top over 88 frames (sum ≤ 2 × 0.5).
    const auto& out = s.r->history[3];
    const std::size_t b = 2 * kBlockFrames;
    CHECK(out[b] > 0.5f);
    CHECK(out[b + 63] > 0.5f);
    s.r->block();
    CHECK(out[b + 64 + 40] == 0.5f); // tail gone after 2 ms (88 frames)
    CHECK(s.r->src[3].playingSlice() == 8);
}

TEST_CASE("slice.playback: REPEAT hits replay the held pad's slice", "[slice]")
{
    const auto audio = noise(kFrames, 21);
    TestBuffer buf(audio);
    SliceRig s;
    s.r->src[3].setSample(buf.get());
    s.hitPad(6);
    s.r->blocks(12);
    const std::size_t from = s.r->history[3].size();
    REQUIRE(s.r->router.push(Rig::event(3, TriggerKind::Full, 0, TriggerSource::Repeat)));
    s.r->block();
    CHECK(s.r->history[3][from + 1] == audio[6 * kSliceFrames + 1]);
}
