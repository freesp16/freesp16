// Song mode on the audio thread (ARR-004…008): the SectionSequencer against the scene switcher's boundary contract on
// a transport model — row changes on the exact pattern-end frames (mixed lengths 8/16/64, repeats, fixed and "--"
// BPMs, every PATTERN QUANTIZE unit), end stop and loop, manual switches refused, SPP = the start table at every row
// start, startFrom / locate, plan swaps while playing, 999 rows.
#include "tests/unit/arrangement/ArrTestUtil.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace arrtest;

namespace
{
// The acceptance song: five rows, pattern lengths 16 / 8 / 64 / 16 / 8, repeats, fixed and "--" BPMs.
const std::vector<Row> kSong {
    {0, 0, 16, 2, 0.0f},   // 1.1 ×2  --   (120 BPM in force)
    {0, 1, 8, 3, 140.0f},  // 1.2 ×3  140
    {1, 0, 64, 1, 0.0f},   // 2.1 ×1  --   (stays 140)
    {0, 2, 16, 1, 90.0f},  // 1.3 ×1  90
    {1, 1, 8, 2, 0.0f},    // 2.2 ×2  --   (stays 90)
};

std::vector<std::pair<std::int64_t, int>> rowSwitches(const SongRig& rig, const std::vector<Row>& rows)
{
    // The switches the rig saw, as (frame, row) — rows differ from their predecessor, so every row change switches.
    std::vector<std::pair<std::int64_t, int>> out;
    std::size_t row = 1;
    for (const auto& s : rig.switches)
    {
        if (row >= rows.size())
            break;
        if (s.scene == rows[row].scene && s.pattern == rows[row].pattern)
            out.emplace_back(s.frame, static_cast<int>(row++));
    }
    return out;
}

void playFromTop(SongRig& rig)
{
    rig.run(4); // stopped: the first row is cued (its scene / pattern and BPM)
    rig.origin = rig.clock.block * engine::kBlockFrames;
    rig.clock.command({engine::TransportCmd::Play, 0.0});
}
} // namespace

TEST_CASE("arr.sequencer: a 5-row song changes rows on the exact pattern-end frames for every quantize unit",
          "[arrangement]")
{
    const auto unit = GENERATE(es::SwitchUnit::Pattern, es::SwitchUnit::Bar, es::SwitchUnit::Step);
    SongRig rig;
    rig.sw.setUnit(unit);
    setSong(rig, kSong);
    playFromTop(rig);
    const auto want = expectRows(kSong, 120.0);
    REQUIRE(want.rows.size() == 4);
    rig.run(want.endFrame / 64 + 40);
    // The model: 8 quarters at 120 BPM → frame 176 400; the rest follow from the block tempo steps.
    CHECK(want.rows.front().first == 176400);
    CHECK(rowSwitches(rig, kSong) == want.rows);
    // No switch other than the four row changes and, after the stop, the cue back to row 1 (immediate, block start).
    REQUIRE(rig.switches.size() == 5);
    const std::int64_t stopBlock = want.endFrame / 64 + 1; // the transport stops at the block after the end
    CHECK(rig.switches.back() == Switch {stopBlock * 64, 0, 0});
    CHECK(rig.clock.stops() == 1);
    CHECK_FALSE(rig.seq.active());
    CHECK(rig.seq.playing().row == -1);
    // Fixed BPMs step at the row change (the next block start); "--" keeps the tempo in force.
    std::vector<double> tempos;
    for (const auto& c : rig.clock.commands)
        if (c.kind == engine::TransportCmd::SetTempo && (tempos.empty() || tempos.back() != c.value))
            tempos.push_back(c.value);
    CHECK(tempos == std::vector<double> {140.0, 90.0});
    CHECK(rig.clock.tempo == 90.0);
}

TEST_CASE("arr.sequencer: SPP out equals the start table at every row start and advances by sixteenths", "[arrangement]")
{
    SongRig rig;
    setSong(rig, kSong);
    playFromTop(rig);
    std::int64_t last = -1;
    bool monotonic = true;
    for (int i = 0; i < 20000 && rig.clock.stops() == 0; ++i)
    {
        rig.step();
        if (rig.seq.active())
        {
            const auto pos = rig.seq.songPosition16th();
            monotonic = monotonic && pos >= last && pos - last <= 1;
            last = pos;
        }
    }
    CHECK(monotonic);
    CHECK(last == rig.plan->startPpq16[5] - 1); // the last sixteenth before the end
    REQUIRE(rig.rowStarts.size() == 5);
    for (const auto& [row, pos] : rig.rowStarts)
        CHECK(pos == rig.plan->startPpq16[static_cast<std::size_t>(row)]);
}

