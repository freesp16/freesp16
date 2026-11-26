// SceneEditor and pattern chain benchmarks (Cortex-A9 performance budget: message-thread part of any
// edit ≤ 2 ms, a full scene copy on a worker ≤ 60 ms, the swap at the next block, chain player ≤ 50 cycles at a pattern
// end). Budgets are A9 cycles at 1 GHz; host numbers are relative only (hardware numbers are measured on a unit).
//
//   bench_edit_chain_block        audio thread, per 64-frame block: PatternChainPlayer::process (active chain, the
//                                 pattern-end test every block; a 1-step pattern at 300 BPM so ends are frequent)
//   bench_edit_chain_pattern_end  audio thread: onPatternEnd's decision alone (a chain of repeats: no request)
//   bench_edit_chain_pattern_end_switch  onPatternEnd + request + the switch it causes (switcher advance included)
//   bench_edit_apply_track_paste  audio thread: the block that drains a track paste's ParamChanges (≈ 50 messages from
//                                 the composition root's forwarders) minus an idle block (bench_edit_engine_idle_block)
//   bench_edit_swap_playing_tracks  block-start track permutation and a two-track model diff
//   bench_edit_scene_prepare      worker: preparePaste of a full scene (16 patterns × 16 tracks × 64 steps, every
//                                 third trigger locked) — the deep copy
//   bench_edit_scene_copy         message thread: COPY of that scene (the clipboard snapshot)
//   bench_edit_scene_install      message thread: installing a prepared scene paste = undo + redo of it (/2: one
//                                 exchange + its 712 change paths each) + the observer flush
//   bench_edit_track_seq_install  message thread: install of a track-with-sequences paste (16 pattern merges) as undo +
//                                 redo (/2)
#include "tests/bench/harness/Bench.h"

#include "core/commands/CommandBusAccess.h"
#include "core/edit/EditCommands.h"
#include "core/edit/SceneEditor.h"
#include "core/model/ModelOps.h"
#include "core/util/Realtime.h"
#include "engine/Engine.h"
#include "engine/scene/PatternChain.h"
#include "tests/fakes/FakeEngineBridge.h"
#include "ui/input/pads/PadController.h"
#include "ui/input/scenes/SceneSync.h"
#include "ui/screens/effects/ModelFxSlots.h"
#include "ui/shell/ModelParamAccess.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace
{
namespace ce = core::edit;
namespace es = engine::scene;
namespace m = core::model;

// A dense scene: every cell triggered, every third one locked (worst case for the copy).
void fill(m::Scene& s)
{
    for (auto& p : s.patterns)
    {
        p.length = 64;
        p.locks.clear();
        for (int st = 0; st < m::kSteps; ++st)
            for (int t = 0; t < m::kTracks; ++t)
            {
                auto& c = p.steps[static_cast<std::size_t>(st)][static_cast<std::size_t>(t)];
                c.trigger = 15;
                c.value2 = static_cast<std::int16_t>(1 + (st * 7 + t) % 127);
                if ((st + t) % 3 == 0)
                {
                    m::StepLocks l;
                    l.mask = 0b1011;
                    l.values[0] = static_cast<float>(st);
                    p.locks.push_back(l);
                    c.lockIndex = static_cast<std::uint16_t>(p.locks.size() - 1);
                }
            }
    }
    for (int t = 0; t < m::kTracks; ++t)
        s.tracks[static_cast<std::size_t>(t)].name = "TRACK NAME " + std::to_string(t);
}

struct Doc
{
    core::CommandBus bus;
    ce::InlineWorkerPool pool;
    std::unique_ptr<ce::SceneEditor> editor;
    Doc()
    {
        core::commands::replaceProject(bus, m::newDefaultProject());
        fill(core::commands::project(bus).scenes[0]);
        editor = std::make_unique<ce::SceneEditor>(bus, pool);
    }
};

struct ChainRig
{
    es::ScenePatternSwitcher sw;
    es::PatternChainPlayer player;
    es::ChainPlan plan {};
    engine::stages::BlockBuffers::Timing timing {};
    engine::BlockContext ctx {};
    std::int64_t block = 0;
    double ppqPerBlock = 0;
    ChainRig()
    {
        ppqPerBlock = 300.0 / (60.0 * 44100.0) * engine::kBlockFrames;
        ctx.sampleRate = 44100.0;
        ctx.frames = engine::kBlockFrames;
        ctx.bpm = 300.0;
        ctx.playing = true;
        plan.length = 4;
        plan.patterns = {0, 1, 1, 3};
        for (int p = 0; p < 4; ++p)
            sw.setPatternLength(0, p, 1);
    }
    void step() noexcept
    {
        ctx.startedThisBlock = block == 0;
        ctx.ppqStart = static_cast<double>(block) * ppqPerBlock;
        ctx.ppqEnd = static_cast<double>(block + 1) * ppqPerBlock;
        player.process(ctx, timing, sw, &plan);
        bench::doNotOptimize(sw.advance(ctx, timing));
        ++block;
    }
};
} // namespace

BENCH(bench_edit_chain_block, 400)
{
    ChainRig r;
    core::rt::ScopedRealtimeThread rt;
    state.run([&] { r.step(); });
#if ! defined(BENCH_STANDALONE)
    CHECK(r.player.active());
    CHECK(r.player.advances() > 0);
#endif
}

BENCH(bench_edit_chain_pattern_end, 50)
{
    // The decision alone: a chain of repeats (0→0→0→0) never requests, so the player stays active.
    ChainRig r;
    r.plan.patterns = {0, 0, 0, 0};
    r.step(); // installs the plan
    core::rt::ScopedRealtimeThread rt;
    state.run([&] {
        r.player.onPatternEnd(r.sw);
        bench::doNotOptimize(r.player.index());
    });
#if ! defined(BENCH_STANDALONE)
    CHECK(r.player.active());
#endif
}

