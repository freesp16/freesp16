// Ducker (spec 02-features/effects/ducker.md): the Sixteen sync tables (FX-1210) and the sync attack
// length, both quirk profiles (FX-1280 edge trigger, FX-1225 clamp), idle pass-through (FX-1260), reset during a duck
// (FX-1261), the Source default (FX-1262), MIDI sources (FX-1241/1263), the catalogue, and the trigger path through
// the engine's TriggerRouter with 6 Duckers on one 16-track schedule (FX-1240…1243; the research's F201 analogue).
// Free-mode goldens against tools/fx/ducker.py: tests/golden/ducker.
#include "tests/unit/fx/FxTestUtil.h"
#include "tests/unit/trigger/TriggerRig.h"

#include "core/commands/fx/FxCommands.h"
#include "core/model/Project.h"
#include "core/util/Realtime.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/ducker_tables.h"
#include "engine/fx/effects/Ducker.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using engine::fx::ProcessContext;
using engine::fx::effects::Ducker;

namespace
{
constexpr int N = engine::kBlockFrames;

ProcessContext ctxFor(double bpm = 120.0, int track = -1, int offset = 0, std::uint16_t midi = 0)
{
    ProcessContext c{44100.0, bpm, true, 0u, {}, midi};
    c.triggerOffset.fill(-1);
    if (track >= 0)
    {
        c.triggerMask = 1u << track;
        c.triggerOffset[static_cast<std::size_t>(track)] = static_cast<std::int16_t>(offset);
    }
    return c;
}

struct Rig
{
    Ducker d;
    explicit Rig(std::uint32_t quirks = engine::fx::kQuirkProfileSixteen, double fs = 44100.0)
    {
        d.prepare(fs, N);
        d.setQuirkProfile(quirks);
        // the catalogue defaults (fx_ducker.cpp): 30 ms, linear, ≈ 300 ms, linear, depth 100 %
        d.setParam(0, 0.03f);
        d.setParam(1, 0.5f);
        d.setParam(2, 2999.0f / 79999.0f);
        d.setParam(3, 0.5f);
        d.setParam(5, 1.0f);
        d.reset();
    }
    // One block of DC 1.0 (left) / 0.5 (right); returns the left gains.
    std::vector<float> block(const ProcessContext& c)
    {
        alignas(16) float l[N], r[N];
        for (int i = 0; i < N; ++i)
        {
            l[i] = 1.0f;
            r[i] = 0.5f;
        }
        {
            core::rt::ScopedRealtimeThread rt;
            d.process(l, r, c);
        }
        return std::vector<float>(l, l + N);
    }
};

float minOf(const std::vector<float>& v)
{
    float m = 1e9f;
    for (float x : v)
        m = std::min(m, x);
    return m;
}
} // namespace

TEST_CASE("fx.ducker: the Sixteen sync tables by formula equal the catalogue's (FX-1210)", "[fx][ducker]")
{
    const engine::fx::dsp::ducker::SyncTables t;
    for (int k = 0; k < 128; ++k)
    {
        INFO("k " << k);
        CHECK(t.attack[k] == core::fx::duckerSixteenths(core::fx::Family::DuckerAttack, k));
        CHECK(t.release[k] == core::fx::duckerSixteenths(core::fx::Family::DuckerRelease, k));
    }
    CHECK(t.attack[0] == 0.0f);
    CHECK(t.attack[127] == 4.0f);
    CHECK(t.release[0] == 0.25f);
    CHECK(t.release[127] == 32.0f);
}

TEST_CASE("fx.ducker: sync attack length = q·T_A[k] ± 1 sample at 60, 120 and 999 BPM (FX-1210)", "[fx][ducker]")
{
    const engine::fx::dsp::ducker::SyncTables t;
    for (double bpm : {60.0, 120.0, 999.0})
        for (int k : {1, 10, 64, 100, 127})
        {
            INFO("bpm " << bpm << " k " << k);
            Rig r;
            r.d.setOption(Ducker::kOptSync, 1);
            r.d.setParam(0, static_cast<float>(k) / 127.0f);
            r.d.setParam(2, 0.0f); // the shortest release (0.25 sixteenth)
            r.d.setParam(1, 0.5f);
            const double q = 44100.0 * 0.25 * 60.0 / bpm;
            const double want = q * static_cast<double>(t.attack[k]);
            // fire at block 0, offset 0; the attack ends when the state turns to release
            auto c = ctxFor(bpm, 0, 0);
            int samples = 0, b = 0;
            r.block(c);
            for (; b < 4000 && r.d.state() == 1; ++b)
                r.block(ctxFor(bpm));
            samples = r.d.attackSamples();
            CHECK(std::abs(static_cast<double>(samples) - want) <= 1.0);
            CHECK(r.d.state() != 1);
        }
}

