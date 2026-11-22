// Playback parameters: the PLAYBACK catalogue, ParamChange targets per [scene][track], step-lock
// resolution (SEQ-021 Revert / Hold), the grid ↔ frame helper (PLY-004, core/util/playback) and the swap set-up.
#include "core/params/ParamRegistry.h"
#include "core/util/playback/Grid.h"
#include "engine/Engine.h"
#include "engine/voice/PlaybackParams.h"
#include "engine/voice/VoiceSwaps.h"

#include "VoiceHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace pb = core::playback;
using engine::voice::PlaybackField;

TEST_CASE("voice.params: the PLAYBACK catalogue has every id of 09 §0.4 with its CC", "[voice][params]")
{
    const auto& r = core::builtinParams();
    CHECK(r.errors().empty());
    const struct
    {
        const char* id;
        int cc;
    } ids[] = {{"playback.pitch", 20},   {"playback.start", 21},       {"playback.loopStart", 22},
               {"playback.length", 23},  {"playback.loop", 15},        {"playback.timeStretch", 14},
               {"playback.reverse", -1}, {"playback.gridSnap", -1},    {"playback.bypass", -1}};
    for (const auto& e : ids)
    {
        INFO(e.id);
        const auto* s = r.find(e.id);
        REQUIRE(s != nullptr);
        CHECK(s->group == "PLAYBACK");
        CHECK(s->midiCc == e.cc);
        CHECK(s->scope == core::ParamScope::Track);
        const std::string id = e.id;
        CHECK(s->lockable == (id != "playback.gridSnap" && id != "playback.bypass")); // ● of 09 §0.4
        if (e.cc >= 0)
            CHECK(r[*r.byMidiCc(e.cc)].id == e.id);
        CHECK((engine::voice::playbackFieldOf(e.id) == PlaybackField::Count)
              == (std::string(e.id) == "playback.gridSnap")); // GRID SNAP is an editing mode, not an engine value
    }
    CHECK(engine::voice::playbackFieldOf("track.volume") == PlaybackField::Count);
    char buf[32];
    const int n = r.find("playback.length")->formatValue(44100, buf, sizeof buf);
    CHECK(std::string(buf, static_cast<std::size_t>(n)) == "44100");
}

TEST_CASE("voice.params: ParamChange values land in their [scene][track] slot in voice units", "[voice][params]")
{
    auto block = std::make_unique<engine::voice::PlaybackParamBlock>();
    const std::uint16_t scope = (3 << 4) | 9; // scene 4, track 10
    CHECK(block->set(PlaybackField::Pitch, scope, 7.5f));
    CHECK(block->set(PlaybackField::Start, scope, 1000.0f));
    CHECK(block->set(PlaybackField::LoopStart, scope, 1500.0f));
    CHECK(block->set(PlaybackField::Length, scope, 16777215.0f));
    CHECK(block->set(PlaybackField::Loop, scope, 1.0f));
    CHECK(block->set(PlaybackField::Reverse, scope, 1.0f));
    CHECK(block->set(PlaybackField::TimeStretch, scope, 2.0f));
    CHECK(block->set(PlaybackField::Bypass, scope, 1.0f));
    CHECK_FALSE(block->set(PlaybackField::Count, scope, 1.0f));
    CHECK_FALSE(block->set(PlaybackField::Pitch, 0xFFFF, 1.0f)); // project scope: not a track value
    const auto& p = block->at(3, 9);
    CHECK(p.pitchCents == 750.0f);
    CHECK(p.region.start == 1000);
    CHECK(p.region.loopStart == 1500);
    CHECK(p.region.length == 16777215);
    CHECK(p.region.loop);
    CHECK(p.region.reverse);
    CHECK(p.stretch == core::model::StretchMode::MasterTempo);
    CHECK(p.bypass);
    CHECK(block->at(0, 9).pitchCents == 0.0f); // other scenes untouched
    CHECK(block->at(3, 8).region.start == 0);
    block->set(PlaybackField::Pitch, scope, 99.0f);
    CHECK(block->at(3, 9).pitchCents == engine::voice::kMaxPitchCents);
}

