// FX memory budget: every type × slot pre-allocated (FX-005) must fit 24 MiB at ≤ 48 kHz. This builds the full 34-slot
// bank from stand-in effects with the real effects' buffer sizes (Insert Delay 1 s + 2500 frames, Chorus/Flanger
// ≈ 40 KB, Send Delay 2 × 194,500 frames, Reverb ≈ 0.4 MB, the rest small), allocated through the DSP kit's DelayLine,
// and measures the heap the bank holds.
// The real per-instance bytes of each registered effect are printed by test_effect_contract.cpp.
#include "tests/unit/fx/FxTestUtil.h"

#include "engine/fx/dsp/DelayLine.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

using namespace engine::fx;

namespace
{
// Ring length in frames at 48 kHz (scaled with fs); 0 = a small effect (filter states only).
template <int Frames48k, int Extra> class Footprint final : public EffectBase
{
public:
    void prepare(double fs, int) override
    {
        if (Frames48k > 0)
            ring_.prepare(static_cast<int>(static_cast<double>(Frames48k) * fs / 48000.0) + Extra);
    }
    void reset() noexcept override {}
    void setParam(int, float) noexcept override {}
    void setOption(int which, int v) noexcept override
    {
        if (which == kClearState && v != 0)
            ring_.clear();
    }
    void process(float*, float*, const ProcessContext&) noexcept override {}

private:
    dsp::DelayLine ring_;
    float state_[64]{}; // filter/coefficient state of a small effect
};

constexpr std::uint8_t kIns = 1, kBus = 2 | 4;
EffectDescriptor fp(std::string_view id, std::uint8_t slots, std::unique_ptr<EffectBase> (*create)())
{
    return {.id = id,
            .name = "FP",
            .slots = slots,
            .params = {},
            .options = {},
            .visual = {},
            .shell = {},
            .create = create};
}

std::size_t measure(double fs)
{
    EffectRegistry reg;
    reg.add(fp("chorus", kIns | kBus, &make<Footprint<5000, 0>>));
    reg.add(fp("flanger", kIns | kBus, &make<Footprint<5000, 0>>));
    reg.add(fp("eq2", kIns | kBus, &make<Footprint<0, 0>>));
    reg.add(fp("filter", kIns | kBus, &make<Footprint<0, 0>>));
    reg.add(fp("delay", kIns, &make<Footprint<48000, 2500>>));
    reg.add(fp("lofi", kIns | kBus, &make<Footprint<0, 0>>));
    reg.add(fp("distortion", kIns | kBus, &make<Footprint<0, 0>>));
    reg.add(fp("compressor", kIns | kBus, &make<Footprint<96, 0>>));
    reg.add(fp("phaser", kIns | kBus, &make<Footprint<0, 0>>));
    reg.add(fp("ducker", kIns, &make<Footprint<0, 0>>));
    reg.add(fp("delayBus", kBus, &make<Footprint<194500, 0>>));
    reg.add(fp("reverb", kBus, &make<Footprint<50000, 0>>));
    const std::size_t before = fxtest::heapInUse();
    std::size_t held = 0;
    {
        auto bank = std::make_unique<FxSlotBank>();
        bank->build(reg, {});
        REQUIRE(bank->prepare(fs, engine::kBlockFrames));
        CHECK(bank->insertTypes().size() == 10);
        CHECK(bank->sendTypes().size() == 10);
        held = fxtest::heapInUse() - before;
    }
    return held;
}
} // namespace

TEST_CASE("fx.memory: every type x slot pre-allocated fits 24 MiB at 48 kHz", "[fx][memory]")
{
    if (fxtest::heapInUse() == 0)
        SKIP("no heap statistics on this platform");
    const std::size_t m441 = measure(44100.0), m48 = measure(48000.0), m96 = measure(96000.0);
    std::printf("fx.memory: pre-allocated FX bank (stand-in buffer sizes): %.2f MiB @ 44.1 kHz, %.2f MiB @ 48 kHz, "
                "%.2f MiB @ 96 kHz\n",
                static_cast<double>(m441) / 1048576.0, static_cast<double>(m48) / 1048576.0,
                static_cast<double>(m96) / 1048576.0);
    CHECK(m48 <= 24u * 1024u * 1024u);
    CHECK(m441 <= m48);
    CHECK(m96 > m48); // 96 kHz exceeds the cap: the on-demand ring policy applies there
}

TEST_CASE("fx.memory: the framework's own footprint (test effects only)", "[fx][memory]")
{
    // Only None and the test effects (the real effects' buffers are the budget test's above).
    EffectRegistry reg;
    for (const auto& d : EffectRegistry::builtin().all())
        if (d.id == "none" || d.featureFlag == kTestEffectFlag)
            reg.add(d);
    const auto ids = fxtest::builtinIds();
    const auto k = core::fx::internalIndexTable(core::builtinParams());
    const std::size_t before = fxtest::heapInUse();
    auto bank = std::make_unique<FxSlotBank>();
    bank->build(reg, ids, fxtest::kTestFlags, k);
    bank->prepare(48000.0, engine::kBlockFrames);
    const std::size_t held = fxtest::heapInUse() - before;
    std::printf("fx.memory: framework (34 slots, None + GainTest per slot, routes): %.1f KiB (sizeof bank %zu, "
                "sizeof FxProcessor %zu, sizeof FxShell %zu)\n",
                static_cast<double>(held) / 1024.0, sizeof(FxSlotBank), sizeof(FxProcessor), sizeof(FxShell));
    CHECK(held < 1024u * 1024u);
}
