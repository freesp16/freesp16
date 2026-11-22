// Pattern chains (SCN-030/031): the builder, the player against the switcher's boundary contract (switches land exactly
// on pattern ends for every PATTERN QUANTIZE unit, no extra repeat, tempos and lengths fuzzed), and inside the engine:
// a chain 1→2→2→4 switches at the exact pattern-end samples (offline render), a manual selection ends it.
#include "core/edit/PatternChainBuilder.h"
#include "engine/scene/PatternChain.h"
#include "tests/unit/engine/EngineTestUtil.h"
#include "tests/unit/scene/SceneTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <utility>
#include <vector>

namespace ce = core::edit;
namespace es = engine::scene;

namespace
{
es::ChainPlan plan(int scene, std::initializer_list<int> patterns)
{
    es::ChainPlan p {};
    p.scene = static_cast<std::uint8_t>(scene);
    for (const int x : patterns)
        p.patterns[p.length++] = static_cast<std::uint8_t>(x);
    return p;
}

// The player + switcher on a drift-free clock; records the pattern of every pass and where each pass started.
struct ChainRig
{
    es::ScenePatternSwitcher sw;
    es::PatternChainPlayer player;
    scenetest::Clock clock;
    engine::stages::BlockBuffers::Timing timing {};
    scenetest::FakeTransport transport;
    scenetest::FakeMixer mixer;
    scenetest::FakeEvents events;
    const es::ChainPlan* installed = nullptr;
    struct Pass
    {
        int pattern;
        double startPpq;
    };
    std::vector<Pass> passes;
    std::vector<std::int64_t> switchFrames;

    void step()
    {
        const auto b = clock.block;
        timing.preCount = clock.preCount;
        const engine::BlockContext ctx = clock.next();
        player.process(ctx, timing, sw, installed);
        const auto r = sw.process(ctx, timing, transport, mixer, events);
        if (r.switched)
            switchFrames.push_back(b * engine::kBlockFrames + r.frame);
        if (ctx.playing && (passes.empty() || passes.back().startPpq != timing.patternStartPpq ||
                            passes.back().pattern != timing.pattern))
            passes.push_back({timing.pattern, timing.patternStartPpq});
    }
    void runUntilPpq(double ppq)
    {
        while (clock.ppqAt(clock.block) < ppq)
            step();
    }
};
} // namespace

TEST_CASE("edit.chain: holding the current pattern and pressing others builds the chain (≤ 64, repeats)", "[edit]")
{
    ce::PatternChainBuilder b;
    CHECK_FALSE(b.add(3)); // not holding
    b.begin(2, 0);
    CHECK(b.holding());
    CHECK(b.add(1));
    CHECK(b.add(1));
    CHECK(b.add(3));
    const auto p = b.end();
    REQUIRE(p.has_value());
    CHECK(p->scene == 2);
    CHECK(p->length == 4);
    CHECK(std::vector<int>(p->patterns.begin(), p->patterns.begin() + 4) == std::vector<int> {0, 1, 1, 3});
    CHECK_FALSE(b.holding());
    b.begin(0, 5);
    CHECK_FALSE(b.end().has_value()); // only the held key: a plain press (select / cancel)
    b.begin(0, 0);
    for (int i = 0; i < 70; ++i)
        b.add(i & 15);
    CHECK(b.size() == 64);
    CHECK(b.end()->length == 64);
    CHECK(ce::PatternChainBuilder::clear().length == 0);
}