TEST_CASE("voice.locks: locked values apply to one note and revert, Hold keeps them", "[voice][locks]")
{
    namespace slot = engine::voice::lockslot;
    engine::voice::PlaybackLocks locks;
    engine::voice::PlaybackParams base;
    base.region.start = 10;
    base.pitchCents = 100;
    float values[35] {};
    std::uint8_t change[35] {};
    values[slot::Start] = 500;
    values[slot::Pitch] = -12; // semitones (the ParamSpec unit)
    values[slot::Reverse] = 1;
    change[slot::Pitch] = static_cast<std::uint8_t>(engine::voice::LockChange::Hold);
    const std::uint64_t mask = (1ull << slot::Start) | (1ull << slot::Pitch) | (1ull << slot::Reverse) | (1ull << 0);
    auto p = locks.resolve(base, mask, values, change);
    CHECK(p.region.start == 500);
    CHECK(p.pitchCents == -1200.0f);
    CHECK(p.region.reverse);
    // The next trigger without locks: Start and Reverse revert, the held pitch stays.
    p = locks.resolve(base, 0, nullptr, nullptr);
    CHECK(p.region.start == 10);
    CHECK_FALSE(p.region.reverse);
    CHECK(p.pitchCents == -1200.0f);
    // A Revert lock of the slot ends the hold after its note.
    values[slot::Pitch] = 3;
    change[slot::Pitch] = 0;
    p = locks.resolve(base, 1ull << slot::Pitch, values, change);
    CHECK(p.pitchCents == 300.0f);
    CHECK(locks.resolve(base, 0, nullptr, nullptr).pitchCents == 100.0f);
}

TEST_CASE("voice.grid: grid positions and frames (PLY-004), coarse steps (PLY-002), CC mapping (PLY-003)",
          "[voice][grid]")
{
    static_assert(pb::gridFrame(1, 16, 1600) == 0);
    static_assert(pb::gridFrame(16, 16, 1600) == 1500);
    static_assert(pb::gridFrame(99, 16, 1600) == 1500); // clamped to N
    static_assert(pb::gridPosition(0, 16, 1600) == 1);
    static_assert(pb::gridPosition(149, 16, 1600) == 2);
    static_assert(pb::gridPosition(150, 16, 1600) == 2); // a tie goes to the earlier position
    static_assert(pb::gridPosition(151, 16, 1600) == 3);
    static_assert(pb::gridPosition(1600, 16, 1600) == 16);
    static_assert(pb::snapToGrid(1234, 16, 1600) == 1200);
    static_assert(pb::coarseStep(44100) == 344);
    static_assert(pb::coarseStep(50) == 1);
    static_assert(pb::ccToFrame(0, 1000) == 0);
    static_assert(pb::ccToFrame(127, 1000) == 1000);
    static_assert(pb::ccToFrame(64, 1000) == 504);
    for (int count : {1, 3, 16, 64})
        for (std::int64_t frames : {1LL, 7LL, 1000LL, 44100LL * 60 * 10})
            for (int n = 1; n <= count; ++n)
            {
                INFO(count << " " << frames << " " << n);
                if (frames >= count)
                    REQUIRE(pb::gridPosition(pb::gridFrame(n, count, frames), count, frames) == n); // round trip
                REQUIRE(pb::gridFrame(n, count, frames) < frames);
            }
    CHECK(pb::clampGridCount(0) == 1);
    CHECK(pb::clampGridCount(100) == pb::kMaxGridCount);
}

TEST_CASE("voice.swaps: the engine accepts TrackSample and Preview swaps and releases retired buffers",
          "[voice][swaps]")
{
    voicetest::TestBuffer a(std::vector<float>(100, 1.0f));
    voicetest::TestBuffer b(std::vector<float>(100, 1.0f));
    {
        engine::EngineOptions o;
        o.testTone = false;
        auto e = std::make_unique<engine::Engine>(engine::EngineSetup {}, o);
        engine::voice::acceptVoiceSwaps(*e); // the engine already does it; a second call is harmless
        core::audio::retain(a.get()); // the sender's reference for the slot
        REQUIRE(e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 3), a.get()}));
        std::vector<float> l(256), r(256);
        float* out[2] = {l.data(), r.data()};
        e->process(out, 2, 64);
        CHECK(engine::voice::asSampleBuffer(e->installed(engine::voice::trackSampleSlot(0, 3))) == a.get());
        core::audio::retain(b.get());
        REQUIRE(e->bridge().post(engine::ObjectSwap {engine::voice::trackSampleSlot(0, 3), b.get()}));
        core::audio::retain(a.get());
        REQUIRE(e->bridge().post(engine::ObjectSwap {engine::voice::previewSlot(), a.get()}));
        for (int i = 0; i < 4; ++i)
        {
            e->process(out, 2, 64);
            e->bridge().retirePending();
        }
        CHECK(a.refs() == 1); // the retired slot reference was dropped; the preview slot holds one
        CHECK(b.refs() == 2); // the slot's + track 4's voice (TrackVoice::setSample holds the track's buffer)
    }
    CHECK(a.refs() == 0); // the engine's destructor released what was installed
    CHECK(b.refs() == 0);
}