BENCH(bench_edit_chain_pattern_end_switch, 400)
{
    // onPatternEnd + the request + the switch it causes (here immediate: a stopped context), 0→1→1→3.
    ChainRig r;
    r.step();
    engine::BlockContext stopped = r.ctx;
    stopped.playing = false;
    core::rt::ScopedRealtimeThread rt;
    state.run([&] {
        r.player.onPatternEnd(r.sw);
        bench::doNotOptimize(r.sw.advance(stopped, r.timing));
    });
#if ! defined(BENCH_STANDALONE)
    CHECK(r.player.active());
#endif
}

#if ! defined(BENCH_STANDALONE)
namespace
{
struct EngineRig
{
    std::unique_ptr<engine::Engine> engine;
    std::vector<engine::EngineMessage> paste; // what a track paste posts
    std::vector<std::variant<engine::ParamChange, engine::ObjectSwap>> swapEdits;
    std::vector<float> l = std::vector<float>(64), r = std::vector<float>(64);
    EngineRig()
    {
        engine::EngineOptions o;
        o.testTone = false;
        engine::EngineSetup setup {[](std::string_view id) { return core::builtinParams().idOf(id).value_or(core::ParamId {}); }};
        engine = std::make_unique<engine::Engine>(setup, o);
        engine->prepare(44100.0, 64);
        Doc d;
        fakes::FakeEngineBridge fake;
        core::commands::setBridge(d.bus, [&fake](const core::ChangeSet& c, const m::Project& p) {
            ui::input::pads::forwardTrackChanges(c, p, fake);
            ui::forwardModelChanges(c, p, fake);
            ui::fx::forwardFxChanges(c, p, fake);
        });
        d.editor->copy(ce::trackRef(0, 3, false));
        d.editor->paste(ce::trackRef(0, 4, false));
        paste = fake.posted;
        fake.posted.clear();
        ui::input::scenes::SceneSync scenes(core::builtinParams(), fake);
        core::commands::setBridge(d.bus,
                                  [&](const core::ChangeSet& c, const m::Project& p)
                                  {
                                      ui::input::pads::forwardTrackChanges(c, p, fake);
                                      ui::forwardModelChanges(c, p, fake);
                                      ui::fx::forwardFxChanges(c, p, fake);
                                      scenes.onChange(c, p);
                                  });
        d.editor->swapTracks(0, 3, 4, true);
        for (const auto& edit : fake.posted)
            swapEdits.emplace_back(std::get<engine::ParamChange>(edit));
        engine->bridge().post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
    }
    void block() noexcept
    {
        float* out[2] = {l.data(), r.data()};
        engine->process(out, 2, 64);
        bench::doNotOptimize(l[0]);
    }
};
} // namespace

BENCH(bench_edit_engine_idle_block, 200000)
{
    EngineRig e;
    state.run([&] { e.block(); });
}

BENCH(bench_edit_apply_track_paste, 200000)
{
    EngineRig e;
    INFO("messages " << e.paste.size());
    CHECK(e.paste.size() > 10);
    state.run([&] {
        for (const auto& msg : e.paste)
            e.engine->bridge().post(msg);
        e.block();
    });
}
BENCH(bench_edit_swap_playing_tracks, 200000)
{
    EngineRig e;
    REQUIRE(e.swapEdits.size() > e.paste.size());
    const auto postSwap = [&]
    {
        auto batch = std::make_unique<engine::TrackSwapBatch>(nullptr);
        batch->swaps.push_back({0, 3, 4, true});
        batch->edits = e.swapEdits;
        if (e.engine->bridge().post(engine::ObjectSwap{{engine::SwapSlot::TrackSwapBatch, 0, 0}, batch.get()}))
            batch.release();
    };
    state.run(
        [&]
        {
            postSwap();
            e.block();
            e.engine->bridge().retirePending();
        });
}
#endif

BENCH(bench_edit_scene_prepare, 60000000)
{
    Doc d;
    const auto clip = ce::snapshot(core::commands::project(d.bus), ce::sceneRef(0));
    state.run([&] {
        auto cmd = ce::preparePaste(clip, ce::sceneRef(1));
        bench::doNotOptimize(cmd.get());
        // The command owns the prepared 2 MB scene until it is installed; dropping it here frees it (worker-side cost,
        // like the allocation of the copy).
    });
}

BENCH(bench_edit_scene_copy, 2000000)
{
    Doc d;
    state.run([&] { d.editor->copy(ce::sceneRef(0)); });
}

BENCH(bench_edit_scene_install, 4000000)
{
    Doc d;
    d.editor->copy(ce::sceneRef(0));
    d.editor->paste(ce::sceneRef(1));
    state.run([&] {
        d.bus.undo();
        d.bus.redo();
        core::commands::flushNotifications(d.bus); // the once-per-loop observer batch (sort + dedupe of the paths)
    });
#if ! defined(BENCH_STANDALONE)
    CHECK(core::commands::project(d.bus).scenes[1] == core::commands::project(d.bus).scenes[0]);
#endif
}

BENCH(bench_edit_track_seq_install, 4000000)
{
    Doc d;
    d.editor->copy(ce::trackRef(0, 2, true));
    d.editor->paste(ce::trackRef(0, 9, true));
    state.run([&] {
        d.bus.undo();
        d.bus.redo();
        core::commands::flushNotifications(d.bus); // the once-per-loop observer batch (sort + dedupe of the paths)
    });
}