TEST_CASE("fx.ducker: an idle Ducker passes its input bit for bit and ducks only on its Source (FX-1260, FX-1243)",
          "[fx][ducker]")
{
    Rig r;
    r.d.setOption(Ducker::kOptSource, 4);
    alignas(16) float l[N], r2[N], l0[N], r0[N];
    for (int i = 0; i < N; ++i)
    {
        l[i] = l0[i] = std::sin(0.1f * static_cast<float>(i)) * 0.7f;
        r2[i] = r0[i] = -0.0f;
    }
    r.d.process(l, r2, ctxFor(120.0, 3, 0)); // another track's trigger
    CHECK(std::memcmp(l, l0, sizeof l) == 0);
    CHECK(std::memcmp(r2, r0, sizeof r2) == 0);
    CHECK(r.d.state() == 0);
    auto g = r.block(ctxFor(120.0, 4, 20));
    for (int i = 0; i < 20; ++i)
        CHECK(g[static_cast<std::size_t>(i)] == 1.0f); // the first `offset` samples pass (FX-1221)
    CHECK(g[20] < 1.0f);
    CHECK(r.d.telemetry(Ducker::kTelEnvelope) > 0.0f);
}

TEST_CASE("fx.ducker: reset re-applies the parameters and does not stop a duck in progress (FX-1261)", "[fx][ducker]")
{
    Rig a, b;
    a.block(ctxFor(120.0, 0, 0));
    b.block(ctxFor(120.0, 0, 0));
    a.d.reset();
    for (int k = 0; k < 10; ++k)
        CHECK(a.block(ctxFor()) == b.block(ctxFor()));
    CHECK(a.d.state() != 0);
}

TEST_CASE("fx.ducker: quirk_ducker FX-1280 — the reference misses a trigger in the block after one that fired",
          "[fx][ducker][quirk]")
{
    for (bool reference : {true, false})
    {
        INFO((reference ? "reference" : "sixteen"));
        Rig r(reference ? Ducker::kQuirkEdgeTrigger : 0u);
        r.d.setParam(0, 0.0f); // one-sample attack
        r.d.setParam(2, 0.5f); // 4 s release: the second hit retriggers from the release
        r.d.reset();
        r.block(ctxFor(120.0, 0, 0));
        r.block(ctxFor(120.0, 0, 0)); // consecutive block (index 1)
        const auto g1 = r.block(ctxFor());
        // the first hit took the gain to 0; in release it rises again. A second hit (Sixteen) re-enters the attack at
        // block 1 and pulls it back to the floor: block 2 is lower than without it.
        Rig ref(Ducker::kQuirkEdgeTrigger);
        ref.d.setParam(0, 0.0f);
        ref.d.setParam(2, 0.5f);
        ref.d.reset();
        ref.block(ctxFor(120.0, 0, 0));
        ref.block(ctxFor());
        const auto g1single = ref.block(ctxFor());
        if (reference)
            CHECK(g1 == g1single); // missed: identical to a single hit
        else
            CHECK(g1.back() < g1single.back());
    }
    // with one empty block in between both profiles fire (the goldens' patterns)
    for (std::uint32_t q : {Ducker::kQuirkEdgeTrigger, 0u})
    {
        Rig r(q);
        r.d.setParam(0, 0.0f);
        r.d.setParam(2, 0.5f);
        r.d.reset();
        r.block(ctxFor(120.0, 0, 0));
        r.block(ctxFor());
        const auto before = r.block(ctxFor()).back();
        const auto g = r.block(ctxFor(120.0, 0, 10));
        CHECK(before > 0.0f);
        CHECK(g[0] == 0.0f); // a retrigger in the release starts at once (the offset applies from idle only, FX-1221)
    }
}

TEST_CASE("fx.ducker: quirk_ducker FX-1225 — Sixteen clamps the gain at 0, the reference may dip below", "[fx][ducker][quirk]")
{
    // Depth 100 %, the default curves, a hit and a retrigger during the release (the golden ducker/defaults): the
    // reference's linear attack from the release ends at about −1e−10; Sixteen never goes below 0.
    for (bool reference : {true, false})
    {
        Rig r(reference ? Ducker::kQuirkNoClamp : 0u);
        float lowest = 1.0f;
        for (int b = 0; b < 80; ++b)
            lowest = std::min(lowest, minOf(r.block(b == 2 ? ctxFor(120.0, 0, 0) : (b == 40 ? ctxFor(120.0, 0, 17)
                                                                                                : ctxFor()))));
        INFO((reference ? "reference" : "sixteen") << " lowest " << lowest);
        if (reference)
            CHECK(lowest < 0.0f);
        else
            CHECK(lowest == 0.0f);
    }
}

