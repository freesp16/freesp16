// The modulation matrix and the LFO stage (spec lfo.md LFO-003/005/006/007/009/010/013,
// effects/parameters.md FX-083…085): the sum and the clamp, Override, release, dirty flags, the FX destinations
// through FxSlotBank::setModulation (a destination of another effect type changes nothing, bit for bit), the
// engine's track sink, and the stage's restarts and step locks.
#include "tests/unit/fx/FxTestUtil.h"

#include "core/util/Realtime.h"
#include "core/util/lfo/LfoLaw.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/mixer/Mixer.h"
#include "engine/modulation/LfoStage.h"
#include "engine/modulation/ModMatrix.h"
#include "engine/trigger/SourceSet.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

using engine::fx::FxSlotBank;
using engine::fx::insertSlot;
using engine::modulation::ModKind;
using engine::modulation::ModMatrix;
using engine::modulation::ModSource;
using engine::modulation::ModValue;

namespace
{
struct Call
{
    int track;
    core::ParamId dest;
    ModValue v;
    bool release;
};
struct RecordingSink final : engine::modulation::ParamStateSink
{
    std::vector<Call> calls;
    bool known = true;
    RecordingSink() { calls.reserve(256); } // no allocation inside the real-time scopes
    bool modulate(int track, core::ParamId dest, const ModValue& v) noexcept override
    {
        calls.push_back({track, dest, v, false});
        return known;
    }
    void release(int track, core::ParamId dest) noexcept override { calls.push_back({track, dest, {}, true}); }
};

core::ParamId id(std::string_view s) { return fxtest::idOf(s); }

int typeOf(const FxSlotBank& bank, std::string_view effect) { return engine::fx::typeIndexOf(bank.insertTypes(), effect); }

constexpr std::uint16_t scope(int scene, int track) { return static_cast<std::uint16_t>((scene << 4) | track); }
} // namespace

