// PerfStage against the ModMatrix contract (engine/modulation/ModMatrix.h): transpose is one ModSource::Transpose
// Offset on playback.pitch of pitchOffset01(st · 100), published once per change and again after a scene switch,
// cleared at 0 st; the matrix's own clamp(base + offset) puts the note exactly `st` semitones from a centred base.
// Built against the header only: the real class is checked by the TransposeMatrix concept, a recorder stands in for it
// here.
#include "engine/sequencer/perf/PerfStage.h"

#include "core/model/ModelOps.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "engine/modulation/ModMatrix.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <utility>

namespace perf = engine::sequencer::perf;
namespace mod = engine::modulation;

static_assert(perf::TransposeMatrix<mod::ModMatrix>, "PerfStage::publishMods takes the LFO ModMatrix");

namespace
{
core::ParamId id(const char* name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }
core::ParamId resolve(std::string_view s) { return core::builtinParams().idOf(s).value_or(core::ParamId {}); }

struct FakeMatrix
{
    std::map<int, float> value; // track → Offset
    int sets = 0, clears = 0, refuse = -1;
    core::ParamId dest {};
    bool set(int track, core::ParamId d, mod::ModSource src, mod::ModKind kind, float v) noexcept
    {
        if (track == refuse)
            return false;
        CHECK(src == mod::ModSource::Transpose);
        CHECK(kind == mod::ModKind::Offset);
        dest = d;
        value[track] = v;
        ++sets;
        return true;
    }
    void clear(int track, core::ParamId d, mod::ModSource src) noexcept
    {
        CHECK(src == mod::ModSource::Transpose);
        dest = d;
        value.erase(track);
        ++clears;
    }
};

engine::ParamChange change(const char* name, int scene, int track, float v)
{
    return engine::ParamChange {id(name), static_cast<std::uint16_t>((scene << 4) | track), v, 0};
}
} // namespace

TEST_CASE("perf.stage: seq.retrigCount / retrigSpeed / transpose are bound, per scene and track, clamped", "[perf][stage]")
{
    perf::PerfStage s;
    engine::bridge::ParamBindings b;
    s.bindParams(resolve, b);
    for (const char* n : {"seq.retrigCount", "seq.retrigSpeed", "seq.transpose"})
        CHECK(b.find(id(n)) == engine::bridge::ParamTarget::Sequencer);
    CHECK(s.pitchParam() == id("playback.pitch"));
    CHECK(s.trackRetrig(0, 0).count == 1);
    CHECK(s.trackRetrig(0, 0).speedIndex == perf::kDefaultRetrigSpeed);
    CHECK(s.onParam(change("seq.retrigCount", 2, 5, 9.0f)));
    CHECK(s.onParam(change("seq.retrigSpeed", 2, 5, 7.0f)));   // clamped to 5
    CHECK(s.onParam(change("seq.transpose", 2, 5, -30.0f)));   // clamped to −24
    CHECK(s.onParam(change("seq.retrigCount", 3, 5, 0.0f)));   // clamped to 1
    CHECK_FALSE(s.onParam(change("seq.swing", 0, 0, 10.0f)));  // the sequencer's
    CHECK(s.trackRetrig(2, 5).count == 9);
    CHECK(s.trackRetrig(2, 5).speedIndex == 5);
    CHECK(s.transpose(2, 5) == -24);
    CHECK(s.trackRetrig(3, 5).count == 1);
    CHECK(s.trackRetrig(0, 5).count == 1);

    // A step's locks override the track values.
    float values[35] = {};
    values[perf::kLockSlotRetrigCount] = 3.0f;
    values[perf::kLockSlotRetrigSpeed] = 0.0f;
    const engine::trigger::LockSet set {1ull << perf::kLockSlotRetrigCount | 1ull << perf::kLockSlotRetrigSpeed, values,
                                        nullptr};
    engine::trigger::TriggerEvent e {};
    e.track = 5;
    CHECK(s.specOf(2, e).count == 9);
    e.locks = &set;
    CHECK(s.specOf(2, e).count == 3);
    CHECK(s.specOf(2, e).speedIndex == 0);
}

TEST_CASE("perf.stage: transpose reaches the matrix as one Offset of st/48 on playback.pitch; exact semitones",
          "[perf][stage][transpose]")
{
    perf::PerfStage s;
    engine::bridge::ParamBindings b;
    s.bindParams(resolve, b);
    FakeMatrix m;
    s.publishMods(m); // prepare state: every track published once (all 0 → clear)
    CHECK(m.clears == 16);
    CHECK(s.transposeDirty() == 0);
    const auto* pitch = core::builtinParams().find("playback.pitch");
    REQUIRE(pitch != nullptr);
    for (const int st : {-24, -12, -1, 1, 7, 24})
    {
        INFO("st " << st);
        REQUIRE(s.onParam(change("seq.transpose", 0, 4, static_cast<float>(st))));
        s.publishMods(m);
        REQUIRE(m.value.count(4) == 1u);
        CHECK(m.dest == s.pitchParam());
        CHECK(m.value[4] == mod::pitchOffset01(static_cast<float>(st) * 100.0f));
        CHECK(mod::pitchOffsetCents(m.value[4]) == Catch::Approx(st * 100.0).margin(1e-3));
        // The matrix's sum on a centred base (0 st, normalised 0.5) lands exactly st semitones away.
        const mod::ModValue v {m.value[4], 0.0f, false};
        const float semis = pitch->minValue + v.apply(pitch->normalised(0.0f)) * (pitch->maxValue - pitch->minValue);
        CHECK(semis == Catch::Approx(st).margin(1e-4));
    }
    const int sets = m.sets;
    s.publishMods(m); // unchanged: nothing
    CHECK(m.sets == sets);
    REQUIRE(s.onParam(change("seq.transpose", 0, 4, 0.0f)));
    s.publishMods(m);
    CHECK(m.value.count(4) == 0u);
}

