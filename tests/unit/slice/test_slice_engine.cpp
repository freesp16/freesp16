// Slicing inside engine::Engine (SLC-003/004/010, threading.md "Object swap protocol"): the slice table
// travels as ObjectSwap{SwapSlot::SliceMap, scene, track}, is installed at a block start (never mid-block), replaces
// the previous one (which retires to the message thread), and SLICE pad mode hits from the TriggerSink play the paged
// slice of the selected track; the engine renders under the real-time checks (OfflineDriver marks the thread).
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/voice/VoiceHarness.h"

#include "core/commands/slice/SlicePoints.h"
#include "engine/voice/SliceMap.h"
#include "engine/voice/VoiceSwaps.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

const core::slice::SliceTable* post(engine::Engine& e, int scene, int track, int count, int window, std::int64_t frames)
{
    core::model::SliceSettings s;
    s.gridCount = count;
    s.windowOffset = window;
    auto* t = new core::slice::SliceTable(core::slice::buildTable(s, frames));
    if (! e.bridge().post(engine::ObjectSwap {engine::voice::sliceMapSlot(scene, track), t}))
    {
        core::slice::destroySliceTable(t);
        return nullptr;
    }
    return t;
}
} // namespace

TEST_CASE("slice.engine: the table swaps in at a block start; SLICE pad hits play the paged slice", "[slice][engine]")
{
    constexpr int frames = 64 * 256; // 64 slices of 256 frames
    voicetest::TestBuffer buf(voicetest::sine(frames, 330.0, voicetest::kRate, 0.5f));
    auto e = enginetest::make(false);
    core::audio::retain(buf.get());
    REQUIRE(e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 2), buf.get()}));
    REQUIRE(e->bridge().post(engine::ParamChange {id("pads.selected"), 0xFFFF, 2.0f, 0}));
    REQUIRE(e->bridge().post(engine::ParamChange {id("pads.mode"), 0xFFFF, 3.0f, 0})); // SLICE
    const auto* table = post(*e, 0, 2, 64, 1, frames);
    REQUIRE(table != nullptr);
    CHECK(e->installed(engine::voice::sliceMapSlot(0, 2)) == nullptr); // nothing before the next block
    enginetest::render(*e, 64, 64);
    CHECK(e->installed(engine::voice::sliceMapSlot(0, 2)) == table);

    REQUIRE(e->triggers().push({3, 127, 60, 3})); // pad 4 → slice 16·1 + 3 = slice 20
    enginetest::render(*e, 128, 64);
    const auto& src = e->trackSource(2);
    CHECK(src.playingSlice() == 19);
    CHECK(src.voice().position() >= 19 * 256);
    CHECK(src.voice().position() < 20 * 256);
    CHECK_FALSE(e->trackSource(3).voice().active()); // the pad's own track stays silent
    enginetest::render(*e, 512, 64);
    CHECK_FALSE(src.voice().mainActive()); // the slice ended at its end (256 frames)

    // A re-slice: the new table replaces the old one at the next block start; the old one retires (message thread).
    const auto* next = post(*e, 0, 2, 8, 0, frames);
    REQUIRE(next != nullptr);
    enginetest::render(*e, 64, 64);
    CHECK(e->installed(engine::voice::sliceMapSlot(0, 2)) == next);
    enginetest::render(*e, 256, 64);
    e->bridge().retirePending(); // frees the 64-slice table (destroySliceTable)
    REQUIRE(e->triggers().push({12, 127, 60, 12})); // pad 13: an 8-slice grid has none
    enginetest::render(*e, 128, 64);
    CHECK_FALSE(src.voice().active());
    REQUIRE(e->triggers().push({7, 127, 60, 7})); // pad 8 = slice 8 of 8
    enginetest::render(*e, 128, 64);
    CHECK(src.playingSlice() == 7);
    CHECK(src.voice().position() >= 7 * frames / 8);
}

TEST_CASE("slice.engine: each scene × track has its own slot (tables of other scenes are kept for their scene)", "[slice][engine]")
{
    auto e = enginetest::make(false);
    const auto* other = post(*e, 5, 0, 32, 0, 1000);
    REQUIRE(other != nullptr);
    enginetest::render(*e, 64, 64);
    CHECK(e->installed(engine::voice::sliceMapSlot(5, 0)) == other);
}