TEST_CASE("edit.chain: 1→2→2→4 advances exactly at each pattern end for STEP, BAR and PATTERN quantize", "[edit]")
{
    for (const auto unit : {es::SwitchUnit::Pattern, es::SwitchUnit::Step, es::SwitchUnit::Bar})
    {
        INFO("unit " << static_cast<int>(unit));
        ChainRig r;
        r.sw.setUnit(unit);
        r.sw.setPatternLength(0, 1, 12); // 3 quarters
        r.sw.setPatternLength(0, 3, 24); // 1.5 bars: BAR must not cut it at its first bar
        const auto p = plan(0, {0, 1, 1, 3});
        r.installed = &p;
        r.clock.play();
        r.runUntilPpq(4 + 3 + 3 + 6 + 4 + 0.5);
        REQUIRE(r.passes.size() >= 6);
        const int want[] = {0, 1, 1, 3, 0, 1};
        const double starts[] = {0, 4, 7, 10, 16, 20};
        for (int i = 0; i < 6; ++i)
        {
            CHECK(r.passes[static_cast<std::size_t>(i)].pattern == want[i]);
            CHECK(std::fabs(r.passes[static_cast<std::size_t>(i)].startPpq - starts[i]) < 1e-9);
        }
        // Switch frames: PPQ × 22 050 frames per quarter (120 BPM, 44.1 kHz); the 2→2 repeat is a wrap, no switch.
        CHECK(r.switchFrames == std::vector<std::int64_t> {4 * 22050, 10 * 22050, 16 * 22050, 20 * 22050});
        CHECK(r.player.active());
    }
}

TEST_CASE("edit.chain: fuzz: the chain never repeats or skips an entry and switches only on pass ends", "[edit]")
{
    scenetest::Clock seedClock;
    std::uint32_t s = 0x9e3779b9u;
    auto rnd = [&s](int n) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<int>(s % static_cast<std::uint32_t>(n));
    };
    for (int run = 0; run < 60; ++run)
    {
        ChainRig r;
        r.clock.sampleRate = run % 2 ? 48000.0 : 44100.0;
        r.clock.bpm = 40.0 + rnd(261); // 40…300 BPM
        r.sw.setUnit(static_cast<es::SwitchUnit>(rnd(3)));
        es::ChainPlan p {};
        p.length = static_cast<std::uint8_t>(2 + rnd(6));
        for (int i = 0; i < p.length; ++i)
            p.patterns[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(rnd(4));
        for (int q = 0; q < 4; ++q)
            r.sw.setPatternLength(0, q, 1 + rnd(64));
        // Start on the chain's first entry (the switch is immediate while stopped).
        r.sw.request(0, p.patterns[0]);
        r.step();
        r.installed = &p;
        r.clock.play();
        double total = 0;
        for (int i = 0; i < 3 * p.length; ++i)
            total += r.sw.patternLength(0, p.patterns[static_cast<std::size_t>(i % p.length)]) * 0.25;
        r.runUntilPpq(total + 0.01);
        INFO("run " << run << " bpm " << r.clock.bpm << " unit " << static_cast<int>(r.sw.unit()));
        REQUIRE(r.passes.size() >= static_cast<std::size_t>(3 * p.length));
        double expect = 0;
        for (int i = 0; i < 3 * p.length; ++i)
        {
            const int pat = p.patterns[static_cast<std::size_t>(i % p.length)];
            CHECK(r.passes[static_cast<std::size_t>(i)].pattern == pat);
            CHECK(std::fabs(r.passes[static_cast<std::size_t>(i)].startPpq - expect) < 1e-6);
            expect += r.sw.patternLength(0, pat) * 0.25;
        }
    }
    (void) seedClock;
}