TEST_CASE("modulation.matrix: offsets sum, Override replaces the base, the clamp holds (FX-084)", "[modulation]")
{
    ModValue v;
    v.offset01 = 1.0f; // depth 127, lfoOut +1
    CHECK(v.apply(0.9f) == 1.0f);
    v.offset01 = -1.0f;
    CHECK(v.apply(0.9f) == 0.0f);
    v.offset01 = 0.05f;
    CHECK(std::fabs(v.apply(0.9f) - 0.95f) < 1e-7f);
    v.hasOverride = true;
    v.override01 = 0.2f;
    CHECK(std::fabs(v.apply(0.9f) - 0.25f) < 1e-7f);

    ModMatrix m;
    m.prepare();
    RecordingSink sink;
    FxSlotBank bank; // not built: no FX routes, everything is a track destination
    const auto pitch = id("playback.pitch");
    REQUIRE(m.set(3, pitch, ModSource::Lfo, ModKind::Offset, 0.25f));
    REQUIRE(m.set(3, pitch, ModSource::Transpose, ModKind::Offset, -0.125f));
    REQUIRE(m.set(3, pitch, ModSource::Lfo, ModKind::Offset, 0.5f)); // replaces the LFO's own value
    CHECK(m.contribution(3, pitch, ModSource::Lfo) == 0.5f);
    CHECK(m.value(3, pitch).offset01 == 0.375f);
    CHECK(m.activeDestinations(3) == 1);
    {
        core::rt::ScopedRealtimeThread rt; // applyBlock allocates nothing (the test sink records outside)
        m.applyBlock(bank, sink);
    }
    REQUIRE(sink.calls.size() == 1);
    CHECK(sink.calls[0].track == 3);
    CHECK(sink.calls[0].v.offset01 == 0.375f);
    CHECK_FALSE(sink.calls[0].v.hasOverride);
    // Untouched: nothing is applied again (dirty flags); touch() re-applies.
    m.applyBlock(bank, sink);
    CHECK(sink.calls.size() == 1);
    m.set(3, pitch, ModSource::Lfo, ModKind::Offset, 0.5f); // the same value: still clean
    m.applyBlock(bank, sink);
    CHECK(sink.calls.size() == 1);
    m.touch(3);
    m.applyBlock(bank, sink);
    CHECK(sink.calls.size() == 2);
    // Ribbon Override wins over the base; the highest overriding source wins.
    m.set(3, pitch, ModSource::Ribbon, ModKind::Override, 0.75f);
    m.set(3, pitch, ModSource::Lfo, ModKind::Override, 0.1f);
    const auto o = m.value(3, pitch);
    CHECK(o.hasOverride);
    CHECK(o.override01 == 0.75f);
    CHECK(o.offset01 == -0.125f);
    // Clearing every source releases the destination once and frees the slot.
    m.clear(3, ModSource::Lfo);
    m.clear(3, ModSource::Ribbon);
    m.clear(3, pitch, ModSource::Transpose);
    sink.calls.clear();
    m.applyBlock(bank, sink);
    REQUIRE(sink.calls.size() == 1);
    CHECK(sink.calls[0].release);
    CHECK(m.activeDestinations(3) == 0);
    m.applyBlock(bank, sink);
    CHECK(sink.calls.size() == 1);
    // Fixed slots: 8 destinations per track, the 9th is refused; invalid input is refused.
    for (std::uint16_t i = 0; i < 8; ++i)
        CHECK(m.set(0, core::ParamId {i}, ModSource::Ribbon, ModKind::Override, 0.5f));
    CHECK_FALSE(m.set(0, core::ParamId {8}, ModSource::Ribbon, ModKind::Override, 0.5f));
    CHECK_FALSE(m.set(16, pitch, ModSource::Lfo, ModKind::Offset, 0.1f));
    CHECK_FALSE(m.set(1, core::ParamId {}, ModSource::Lfo, ModKind::Offset, 0.1f));
    m.clearAll(ModSource::Ribbon);
    m.applyBlock(bank, sink);
    CHECK(m.activeDestinations(0) == 0);
    // A destination the sink does not know is kept (nothing applied) and never released.
    sink.known = false;
    sink.calls.clear();
    m.set(5, core::ParamId {1}, ModSource::Lfo, ModKind::Offset, 0.3f);
    m.applyBlock(bank, sink);
    m.clear(5, ModSource::Lfo);
    m.applyBlock(bank, sink);
    REQUIRE(sink.calls.size() == 1);
    CHECK_FALSE(sink.calls[0].release);
}

TEST_CASE("modulation.fx: FX destinations reach FxSlotBank::setModulation; the clamp holds at depth 127 / base 0.9",
          "[modulation][lfo]")
{
    FxSlotBank bank;
    fxtest::buildBank(bank);
    const int gt = typeOf(bank, "gainTest");
    REQUIRE(gt >= 0);
    const int s = insertSlot(4, 0);
    bank.onParamChange(fxtest::change("fx1.type", static_cast<float>(gt + 1), scope(0, 4)));
    bank.onParamChange(fxtest::change("fx1.gainTest.gain", 0.9f, scope(0, 4)));
    ModMatrix m;
    m.prepare();
    RecordingSink sink;
    const auto dest = id("fx1.gainTest.gain");
    m.set(4, dest, ModSource::Lfo, ModKind::Offset, 1.0f); // lfoOut +1 · depth 127/127
    {
        core::rt::ScopedRealtimeThread rt;
        m.applyBlock(bank, sink);
    }
    CHECK(sink.calls.empty()); // FX ids never reach the track sink
    CHECK(bank.slot(s).shell(gt)->param(0) == 1.0f);
    m.set(4, dest, ModSource::Lfo, ModKind::Offset, -0.3f);
    m.applyBlock(bank, sink);
    CHECK(std::fabs(bank.slot(s).shell(gt)->param(0) - 0.6f) < 1e-6f);
    m.set(4, dest, ModSource::Lfo, ModKind::Offset, -1.0f);
    m.applyBlock(bank, sink);
    CHECK(bank.slot(s).shell(gt)->param(0) == 0.0f);
    // Override (ribbon USER): the absolute value, plus the offsets.
    m.set(4, dest, ModSource::Ribbon, ModKind::Override, 0.3f);
    m.set(4, dest, ModSource::Lfo, ModKind::Offset, 0.1f);
    m.applyBlock(bank, sink);
    CHECK(std::fabs(bank.slot(s).shell(gt)->param(0) - 0.4f) < 1e-6f);
    // Released: the stored value returns.
    m.clear(4, ModSource::Lfo);
    m.clear(4, ModSource::Ribbon);
    m.applyBlock(bank, sink);
    CHECK(std::fabs(bank.slot(s).shell(gt)->param(0) - 0.9f) < 1e-6f);
    CHECK_FALSE(m.modulated(4, dest));
}

