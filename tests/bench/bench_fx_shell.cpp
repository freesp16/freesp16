// FX framework benchmarks. One iteration = one 64-frame block.
//   bench_fx_idle_slot       one Off slot (Master): the idle branch                       ≤ 25 cycles
//   bench_fx_idle_bank       all 34 slots idle (16 track insert pairs, Send, Master)       ≤ 1 k
//   bench_fx_shell_running   one running slot, shell only (None, Mix 0.5: copy + mix)     ≤ 450
//   bench_fx_shell_hpf       one running slot with the 10 Hz wet high-pass               ≤ 900
//   bench_fx_shell_transition  a slot cycling activation/stop fades (ramps every block)   ≤ 900
//   bench_fx_insert_bypass   an insert slot crossfading its bypass (dry copy + N_mute)     ≤ 900
//   bench_fx_worst_case      34 running slots, all with the wet high-pass                 ≤ 31 k
// Host numbers are ns at 1 GHz (harness convention); A9 numbers come from scripts/emulator-bench.sh bench_fx
// (relative only) and from hardware (open).
#include "tests/bench/harness/Bench.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/FxSlotBank.h"
#include "engine/fx/effects/None.h"

#include <cmath>
#include <memory>

namespace
{
using namespace engine::fx;
constexpr int N = engine::kBlockFrames;

struct Buffers
{
    alignas(16) float l[N];
    alignas(16) float r[N];
    Buffers()
    {
        for (int i = 0; i < N; ++i)
        {
            l[i] = 0.5f * std::sin(0.1f * static_cast<float>(i));
            r[i] = 0.4f * std::cos(0.07f * static_cast<float>(i));
        }
    }
};

ProcessContext context()
{
    ProcessContext c{44100.0, 120.0, true, 0u, {}, 0};
    c.triggerOffset.fill(-1);
    return c;
}

const EffectDescriptor kPlain{.id = "none",
                              .name = "NONE",
                              .slots = kAllSlotKinds,
                              .params = {},
                              .options = {},
                              .visual = {},
                              .shell = {0, static_cast<std::uint8_t>(MixLaw::CrossfadeK5), false, false, false},
                              .create = &make<effects::None>};
const EffectDescriptor kHpf{.id = "noneHpf",
                            .name = "NONE HPF",
                            .slots = kAllSlotKinds,
                            .params = {},
                            .options = {},
                            .visual = {},
                            .shell = {0, static_cast<std::uint8_t>(MixLaw::CrossfadeK5), true, false, false},
                            .create = &make<effects::None>};

std::unique_ptr<FxProcessor> running(SlotKind kind, const EffectDescriptor& d, Buffers& b)
{
    const EffectDescriptor* types[] = {&d};
    auto p = std::make_unique<FxProcessor>();
    p->build(kind, types);
    p->prepare(44100.0, N);
    p->setParam(0, 5, 0.5f);
    p->requestType(0);
    const auto ctx = context();
    for (int k = 0; k < 8; ++k) // past the activation fade
        p->process(b.l, b.r, ctx);
    return p;
}

std::unique_ptr<FxSlotBank> bankOf(const EffectDescriptor& d, bool run)
{
    static EffectRegistry reg; // one registry per descriptor kind is enough for the benches
    if (reg.find(d.id) == nullptr)
        reg.add(d);
    auto bank = std::make_unique<FxSlotBank>();
    bank->build(reg, {});
    bank->prepare(44100.0, N);
    if (run)
        for (int s = 0; s < kFxSlots; ++s)
        {
            const auto& list =
                s < kInsertSlots ? bank->insertTypes() : (s == kSendSlot ? bank->sendTypes() : bank->masterTypes());
            const int t = typeIndexOf(list, d.id);
            bank->slot(s).setParam(t, 5, 0.5f);
            bank->slot(s).requestType(t);
        }
    return bank;
}

void runBank(FxSlotBank& bank, Buffers* tracks, Buffers& send, Buffers& master, const ProcessContext& ctx) noexcept
{
    for (int t = 0; t < engine::kTracks; ++t)
        bank.processInsert(t, tracks[t].l, tracks[t].r, ctx);
    bank.processSend(send.l, send.r, ctx);
    bank.processMaster(master.l, master.r, ctx);
}
} // namespace

BENCH(bench_fx_idle_slot, 25)
{
    auto bank = std::make_unique<FxSlotBank>();
    bank->build(EffectRegistry::builtin(), {});
    bank->prepare(44100.0, N);
    Buffers b;
    const auto ctx = context();
    state.run(
        [&]
        {
            bank->processMaster(b.l, b.r, ctx);
            bench::doNotOptimize(b.l[5]);
        });
}

BENCH(bench_fx_idle_bank, 1000)
{
    auto bank = std::make_unique<FxSlotBank>();
    bank->build(EffectRegistry::builtin(), {});
    bank->prepare(44100.0, N);
    auto tracks = std::make_unique<Buffers[]>(engine::kTracks);
    Buffers send, master;
    const auto ctx = context();
    state.run(
        [&]
        {
            runBank(*bank, tracks.get(), send, master, ctx);
            bench::doNotOptimize(master.l[5]);
        });
}

BENCH(bench_fx_shell_running, 450)
{
    Buffers b;
    auto p = running(SlotKind::Master, kPlain, b);
    const auto ctx = context();
    state.run(
        [&]
        {
            p->process(b.l, b.r, ctx);
            bench::doNotOptimize(b.l[5]);
        });
}

BENCH(bench_fx_shell_hpf, 900)
{
    Buffers b;
    auto p = running(SlotKind::Master, kHpf, b);
    const auto ctx = context();
    state.run(
        [&]
        {
            p->process(b.l, b.r, ctx);
            bench::doNotOptimize(b.l[5]);
        });
}

BENCH(bench_fx_shell_transition, 900)
{
    Buffers b;
    const EffectDescriptor* types[] = {&kHpf, &kHpf};
    auto p = std::make_unique<FxProcessor>();
    p->build(SlotKind::Master, types);
    p->prepare(44100.0, N);
    p->setParam(0, 5, 0.5f);
    p->setParam(1, 5, 0.7f);
    p->requestType(0);
    const auto ctx = context();
    int block = 0, next = 1;
    state.run(
        [&]
        {
            if (++block % 4 == 0) // a type change every 4 blocks: stop fade, swap, activation fade
            {
                p->requestType(next);
                next ^= 1;
            }
            p->process(b.l, b.r, ctx);
            bench::doNotOptimize(b.l[5]);
        });
}

BENCH(bench_fx_insert_bypass, 900)
{
    Buffers b;
    auto p = running(SlotKind::Insert, kPlain, b);
    const auto ctx = context();
    int block = 0;
    bool on = false;
    state.run(
        [&]
        {
            if (++block % 3 == 0) // keep the N_mute crossfade running
            {
                on = ! on;
                p->setBypass(on);
            }
            p->process(b.l, b.r, ctx);
            bench::doNotOptimize(b.l[5]);
        });
}

BENCH(bench_fx_worst_case, 31000)
{
    auto bank = bankOf(kHpf, true);
    auto tracks = std::make_unique<Buffers[]>(engine::kTracks);
    Buffers send, master;
    const auto ctx = context();
    for (int k = 0; k < 8; ++k)
        runBank(*bank, tracks.get(), send, master, ctx);
    state.run(
        [&]
        {
            runBank(*bank, tracks.get(), send, master, ctx);
            bench::doNotOptimize(master.l[5]);
        });
}
