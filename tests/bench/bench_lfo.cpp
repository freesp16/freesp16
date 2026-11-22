// Track LFO + modulation matrix benchmarks (performance budget): ≤ 150 cycles per LFO per
// block + ≤ 80 per applied destination; all 16 LFOs active ≤ 4 k cycles per 64-frame block (0.6 % of 700 k). One
// iteration = one block: LfoStage::process (restarts from this block's triggers, 16 advanceBlock calls, 16
// ModMatrix::set) + ModMatrix::applyBlock (8 FX knob destinations through FxSlotBank::setModulation into a running
// gainTest, 8 track destinations through a sink that stores the value).
//   bench_lfo         16 LFOs, all seven shapes, free and SYNC, triggers on 4 tracks per block
//   bench_lfo_stage   the 16 LFOs alone (LfoStage::process with its 16 ModMatrix::set): ≤ 150 cycles per LFO
//   bench_lfo_matrix  the matrix alone: 16 tracks × 4 sources on one destination each, all dirty, applied
// Host numbers are ns at 1 GHz (harness convention): relative only; A9 numbers come from scripts/emulator-bench.sh
// and the hardware.
#include "tests/bench/harness/Bench.h"

#include "tests/unit/fx/FxTestUtil.h"

#include "engine/modulation/LfoStage.h"
#include "engine/modulation/ModMatrix.h"

#include <array>
#include <memory>

namespace
{
using namespace engine::modulation;

struct StoreSink final : ParamStateSink
{
    std::array<float, engine::kTracks> value {};
    bool modulate(int track, core::ParamId, const ModValue& v) noexcept override
    {
        value[static_cast<std::size_t>(track)] = v.apply(0.5f);
        return true;
    }
    void release(int track, core::ParamId) noexcept override { value[static_cast<std::size_t>(track)] = 0.5f; }
};

struct Rig
{
    engine::fx::FxSlotBank bank;
    LfoStage stage;
    ModMatrix matrix;
    StoreSink sink;
    std::array<engine::trigger::TriggerEvent, 4> events {};
    engine::BlockContext ctx {44100.0, 64, 0, 0, 120, true, false, false, 0, {}, 0};

    void setUp()
    {
        fxtest::buildBank(bank);
        const int gt = engine::fx::typeIndexOf(bank.insertTypes(), "gainTest");
        stage.prepare(44100.0);
        matrix.prepare();
        const auto fxDest = fxtest::idOf("fx1.gainTest.gain");
        const core::ParamId trackDests[] = {fxtest::idOf("playback.pitch"), fxtest::idOf("track.volume"),
                                            fxtest::idOf("track.pan"), fxtest::idOf("env.attack")};
        for (int t = 0; t < engine::kTracks; ++t)
        {
            const auto sc = static_cast<std::uint16_t>(t);
            core::ParamId dest = trackDests[t & 3];
            if (t < 8)
            {
                bank.onParamChange(fxtest::change("fx1.type", static_cast<float>(gt + 1), sc));
                dest = fxDest;
            }
            stage.setParam(LfoField::Destination, sc, static_cast<float>(dest.value));
            stage.setParam(LfoField::Shape, sc, static_cast<float>(t % kLfoShapes));
            stage.setParam(LfoField::Speed, sc, static_cast<float>(20 + 6 * t));
            stage.setParam(LfoField::Sync, sc, (t & 1) != 0 ? 1.0f : 0.0f);
            stage.setParam(LfoField::Depth, sc, 100.0f);
            stage.setParam(LfoField::TriggerMode, sc, static_cast<float>(t % 3));
            stage.setParam(LfoField::TriggerOn, sc, static_cast<float>((t >> 1) & 1));
        }
    }

    void runBlock(bool apply = true) noexcept
    {
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            auto& e = events[i];
            e.track = static_cast<std::uint8_t>((ctx.blockIndex * 3 + i * 4) & 15);
            e.kind = (i & 1) != 0 ? engine::trigger::TriggerKind::Half : engine::trigger::TriggerKind::Full;
            e.source = engine::trigger::TriggerSource::Sequencer;
            e.frame = static_cast<std::uint8_t>(i * 16);
        }
        stage.process(ctx, 0, events, matrix);
        if (apply)
            matrix.applyBlock(bank, sink);
        ctx.ppqStart = ctx.ppqEnd;
        ctx.ppqEnd += 64.0 * 120.0 / (60.0 * 44100.0);
        ++ctx.blockIndex;
    }
};
} // namespace

BENCH(bench_lfo, 4000)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp();
    for (int i = 0; i < 100; ++i)
        rig->runBlock();
    state.run(
        [&]
        {
            rig->runBlock();
            bench::doNotOptimize(rig->sink.value[9]);
        });
}

BENCH(bench_lfo_stage, 16 * 150)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp();
    for (int i = 0; i < 100; ++i)
        rig->runBlock();
    state.run(
        [&]
        {
            rig->runBlock(false);
            bench::doNotOptimize(rig->stage.amount(9));
        });
}

BENCH(bench_lfo_matrix, 16 * 80)
{
    auto rig = std::make_unique<Rig>();
    rig->setUp();
    const auto pitch = fxtest::idOf("playback.pitch");
    float v = 0.0f;
    state.run(
        [&]
        {
            v = v > 0.5f ? -0.5f : v + 0.01f;
            for (int t = 0; t < engine::kTracks; ++t)
            {
                rig->matrix.set(t, pitch, ModSource::Lfo, ModKind::Offset, v);
                rig->matrix.set(t, pitch, ModSource::Transpose, ModKind::Offset, 0.125f);
                rig->matrix.set(t, pitch, ModSource::Ribbon, ModKind::Offset, -v);
                rig->matrix.set(t, pitch, ModSource::PitchBend, ModKind::Offset, 0.0f);
            }
            rig->matrix.applyBlock(rig->bank, rig->sink);
            bench::doNotOptimize(rig->sink.value[5]);
        });
}