TEST_CASE("modulation.fx: an FX destination of another effect type changes nothing, bit for bit (FX-085, LFO-013)",
          "[modulation][lfo]")
{
    // Two banks with gainTest in track 2's FX2, processing the same noise; one has an LFO on fx2.none.mix (a type that
    // is not in the slot). Their outputs are identical; when the type returns the destination applies.
    auto a = std::make_unique<FxSlotBank>(), b = std::make_unique<FxSlotBank>();
    fxtest::buildBank(*a);
    fxtest::buildBank(*b);
    const int gt = typeOf(*a, "gainTest"), none = typeOf(*a, "none");
    REQUIRE(none >= 0);
    for (auto* bank : {a.get(), b.get()})
    {
        bank->onParamChange(fxtest::change("fx2.type", static_cast<float>(gt + 1), scope(0, 2)));
        bank->onParamChange(fxtest::change("fx2.gainTest.gain", 0.6f, scope(0, 2)));
    }
    ModMatrix m;
    m.prepare();
    RecordingSink sink;
    const auto stale = id("fx2.none.mix");
    std::uint32_t noise = 12345u;
    alignas(16) float la[64], ra[64], lb[64], rb[64];
    bool same = true;
    {
        core::rt::ScopedRealtimeThread rt;
        for (int block = 0; block < 400; ++block)
        {
            m.set(2, stale, ModSource::Lfo, ModKind::Offset, std::sin(block * 0.1f)); // a moving LFO value
            m.applyBlock(*b, sink);
            for (int i = 0; i < 64; ++i)
            {
                noise = core::lfo::xorshift32(noise);
                la[i] = lb[i] = core::lfo::bipolar(noise) * 0.5f;
                ra[i] = rb[i] = -la[i];
            }
            const engine::BlockContext bc {44100.0, 64, 0, 0, 120, false, false, false, 0, {}, static_cast<std::uint64_t>(block)};
            const auto pa = a->beginBlock(bc, 0);
            const auto pb = b->beginBlock(bc, 0);
            a->processInsert(2, la, ra, pa);
            b->processInsert(2, lb, rb, pb);
            for (int i = 0; i < 64; ++i)
                same = same && la[i] == lb[i] && ra[i] == rb[i];
        }
    }
    CHECK(same);
    CHECK(m.stale(2, stale));
    CHECK(sink.calls.empty());
    CHECK(b->slot(insertSlot(2, 1)).shell(gt)->param(0) == 0.6f);
    // The type returns: the LFO applies from that block (clamp(stored 0 + amount) on the none effect's mix).
    b->onParamChange(fxtest::change("fx2.type", static_cast<float>(none + 1), scope(0, 2)));
    m.set(2, stale, ModSource::Lfo, ModKind::Offset, 0.25f);
    m.applyBlock(*b, sink);
    CHECK_FALSE(m.stale(2, stale));
    const int s2 = insertSlot(2, 1), k = 5; // the none effect's MIX is k5
    CHECK(std::fabs(b->slot(s2).shell(none)->param(k) - std::fmin(1.0f, b->effective(s2, k) + 0.25f)) < 1e-6f);
    CHECK(m.modulated(2, stale));
}