TEST_CASE("fx.ducker: a MIDI-track Source never ducks unless Follow MIDI track is on (FX-1241, FX-1263)", "[fx][ducker]")
{
    Rig r;
    r.d.setOption(Ducker::kOptSource, 6);
    const std::uint16_t midi = 1u << 6;
    CHECK(minOf(r.block(ctxFor(120.0, 6, 0, midi))) == 1.0f);
    CHECK(r.d.state() == 0);
    r.d.setOption(Ducker::kOptFollowMidi, 1);
    CHECK(minOf(r.block(ctxFor(120.0, 6, 0, midi))) < 1.0f);
}

TEST_CASE("fx.ducker: catalogue — Insert only, page order, grids, SYNC forms, Source labels (FX-067/069/092)",
          "[fx][ducker]")
{
    const auto& reg = core::builtinParams();
    const auto* d = engine::fx::EffectRegistry::builtin().find("ducker");
    REQUIRE(d != nullptr);
    CHECK(d->slots == static_cast<std::uint8_t>(engine::fx::SlotKind::Insert));
    CHECK(d->params[4].empty());
    CHECK(d->visual == "duckEnvelope");
    CHECK(reg.find("send.ducker.attack") == nullptr);
    const auto* att = reg.find("fx1.ducker.attack");
    REQUIRE(att != nullptr);
    CHECK(att->defaultValue == 0.03f);
    char buf[32];
    att->format(att->defaultValue, buf, sizeof buf);
    CHECK(std::string(buf) == "30.0 ms");
    REQUIRE(reg.find("fx2.ducker.release.div") != nullptr);
    const auto* src = reg.find("fx1.ducker.type1");
    REQUIRE(src != nullptr);
    CHECK(src->enumLabels.size() == 16);
    CHECK(src->enumLabels[15] == "TRACK 16");
    const auto* curve = reg.find("fx1.ducker.attackCurve");
    REQUIRE(curve != nullptr);
    curve->format(0.0f, buf, sizeof buf);
    CHECK(std::string(buf) == "-100");
    CHECK(curve->bipolar);
    CHECK(reg.find("fx1.ducker.depth")->defaultValue == 1.0f);
}

TEST_CASE("fx.ducker: SelectFxType gives a new Ducker its own track as Source (FX-1262)", "[fx][ducker]")
{
    auto project = std::make_unique<core::model::Project>();
    core::model::ParamPath at{};
    at.scope = core::model::Scope::InsertFx;
    at.scene = 2;
    at.track = 11;
    at.slot = 1;
    core::SelectFxType(at, core::model::EffectType::Ducker).apply(*project);
    const auto& s = project->scenes[2].tracks[11].insertFx[1];
    CHECK(s.type == core::model::EffectType::Ducker);
    CHECK(s.type1 == 11);
    CHECK(s.params[0] == static_cast<double>(0.03f));
    CHECK(s.params[5] == 1.0);
}

TEST_CASE("fx.ducker: the Source option reaches the instance through the slot bank's routes", "[fx][ducker]")
{
    // the route of fx2.ducker.type1 is an Option with code kOptSource
    const auto id = fxtest::idOf("fx2.ducker.type1");
    engine::fx::FxSlotRoutes routes;
    const auto ids = fxtest::builtinIds();
    const auto ins = engine::fx::engineTypes(engine::fx::EffectRegistry::builtin(), engine::fx::SlotKind::Insert);
    const auto snd = engine::fx::engineTypes(engine::fx::EffectRegistry::builtin(), engine::fx::SlotKind::Send);
    const auto mst = engine::fx::engineTypes(engine::fx::EffectRegistry::builtin(), engine::fx::SlotKind::Master);
    routes.build(ins, snd, mst, ids, core::fx::internalIndexTable(core::builtinParams()));
    const auto& r = routes.find(id);
    CHECK(r.kind == engine::fx::FxRoute::Kind::Option);
    CHECK(r.index == Ducker::kOptSource);
    CHECK(r.unit == 1);
    const auto& s = routes.find(fxtest::idOf("fx1.ducker.sync"));
    CHECK(s.kind == engine::fx::FxRoute::Kind::Option);
    CHECK(s.index == Ducker::kOptSync);
    const auto& k = routes.find(fxtest::idOf("fx1.ducker.depth"));
    CHECK(k.kind == engine::fx::FxRoute::Kind::Knob);
    CHECK(k.index == 5);
}