TEST_CASE("perf.stage: a scene switch publishes the new scene's transpose; a full matrix is retried", "[perf][stage][transpose]")
{
    perf::PerfStage s;
    engine::bridge::ParamBindings b;
    s.bindParams(resolve, b);
    FakeMatrix m;
    s.publishMods(m);
    REQUIRE(s.onParam(change("seq.transpose", 1, 2, 12.0f))); // scene 2: not playing
    CHECK(s.transposeDirty() == 0);
    s.publishMods(m);
    CHECK(m.value.empty());

    engine::trigger::TriggerRouter router;
    engine::BlockContext ctx {};
    ctx.sampleRate = 44100.0;
    ctx.frames = 64;
    ctx.bpm = 120.0;
    engine::stages::BlockBuffers::Timing timing {};
    timing.scene = 1;
    s.process(ctx, timing, router); // stopped: nothing to ratchet, the scene is followed
    CHECK(s.transposeDirty() == 0xFFFF);
    m.refuse = 2;
    s.publishMods(m);
    CHECK(s.transposeDirty() == (1u << 2)); // refused: kept for the next block
    m.refuse = -1;
    s.publishMods(m);
    CHECK(m.value.at(2) == mod::pitchOffset01(1200.0f));
    CHECK(s.transpose(2) == 12);
}

TEST_CASE("perf.stage: pending events in frame order — the previous step's repeats before the new step", "[perf][stage]")
{
    perf::PerfStage s;
    engine::bridge::ParamBindings b;
    s.bindParams(resolve, b);
    engine::trigger::TriggerRouter router;
    router.prepare(44100.0, 64);
    engine::BlockContext ctx {};
    ctx.sampleRate = 44100.0;
    ctx.frames = 64;
    ctx.bpm = 120.0;
    ctx.playing = true;
    engine::stages::BlockBuffers::Timing timing {};
    REQUIRE(s.onParam(change("seq.retrigCount", 0, 1, 16.0f)));
    // A step at frame 0 with interval 10 frames (a fake tempo: 1/8 step = 10 frames → step = 80 frames).
    ctx.bpm = 15.0 * 44100.0 / 80.0;
    REQUIRE(s.onParam(change("seq.retrigSpeed", 0, 1, 0.0f)));
    engine::trigger::TriggerEvent e {};
    e.track = 1;
    e.velocity = 100;
    e.kind = engine::trigger::TriggerKind::Full;
    e.source = engine::trigger::TriggerSource::Sequencer;
    e.frame = 0;
    REQUIRE(router.push(e));
    s.process(ctx, timing, router);
    CHECK(s.repeatsThisBlock() == 6); // 10, 20, …, 60
    router.process(ctx);
    // Next block: a new step at frame 25 cancels the repeats from 30 on; its own start at 35, 45, 55.
    e.frame = 25;
    REQUIRE(router.push(e));
    s.process(ctx, timing, router);
    router.process(ctx);
    std::vector<int> frames;
    for (const auto& ev : router.blockEvents())
        if (ev.source == engine::trigger::TriggerSource::Retrigger)
            frames.push_back(ev.frame);
    CHECK(frames == std::vector<int> {6, 16, 35, 45, 55});
}

TEST_CASE("perf.stage: one RETRIGGER default in catalogue, model and engine: CNT 1, SPD 1/4 step (SEQ-030, M p.40)",
          "[perf][stage]")
{
    const auto* count = core::builtinParams().find("seq.retrigCount");
    const auto* speed = core::builtinParams().find("seq.retrigSpeed");
    REQUIRE(count != nullptr);
    REQUIRE(speed != nullptr);
    const auto project = core::model::newDefaultProject();
    const auto& seq = project->scenes[0].tracks[0].seq;
    CHECK(speed->defaultValue == static_cast<float>(perf::kDefaultRetrigSpeed));
    CHECK(seq.retriggerSpeed == perf::kDefaultRetrigSpeed);
    CHECK(perf::retrigFraction(perf::kDefaultRetrigSpeed) == 0.25);
    CHECK(speed->enumLabels[perf::kDefaultRetrigSpeed] == "1/64"); // a 1/4 step shown as a note value
    CHECK(count->defaultValue == 1.0f);
    CHECK(seq.retriggerCount == 1);
    perf::PerfStage s;
    CHECK(s.trackRetrig(0, 0).speedIndex == perf::kDefaultRetrigSpeed);
    CHECK(s.trackRetrig(0, 0).count == 1);
}