TEST_CASE("modulation.sink: the engine's track destinations (pitch, envelope, strip) with the clamp", "[modulation][lfo]")
{
    engine::bridge::ParamBindings bindings;
    using T = engine::bridge::ParamTarget;
    bindings.bind(id("playback.pitch"), static_cast<T>(static_cast<int>(T::PlaybackFirst)));
    bindings.bind(id("env.attack"), static_cast<T>(static_cast<int>(T::EnvFirst) + 1));
    bindings.bind(id("env.release"), static_cast<T>(static_cast<int>(T::EnvFirst) + 3));
    bindings.bind(id("track.volume"), T::TrackVolume);
    bindings.bind(id("track.pan"), T::TrackPan);
    auto sources = std::make_unique<engine::trigger::SourceSet>();
    auto mixer = std::make_unique<engine::mixer::Mixer>();
    for (int t = 0; t < engine::kTracks; ++t)
        sources->sources[static_cast<std::size_t>(t)].attach(&sources->env, t);
    engine::modulation::TrackModSink sink;
    sink.attach(&bindings, sources.get(), mixer.get());
    sink.beginBlock(0, 120.0);
    sources->envParams[0][1].attack = 100;
    sources->envParams[0][1].release = 120;
    sources->env.setParams(1, sources->envParams[0][1], 120.0);

    ModValue up;
    up.offset01 = 1.0f;
    {
        core::rt::ScopedRealtimeThread rt; // every destination is allocation-free
        sink.modulate(1, id("playback.pitch"), up);
        sink.modulate(1, id("track.volume"), up);
        sink.release(1, id("track.volume"));
        sink.release(1, id("playback.pitch"));
    }
    CHECK(sink.modulate(1, id("env.attack"), up));
    CHECK(sources->env.params(1).attack == 127); // clamp(100/127 + 1) · 127
    ModValue down;
    down.offset01 = -0.25f;
    CHECK(sink.modulate(1, id("env.release"), down));
    CHECK(sources->env.params(1).release == 88); // 120 − 0.25 · 128
    CHECK(sources->env.params(1).attack == 100); // each call starts from the stored base
    sink.release(1, id("env.release"));
    CHECK(sources->env.params(1).release == 120);
    CHECK(sink.modulate(1, id("playback.pitch"), up));
    CHECK(sink.modulate(1, id("track.volume"), down));
    CHECK(sink.modulate(1, id("track.pan"), up));
    sink.release(1, id("track.pan"));
    CHECK_FALSE(sink.modulate(1, id("lfo.depth"), up)); // not a track destination
    CHECK_FALSE(sink.modulate(1, id("fx1.gainTest.gain"), up));
}

