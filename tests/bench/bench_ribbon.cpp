// Touch strip benchmark (budget: processBlock ≤ 1.5k cycles with 8 USER items × 4 held
// tracks). One iteration = one engine block of the ribbon: a new strip position (the finger moves every block, the
// worst case: every contribution changes), collect (REPEAT), observe (the router's block events) and publish into a
// matrix. The matrix here is a stand-in with ModMatrix::set's shape (fixed 16 × 8 slots, linear search per set); the
// real ModMatrix::set / applyBlock costs are in bench_lfo (≤ 80 cycles per applied destination). Host
// numbers are ns at 1 GHz (relative only); device numbers come from scripts/emulator-bench.sh / the hardware (open).
//
//   bench_ribbon         USER1: 8 items × 4 held tracks, the position changing every block (32 Overrides)
//   bench_ribbon_pitch   PITCH: 4 held tracks, the position changing every block
//   bench_ribbon_repeat  REPEAT 1/32 on the running transport, 4 held tracks (a hit every 2–3 blocks)
#include "tests/bench/harness/Bench.h"

#include "engine/ribbon/RibbonEngine.h"

#include "core/params/ParamRegistry.h"

#include <array>
#include <memory>

namespace
{
using engine::modulation::ModKind;
using engine::modulation::ModSource;

struct SlotMatrix // ModMatrix::set / clear's shape: per track 8 destination slots, found by a linear search
{
    struct Slot
    {
        core::ParamId dest {};
        float value[4] {};
        std::uint8_t sources = 0, overrides = 0;
        bool dirty = false;
    };
    std::array<std::array<Slot, 8>, 16> slots {};
    bool set(int track, core::ParamId dest, ModSource src, ModKind kind, float v) noexcept
    {
        auto& t = slots[static_cast<std::size_t>(track & 15)];
        Slot* free = nullptr;
        for (auto& s : t)
        {
            if (s.dest == dest)
            {
                free = &s;
                break;
            }
            if (free == nullptr && ! s.dest.valid())
                free = &s;
        }
        if (free == nullptr)
            return false;
        const auto bit = static_cast<std::uint8_t>(1u << static_cast<int>(src));
        free->dest = dest;
        free->value[static_cast<int>(src)] = v;
        free->sources |= bit;
        free->overrides = static_cast<std::uint8_t>(kind == ModKind::Override ? (free->overrides | bit) : (free->overrides & ~bit));
        free->dirty = true;
        return true;
    }
    void clear(int track, core::ParamId dest, ModSource src) noexcept
    {
        for (auto& s : slots[static_cast<std::size_t>(track & 15)])
            if (s.dest == dest)
            {
                s.sources = static_cast<std::uint8_t>(s.sources & ~(1u << static_cast<int>(src)));
                s.dirty = true;
                if (s.sources == 0)
                    s.dest = {};
            }
    }
};

core::ParamId id(std::string_view s) { return core::builtinParams().idOf(s).value_or(core::ParamId {}); }

struct Rig
{
    engine::ribbon::RibbonEngine ribbon;
    SlotMatrix matrix;
    engine::BlockContext ctx {};
    std::array<engine::trigger::TriggerEvent, 4> presses {};
    struct Sink
    {
        int n = 0;
        bool push(const engine::trigger::TriggerEvent&) noexcept { return ++n > 0; }
    } sink;
    long block = 0;
    int position = 0;

    explicit Rig(engine::ribbon::Mode mode)
    {
        ribbon.bindParams([](std::string_view s) { return id(s); });
        ribbon.prepare(44100.0);
        ribbon.setMode(mode);
        // 8 USER1 items on distinct targets (any registered ids: the matrix stand-in does not care which)
        const auto& all = core::builtinParams().all();
        for (int i = 0; i < 8; ++i)
            ribbon.setItem(0, i, {core::ParamId {static_cast<std::uint16_t>(i * 3 % all.size())}, 0.1f * i, 1.0f - 0.1f * i});
        for (int t = 0; t < 4; ++t)
        {
            auto& e = presses[static_cast<std::size_t>(t)];
            e.track = static_cast<std::uint8_t>(t * 4);
            e.velocity = 100;
            e.kind = engine::trigger::TriggerKind::Full;
            e.source = engine::trigger::TriggerSource::Pad;
        }
        ribbon.observe(presses, {});
        ctx.sampleRate = 44100.0;
        ctx.frames = engine::kBlockFrames;
        ctx.bpm = 120.0;
        ctx.playing = true;
    }
    void step() noexcept
    {
        const double ppf = ctx.bpm / (60.0 * 44100.0);
        ctx.ppqStart = static_cast<double>(block) * engine::kBlockFrames * ppf;
        ctx.ppqEnd = ctx.ppqStart + engine::kBlockFrames * ppf;
        position = (position + 1) & 127;
        ribbon.onStrip(ribbon.mode() == engine::ribbon::Mode::Repeat ? 127 : position, true);
        ribbon.collect(ctx, sink);
        ribbon.observe({}, {});
        ribbon.publish(matrix);
        ++block;
    }
};

void run(bench::State& state, engine::ribbon::Mode mode)
{
    auto rig = std::make_unique<Rig>(mode);
    for (int i = 0; i < 200; ++i)
        rig->step();
    state.run(
        [&]
        {
            rig->step();
            bench::doNotOptimize(rig->matrix.slots.data());
            bench::doNotOptimize(rig->sink.n);
        });
}
} // namespace

BENCH(bench_ribbon, 1500) { run(state, engine::ribbon::Mode::User1); }
BENCH(bench_ribbon_pitch, 1500) { run(state, engine::ribbon::Mode::Pitch); }
BENCH(bench_ribbon_repeat, 1500) { run(state, engine::ribbon::Mode::Repeat); }
