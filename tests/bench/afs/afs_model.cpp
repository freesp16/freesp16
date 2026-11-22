// AFS digital model cycle budget: 45k A9 cycles per 64-frame stereo block,
// worst case "4-pole ZDF ladder, 2× oversampled, HP on" — measured through the slot at the master point with the
// drive ramp re-armed, an envelope retrigger every block (the per-sample coefficient pass, never the settled fast
// path), resonance 90 % and the high-pass at 200 Hz. Host timings are relative; the device number comes from
// scripts/emulator-bench.sh afs_model / the hardware.
#include "tests/bench/harness/Bench.h"

#include "engine/EngineApi.h"
#include "engine/afs/AnalogFilterSlot.h"

#include <cmath>
#include <span>

BENCH(afs_model, 45000)
{
    using namespace engine::afs;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N];
    static AnalogFilterSlot slot; // ≈ 60 KB: not on the stack
    slot.setField(AfsField::Active, 1);
    slot.setField(AfsField::LpCutoff, 600);
    slot.setField(AfsField::Resonance, 920);
    slot.setField(AfsField::HpCutoff, 300);
    slot.setField(AfsField::EnvAmount, 60);
    slot.setField(AfsField::EnvDecay, 40);
    slot.prepare(44100.0, N);
    const engine::trigger::TriggerEvent ev {0, 100, 0, engine::trigger::TriggerKind::Full,
                                            engine::trigger::TriggerSource::Sequencer, 7, false, nullptr};
    int iter = 0;
    state.run(
        [&]
        {
            for (int i = 0; i < N; ++i)
            {
                l[i] = 0.6f * std::sin(0.05f * static_cast<float>(i + iter));
                r[i] = 0.5f * std::cos(0.031f * static_cast<float>(i + iter));
            }
            if ((++iter & 3) == 0)
                slot.setField(AfsField::Drive, (iter & 4) != 0 ? 700.0f : 300.0f); // the 100-sample drive ramp
            slot.beginBlock(std::span(&ev, 1), 0.0f);
            slot.processAt(Route::Master, -1, l, r, N);
            slot.endBlock();
            bench::doNotOptimize(l[3]);
        });
}

// The common case: the same chain with settled knobs and no envelope (the coefficient fast path: one set per block).
BENCH(afs_model_settled, 45000)
{
    using namespace engine::afs;
    constexpr int N = engine::kBlockFrames;
    alignas(16) float l[N], r[N];
    static AnalogFilterSlot slot;
    slot.setField(AfsField::Active, 1);
    slot.setField(AfsField::LpCutoff, 600);
    slot.setField(AfsField::Resonance, 920);
    slot.setField(AfsField::HpCutoff, 300);
    slot.setField(AfsField::Drive, 500);
    slot.prepare(44100.0, N);
    int iter = 0;
    state.run(
        [&]
        {
            for (int i = 0; i < N; ++i)
            {
                l[i] = 0.6f * std::sin(0.05f * static_cast<float>(i + iter));
                r[i] = 0.5f * std::cos(0.031f * static_cast<float>(i + iter));
            }
            ++iter;
            slot.beginBlock({}, 0.0f);
            slot.processAt(Route::Master, -1, l, r, N);
            slot.endBlock();
            bench::doNotOptimize(l[3]);
        });
}