TEST_CASE("lfo.stage: restarts per Mode / Trigger On, bypass, destination changes, step locks (LFO-007/009/010)",
          "[lfo]")
{
    using engine::modulation::LfoField;
    using engine::trigger::TriggerEvent;
    using engine::trigger::TriggerKind;
    using engine::trigger::TriggerSource;
    auto stage = std::make_unique<engine::modulation::LfoStage>();
    stage->prepare(44100.0);
    ModMatrix m;
    m.prepare();
    const auto pitch = id("playback.pitch"), volume = id("track.volume");
    stage->setParam(LfoField::Destination, scope(0, 0), static_cast<float>(pitch.value));
    stage->setParam(LfoField::Shape, scope(0, 0), static_cast<float>(engine::modulation::LfoShape::Saw));
    stage->setParam(LfoField::Depth, scope(0, 0), 127.0f);
    stage->setParam(LfoField::Speed, scope(0, 0), 127.0f); // 40 Hz
    stage->setParam(LfoField::TriggerMode, scope(0, 0), 1.0f); // TRIGGER
    engine::BlockContext ctx {44100.0, 64, 0, 0, 120, false, false, false, 0, {}, 0};
    {
        core::rt::ScopedRealtimeThread rt;
        for (int b = 0; b < 10; ++b)
            stage->process(ctx, 0, {}, m);
    }
    REQUIRE(stage->appliedDestination(0) == pitch);
    CHECK(m.contribution(0, pitch, ModSource::Lfo) == stage->amount(0));
    const double before = stage->lfo(0).elapsed();
    CHECK(before > 0.5);

    // A Half step does not restart in FULL mode; a Full one does.
    TriggerEvent half {0, 100, 0, TriggerKind::Half, TriggerSource::Sequencer, 10, true, nullptr};
    TriggerEvent full {0, 100, 0, TriggerKind::Full, TriggerSource::Pad, 10, true, nullptr};
    stage->process(ctx, 0, {&half, 1}, m);
    CHECK(stage->lfo(0).elapsed() > before);
    stage->process(ctx, 0, {&full, 1}, m);
    CHECK(stage->lfo(0).elapsed() == 0.0);
    CHECK(stage->lfo(0).rawValue() == 1.0f); // saw at phase offset 0
    stage->setParam(LfoField::TriggerOn, scope(0, 0), 1.0f); // FULL & HALF
    stage->process(ctx, 0, {}, m);
    stage->process(ctx, 0, {&half, 1}, m);
    CHECK(stage->lfo(0).elapsed() == 0.0);
    stage->setParam(LfoField::TriggerMode, scope(0, 0), 0.0f); // FREE: triggers never restart
    stage->process(ctx, 0, {}, m);
    stage->process(ctx, 0, {&full, 1}, m);
    CHECK(stage->lfo(0).elapsed() > 0.0);
    // Transport start restarts every mode.
    engine::BlockContext start = ctx;
    start.startedThisBlock = true;
    start.playing = true;
    stage->process(start, 0, {}, m);
    CHECK(stage->lfo(0).elapsed() == 0.0);

    // Bypass, depth 0 and NONE clear the LFO's contribution; a new destination moves it.
    stage->setParam(LfoField::Bypass, scope(0, 0), 1.0f);
    stage->process(ctx, 0, {}, m);
    CHECK_FALSE(m.modulated(0, pitch));
    stage->setParam(LfoField::Bypass, scope(0, 0), 0.0f);
    stage->setParam(LfoField::Destination, scope(0, 0), static_cast<float>(volume.value));
    stage->process(ctx, 0, {}, m);
    CHECK(m.modulated(0, volume));
    CHECK_FALSE(m.modulated(0, pitch));
    stage->setParam(LfoField::Destination, scope(0, 0), -1.0f);
    stage->process(ctx, 0, {}, m);
    CHECK_FALSE(m.modulated(0, volume));
    CHECK_FALSE(stage->appliedDestination(0).valid());
    // Another scene's values wait for that scene.
    stage->setParam(LfoField::Destination, scope(3, 0), static_cast<float>(volume.value));
    stage->process(ctx, 0, {}, m);
    CHECK_FALSE(m.modulated(0, volume));
    stage->process(ctx, 3, {}, m);
    CHECK(m.modulated(0, volume));

    // Step locks (slots 18–20): depth locked to 0 on one step clears the contribution; the next step reverts it.
    float values[35] {};
    std::uint8_t change[35] {};
    values[engine::modulation::kLockSlotLfoDepth] = 0.0f;
    engine::trigger::LockSet locks {1ull << engine::modulation::kLockSlotLfoDepth, values, change};
    TriggerEvent locked {0, 100, 0, TriggerKind::Full, TriggerSource::Sequencer, 0, true, &locks};
    TriggerEvent plain {0, 100, 0, TriggerKind::Full, TriggerSource::Sequencer, 0, true, nullptr};
    stage->process(ctx, 3, {&locked, 1}, m);
    CHECK(stage->lfo(0).depth() == 0.0f);
    CHECK_FALSE(m.modulated(0, volume));
    TriggerEvent ratchet = plain;
    ratchet.source = TriggerSource::Retrigger; // repeats neither apply nor revert locks
    stage->process(ctx, 3, {&ratchet, 1}, m);
    CHECK(stage->lfo(0).depth() == 0.0f);
    stage->process(ctx, 3, {&plain, 1}, m);
    CHECK(stage->lfo(0).depth() == core::lfo::depth01(63)); // the default depth again
    CHECK(m.modulated(0, volume));
    // Hold keeps the lock for the following steps.
    values[engine::modulation::kLockSlotLfoDepth] = 127.0f;
    change[engine::modulation::kLockSlotLfoDepth] = 1;
    stage->process(ctx, 3, {&locked, 1}, m);
    stage->process(ctx, 3, {&plain, 1}, m);
    CHECK(stage->lfo(0).depth() == 1.0f);
}