TEST_CASE("arr.sequencer: loop plays row 1 again exactly at the end; repeats count passes", "[arrangement]")
{
    SongRig rig;
    rig.seq.setLoop(true);
    const std::vector<Row> rows {{0, 0, 8, 3, 0.0f}, {0, 5, 16, 1, 0.0f}};
    setSong(rig, rows);
    playFromTop(rig);
    rig.run(3000); // > 2 loops (10 quarters each at 120 BPM = 220 500 frames ≈ 3 445 blocks per loop… run 3000 + more)
    rig.run(4000);
    // Row 2 at 3 × 2 quarters = 6 q (132 300 frames), row 1 again at 10 q (220 500), row 2 at 16 q (352 800), …
    REQUIRE(rig.switches.size() >= 3);
    CHECK(rig.switches[0] == Switch {132300, 0, 5});
    CHECK(rig.switches[1] == Switch {220500, 0, 0});
    CHECK(rig.switches[2] == Switch {352800, 0, 5});
    CHECK(rig.clock.stops() == 0);
    CHECK(rig.seq.active());
}

TEST_CASE("arr.sequencer: manual scene/pattern requests are refused while song mode is on", "[arrangement]")
{
    SongRig rig;
    setSong(rig, kSong);
    CHECK_FALSE(rig.seq.refusesSwitch(17.0f)); // the plan is not installed yet (step 5 installs it)
    rig.step();
    CHECK(rig.seq.refusesSwitch(17.0f));
    CHECK_FALSE(rig.seq.refusesSwitch(-1.0f)); // cancel, block, unblock still pass
    CHECK_FALSE(rig.seq.refusesSwitch(-2.0f));
    CHECK(rig.seq.suspendsChain());
    rig.seq.setSongMode(false);
    CHECK_FALSE(rig.seq.refusesSwitch(17.0f));
    CHECK_FALSE(rig.seq.suspendsChain());
    rig.seq.setSongMode(true);
    auto empty = std::make_unique<es::ArrangementPlan>(); // an empty arrangement: song mode does nothing
    rig.installed = empty.get();
    rig.step();
    CHECK_FALSE(rig.seq.refusesSwitch(17.0f));
}

TEST_CASE("arr.sequencer: startFrom cues a row when stopped and jumps at the next pattern end when playing",
          "[arrangement]")
{
    SongRig rig;
    setSong(rig, kSong);
    rig.run(2);
    rig.seq.startFrom(2); // row 3 (2.1, 64 steps)
    rig.run(2);
    CHECK(rig.sw.state().scene == 1);
    CHECK(rig.sw.state().pattern == 0);
    CHECK(rig.seq.songPosition16th() == rig.plan->startPpq16[2]);
    const std::int64_t playAt = rig.clock.block * 64;
    rig.clock.command({engine::TransportCmd::Play, 0.0});
    rig.step();
    CHECK(rig.seq.playing().row == 2);
    CHECK(rig.seq.playing().remaining == 1);
    // While playing: jump to row 1 at the end of the current pass (16 quarters at 120 BPM = 352 800 frames).
    rig.seq.startFrom(0);
    rig.run(6000);
    REQUIRE_FALSE(rig.switches.empty());
    const auto it =
        std::find_if(rig.switches.begin(), rig.switches.end(), [&](const Switch& s) { return s.frame > playAt; });
    REQUIRE(it != rig.switches.end());
    CHECK(*it == Switch {playAt + 352800, 0, 0});
    // The cue returns to row 1 after a stop at the top.
    rig.clock.command({engine::TransportCmd::Stop, 1.0});
    rig.run(3);
    CHECK(rig.seq.playing().cued == 0);
}

TEST_CASE("arr.sequencer: pause holds the row and pass; locate16th cues the row and pass that contain it",
          "[arrangement]")
{
    SongRig rig;
    setSong(rig, kSong);
    playFromTop(rig);
    rig.run(3000); // inside row 2 (starts at 176 400 frames = block 2 756)
    REQUIRE(rig.seq.playing().row == 1);
    const auto rowBefore = rig.seq.playing();
    rig.clock.command({engine::TransportCmd::Stop, 0.0}); // pause
    rig.run(10);
    CHECK(rig.seq.playing().row == rowBefore.row);
    CHECK(rig.seq.playing().remaining == rowBefore.remaining);
    // SPP while stopped (to the top): sixteenth 45 lies in row 2 (starts at 32, 8 per pass), its 2nd pass → cued at
    // that pass's start (40), 2 passes left. Row 3 (64 steps) starts at 56.
    rig.clock.command({engine::TransportCmd::Stop, 1.0});
    rig.run(2);
    rig.seq.locate16th(45);
    rig.run(2);
    CHECK(rig.seq.playing().cued == 1);
    CHECK(rig.seq.songPosition16th() == 40);
    CHECK(rig.sw.state().pattern == 1);
    rig.clock.command({engine::TransportCmd::Play, 0.0});
    rig.step();
    CHECK(rig.seq.playing().row == 1);
    CHECK(rig.seq.playing().remaining == 2);
    rig.seq.locate16th(57); // playing: row 3 at the next pattern end
    rig.run(1500);          // 8 steps at 140 BPM = 37 800 frames
    CHECK(rig.seq.playing().row == 2);
}