TEST_CASE("edit.chain: a manual switch, another scene, a cleared plan or song mode end / stop the chain", "[edit]")
{
    SECTION("a user request pending at the pattern end wins and ends the chain")
    {
        ChainRig r;
        const auto p = plan(0, {0, 1});
        r.installed = &p;
        r.clock.play();
        r.runUntilPpq(5); // pattern 1 (index 1) plays
        r.sw.request(0, 7);
        r.runUntilPpq(20);
        CHECK_FALSE(r.player.active());
        CHECK(r.sw.state().pattern == 7);
        CHECK(r.switchFrames.size() == 2);
    }
    SECTION("a plan of another scene does nothing; an empty plan clears")
    {
        ChainRig r;
        const auto other = plan(3, {0, 1});
        r.installed = &other;
        r.clock.play();
        r.runUntilPpq(9);
        CHECK(r.switchFrames.empty());
        const auto mine = plan(0, {0, 2});
        r.installed = &mine;
        r.runUntilPpq(13);
        CHECK(r.sw.state().pattern == 2);
        const es::ChainPlan none {};
        r.installed = &none;
        r.runUntilPpq(30);
        CHECK(r.sw.state().pattern == 2);
        CHECK_FALSE(r.player.active());
    }
    SECTION("suspended (song mode) or switching blocked: no requests; the current pattern not in the chain enters at 1")
    {
        ChainRig r;
        const auto p = plan(0, {5, 6});
        r.installed = &p;
        r.player.setSuspended(true);
        r.clock.play();
        r.runUntilPpq(9);
        CHECK(r.switchFrames.empty());
        r.player.setSuspended(false);
        r.sw.setSwitchable(false);
        r.runUntilPpq(13);
        CHECK(r.switchFrames.empty());
        r.sw.setSwitchable(true);
        r.runUntilPpq(17);
        CHECK(r.sw.state().pattern == 5); // entered at the chain's first entry, on the pass end
        CHECK(r.switchFrames == std::vector<std::int64_t> {16 * 22050});
    }
    SECTION("stopped: no pattern end")
    {
        ChainRig r;
        const auto p = plan(0, {0, 1});
        r.installed = &p;
        for (int i = 0; i < 5000; ++i)
            r.step();
        CHECK(r.switchFrames.empty());
    }
}

namespace
{
core::ParamId id(const char* name)
{
    return core::builtinParams().idOf(name).value_or(core::ParamId {});
}

// Renders block by block and returns the SwitchDone events as (absolute frame, pattern).
std::vector<std::pair<std::int64_t, int>> renderSwitches(engine::Engine& e, std::int64_t frames)
{
    std::vector<std::pair<std::int64_t, int>> out;
    for (std::int64_t at = 0; at < frames; at += 64)
    {
        enginetest::render(e, 64, 64);
        engine::EngineEvent ev;
        while (e.bridge().popEvent(ev))
            if (ev.kind == engine::EngineEvent::Kind::SwitchDone)
                out.emplace_back(at + ev.step, ev.value & 15);
    }
    return out;
}

bool postPlan(engine::Engine& e, const es::ChainPlan& p)
{
    auto* obj = new es::ChainPlan(p);
    if (e.bridge().post(engine::ObjectSwap {es::kChainSlot, obj}))
        return true;
    es::destroyChainPlan(obj);
    return false;
}
} // namespace

TEST_CASE("edit.chain: in the engine a chain 1→2→2→4 switches at the exact pattern-end samples (offline render)",
          "[edit][engine]")
{
    auto e = enginetest::make(false);
    REQUIRE(e->bridge().post(engine::ParamChange {id("pattern.length"), 0 << 4 | 1, 12.0f, 0}));
    REQUIRE(postPlan(*e, plan(0, {0, 1, 1, 3})));
    REQUIRE(e->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0}));
    const auto sw = renderSwitches(*e, 400000);
    // Pattern 1: 88 200 frames (16 steps at 120 BPM); pattern 2: 66 150 (12 steps) twice; pattern 4: 88 200.
    const std::vector<std::pair<std::int64_t, int>> want {{88200, 1}, {220500, 3}, {308700, 0}, {396900, 1}};
    CHECK(sw == want);
    // Wall-clock overruns on a loaded host without an RT kernel (ASan, parallel ctest): ≤ 1 % of the blocks, the
    // margin tests/unit/perf/test_load_meter.cpp uses; the device budget is checked on hardware.
    CHECK(e->load().overruns <= 1 + e->load().blocks / 100);
    // A manual selection while the chain plays ends it: pattern 6 from the next pattern end, then it stays.
    REQUIRE(e->bridge().post(engine::ParamChange {id("scene.queue"), 0xFFFF, 5.0f, 0}));
    const auto after = renderSwitches(*e, 400000);
    REQUIRE_FALSE(after.empty());
    CHECK(after.front().second == 5);
    CHECK(after.size() == 1);
    // A new plan restarts chaining; the retired plans are freed on the message thread.
    REQUIRE(postPlan(*e, plan(0, {5, 2})));
    const auto again = renderSwitches(*e, 200000);
    REQUIRE_FALSE(again.empty());
    CHECK(again.front().second == 2);
    e->bridge().retirePending();
}