// Several sources on one destination (FX-084): base = the stored value or the step's lock; an Override (touch strip
// USER item) replaces that base, the Offsets (LFO, transpose, strip PITCH, bend) add, one clamp to the range.
namespace
{
struct MotionStep // track 2 with a motion step locking VOLUME 40 and PITCH +12 st (trigless: a PARAM step)
{
    static constexpr int track = 2;
    engine::bridge::ParamBindings bindings;
    std::unique_ptr<engine::trigger::SourceSet> sources = std::make_unique<engine::trigger::SourceSet>();
    std::unique_ptr<engine::mixer::Mixer> mixer = std::make_unique<engine::mixer::Mixer>();
    engine::modulation::TrackModSink sink;
    std::array<float, 64> values {};
    engine::trigger::LockSet locks;
    MotionStep()
    {
        using T = engine::bridge::ParamTarget;
        bindings.bind(id("playback.pitch"), static_cast<T>(static_cast<int>(T::PlaybackFirst)));
        bindings.bind(id("env.velocity"), static_cast<T>(static_cast<int>(T::EnvFirst)));
        bindings.bind(id("track.volume"), T::TrackVolume);
        for (int t = 0; t < engine::kTracks; ++t)
            sources->sources[static_cast<std::size_t>(t)].attach(&sources->env, t);
        sink.attach(&bindings, sources.get(), mixer.get());
        sink.beginBlock(0, 120.0);
        values[engine::mixer::kLockVolume] = 40.0f;
        values[engine::voice::lockslot::Pitch] = 12.0f;
        locks.mask = std::uint64_t {1} << engine::mixer::kLockVolume | std::uint64_t {1} << engine::voice::lockslot::Pitch;
        locks.values = values.data();
        mixer->applyLocks(track, &locks, 0);
        engine::trigger::TriggerEvent e {static_cast<std::uint8_t>(track), 100, 0, engine::trigger::TriggerKind::LockOnly,
                                         engine::trigger::TriggerSource::Sequencer, 0, false, &locks};
        source().onEvent(e, core::model::TriggerMode::OneShot);
    }
    engine::trigger::TrackSource& source() { return sources->sources[static_cast<std::size_t>(track)]; }
};
} // namespace

TEST_CASE("modulation.sink: a strip value with a step lock, an LFO and a touch strip USER item", "[modulation][motion]")
{
    MotionStep f;
    const int track = MotionStep::track;
    REQUIRE(f.mixer->applied(track).volume == 40);
    ModValue lfo; // +10/127 of the range
    lfo.offset01 = 10.0f / 127.0f;
    f.sink.modulate(track, id("track.volume"), lfo);
    CHECK(f.mixer->applied(track).volume == 50); // around the lock, not the stored 100
    ModValue user = lfo; // + a USER item at the middle: absolute, then the LFO on top
    user.hasOverride = true;
    user.override01 = 0.5f;
    f.sink.modulate(track, id("track.volume"), user);
    CHECK(f.mixer->applied(track).volume == 74); // round(63.5 + 10), whatever the lock or the stored value
    user.offset01 = 1.0f;
    f.sink.modulate(track, id("track.volume"), user);
    CHECK(f.mixer->applied(track).volume == 127); // one clamp
    f.sink.release(track, id("track.volume"));
    CHECK(f.mixer->applied(track).volume == 40); // back to the lock
}