TEST_CASE("arr.sequencer: a new plan keeps the playing row; song mode off leaves the pattern playing", "[arrangement]")
{
    SongRig rig;
    setSong(rig, kSong);
    playFromTop(rig);
    rig.run(3000);
    REQUIRE(rig.seq.playing().row == 1);
    auto shorter = std::make_unique<es::ArrangementPlan>(*rig.plan);
    shorter->count = 2;
    shorter->startPpq16[2] = rig.plan->startPpq16[2];
    rig.installed = shorter.get();
    rig.run(1);
    CHECK(rig.seq.playing().row == 1);
    rig.run(4000); // row 2 ends at 14 quarters (≈ 2.57 s at 140 after 4 s): the shorter song ends there
    CHECK(rig.clock.stops() == 1);
    // Song mode off while playing: the current pattern keeps looping, no switch.
    SongRig r2;
    setSong(r2, kSong);
    playFromTop(r2);
    r2.run(1000);
    r2.seq.setSongMode(false);
    const auto n = r2.switches.size();
    r2.run(8000);
    CHECK(r2.switches.size() == n);
    CHECK(r2.clock.stops() == 0);
}

TEST_CASE("arr.sequencer: song mode on while playing enters row 1 at the next pattern end", "[arrangement]")
{
    SongRig rig;
    setSong(rig, kSong);
    rig.seq.setSongMode(false);
    rig.sw.request(3, 7); // stopped: immediate
    rig.run(2);
    rig.clock.command({engine::TransportCmd::Play, 0.0});
    rig.run(100);
    rig.sw.request(4, 4); // a user request pending when song mode comes on: the song wins at the boundary
    rig.seq.setSongMode(true);
    rig.run(2000); // pattern 3.8 (16 steps) ends at 4 quarters = 88 200 frames
    REQUIRE(rig.switches.size() >= 2);
    CHECK(rig.switches[1] == Switch {128 + 88200, 0, 0}); // PLAY at block 2
    CHECK(rig.seq.playing().row == 0);
}

TEST_CASE("arr.sequencer: 999 rows play, each on its pass end", "[arrangement]")
{
    SongRig rig;
    std::vector<Row> rows;
    for (int i = 0; i < 999; ++i)
        rows.push_back({i % 3 == 2 ? 1 : 0, i % 2, 8, 1, i == 0 ? 300.0f : 0.0f});
    for (int i = 1; i < 999; ++i)
        if (rows[static_cast<std::size_t>(i)].scene == rows[static_cast<std::size_t>(i - 1)].scene &&
            rows[static_cast<std::size_t>(i)].pattern == rows[static_cast<std::size_t>(i - 1)].pattern)
            rows[static_cast<std::size_t>(i)].pattern = 2; // consecutive rows differ: every row change switches
    setSong(rig, rows);
    playFromTop(rig);
    const auto want = expectRows(rows, 120.0);
    REQUIRE(want.rows.size() == 998);
    rig.run(want.endFrame / 64 + 4);
    CHECK(rig.seq.rowChanges() == 999);
    const auto got = rowSwitches(rig, rows);
    CHECK(got.size() == want.rows.size());
    for (std::size_t i = 0; i < std::min(got.size(), want.rows.size()); ++i)
        if (got[i] != want.rows[i])
            FAIL("row " << want.rows[i].second << " at frame " << got[i].first << ", expected " << want.rows[i].first);
    CHECK(rig.clock.stops() == 1);
    for (const auto& [row, pos] : rig.rowStarts)
        if (pos != rig.plan->startPpq16[static_cast<std::size_t>(row)])
            FAIL("row " << row << " starts at sixteenth " << pos);
}

TEST_CASE("arr.sequencer: while switching is blocked a row that needs a switch holds", "[arrangement]")
{
    SongRig rig;
    setSong(rig, kSong);
    playFromTop(rig);
    rig.sw.setSwitchable(false); // SAMPLING / EDIT open (SCN-023)
    rig.run(6000);
    CHECK(rig.switches.size() <= 1); // the cue before PLAY at most
    CHECK(rig.seq.playing().row == 0);
    rig.sw.setSwitchable(true);
    rig.run(1400); // the next pass end (16 steps at 120 BPM = 1 378 blocks)
    CHECK(rig.seq.playing().row == 1);
}