TEST_CASE("fx.ducker: ducker_trigger_path — 16-track schedule, 6 Duckers, duck start = block start + offset",
          "[fx][ducker][trigger]")
{
    using engine::trigger::TriggerKind;
    using engine::trigger::TriggerSource;
    auto rig = trigtest::makeRig();
    engine::mixer::StripParams muted;
    muted.mute = true;
    rig->mixer.setStrip(0, 7, muted, 0);    // track 8 muted: its hits never duck (FX-1241)
    const std::uint16_t midiMask = 1u << 9; // track 10 is a MIDI track (engine midiTrackMask)
    struct Unit
    {
        int source;
        std::unique_ptr<Ducker> d;
        std::vector<float> out;
    };
    std::vector<Unit> units;
    for (int s : {0, 0, 3, 7, 9, 15}) // two Duckers follow track 1 (FX1 of one track, FX2 of another)
    {
        Unit u{s, std::make_unique<Ducker>(), {}};
        u.d->prepare(44100.0, N);
        u.d->setOption(Ducker::kOptSource, s);
        u.d->setParam(0, 0.0f);              // one-sample attack: the gain is 0 exactly at the trigger sample
        u.d->setParam(1, 0.5f);
        u.d->setParam(2, 9.0f / 79999.0f);   // 1 ms release
        u.d->setParam(3, 0.5f);
        u.d->setParam(5, 1.0f);
        u.d->reset();
        units.push_back(std::move(u));
    }
    // The schedule: per block, per track, events (kind, frame); every source track sounds at most every 3rd block.
    struct Ev
    {
        int block, track;
        TriggerKind kind;
        int frame;
        TriggerSource src = TriggerSource::Sequencer;
    };
    const std::vector<Ev> schedule = {
        {1, 0, TriggerKind::Full, 0},     {1, 3, TriggerKind::Half, 17},     {1, 7, TriggerKind::Full, 5},
        {1, 9, TriggerKind::Full, 5},     {1, 15, TriggerKind::LockOnly, 9}, {2, 4, TriggerKind::Full, 30},
        {4, 0, TriggerKind::Full, 12},    {4, 0, TriggerKind::Half, 40},     {4, 15, TriggerKind::Full, 63},
        {5, 3, TriggerKind::LockOnly, 2}, {7, 3, TriggerKind::Full, 62},     {7, 0, TriggerKind::LockOnly, 50},
        {8, 15, TriggerKind::Half, 1},    {10, 0, TriggerKind::Full, 33, TriggerSource::Pad},
        {10, 7, TriggerKind::Half, 0},    {11, 15, TriggerKind::Full, 0, TriggerSource::Retrigger},
        {13, 3, TriggerKind::Full, 8},    {13, 3, TriggerKind::Full, 4},     {13, 9, TriggerKind::Half, 60},
        {16, 0, TriggerKind::Full, 63},   {16, 15, TriggerKind::Full, 20},   {17, 11, TriggerKind::Full, 0},
    };
    const int blocks = 20;
    for (int b = 0; b < blocks; ++b)
    {
        for (const auto& e : schedule)
            if (e.block == b)
                rig->router.push(trigtest::Rig::event(e.track, e.kind, e.frame, e.src));
        rig->block();
        ProcessContext ctx{44100.0, 120.0, true, rig->ctx.triggerMask, rig->ctx.triggerOffset, midiMask};
        for (auto& u : units)
        {
            alignas(16) float l[N], r[N];
            for (int i = 0; i < N; ++i)
                l[i] = r[i] = 1.0f;
            {
                core::rt::ScopedRealtimeThread rt;
                u.d->process(l, r, ctx);
            }
            u.out.insert(u.out.end(), l, l + N);
        }
    }
    for (auto& u : units)
    {
        INFO("source track " << u.source + 1);
        std::vector<int> want;
        if (u.source != 7 && u.source != 9) // muted / MIDI sources never duck
            for (int b = 0; b < blocks; ++b)
            {
                int last = -1; // the router applies a block's events in frame order: the later one wins (FX-1242)
                for (const auto& e : schedule)
                    if (e.block == b && e.track == u.source &&
                        (e.kind == TriggerKind::Full || e.kind == TriggerKind::Half))
                        last = std::max(last, e.frame);
                if (last >= 0)
                    want.push_back(b * N + last);
            }
        std::vector<int> got;
        for (std::size_t i = 0; i < u.out.size(); ++i)
            if (u.out[i] == 0.0f && (i == 0 || u.out[i - 1] != 0.0f))
                got.push_back(static_cast<int>(i));
        CHECK(got == want);
        if (u.source == 7 || u.source == 9)
            CHECK(minOf(u.out) == 1.0f);
    }
}
