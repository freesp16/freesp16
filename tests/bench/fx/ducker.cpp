// Ducker bench (budget: 4k cycles per 64-frame stereo block on the Cortex-A9, worst
// case "retrigger in release"). One iteration = one block through Ducker::process with the trigger slot in
// the ProcessContext. Curved attack and release (a pow per 11-sample control point, ≤ 6 per block), a 1 ms attack,
// a 4 s release and a hit every 2nd block, so every block runs the curved envelope and every hit re-enters the attack
// from the release (two log10 for the phase). Desktop numbers are relative only (the A9 budget is confirmed on the device).
#include "tests/bench/harness/Bench.h"

#include "engine/fx/effects/Ducker.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace
{
using engine::fx::effects::Ducker;
constexpr int N = engine::kBlockFrames;
} // namespace

BENCH(fx_ducker, 4000)
{
    auto d = std::make_unique<Ducker>();
    d->prepare(44100.0, N);
    d->setParam(0, 0.001f); // 1 ms attack: it ends inside the block, the next hit finds the release
    d->setParam(1, 0.2f);   // curved
    d->setParam(2, 0.5f);   // 4 s release
    d->setParam(3, 0.8f);   // curved
    d->setParam(5, 0.8f);
    d->setOption(Ducker::kOptSource, 2);
    d->reset();
    alignas(16) float src[N], l[N], r[N];
    for (int i = 0; i < N; ++i)
        src[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
    engine::fx::ProcessContext hit{44100.0, 120.0, true, 1u << 2, {}, 0};
    hit.triggerOffset.fill(-1);
    hit.triggerOffset[2] = 17;
    engine::fx::ProcessContext none = hit;
    none.triggerMask = 0;
    int n = 0;
    state.run(
        [&]
        {
            std::memcpy(l, src, sizeof l);
            std::memcpy(r, src, sizeof r);
            d->process(l, r, (++n & 1) != 0 ? hit : none);
            bench::doNotOptimize(l[5]);
        });
}
