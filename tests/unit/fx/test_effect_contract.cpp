// The effect contract, run for every registered effect × supported slot ×
// 44.1/48/96 kHz: no allocation after prepare (APP_RT_CHECKS aborts), kClearState + reset is bit-equal to a fresh
// instance, no NaN/Inf from ±1 noise at parameters 0, 1 and seeded random, declared latency = measured impulse delay,
// deterministic, parameter ids resolve (insert ParamSpecs lockable, bus ones not), bytes per instance reported.
// A new effect is picked up from the registry with no edit here.
#include "tests/unit/fx/FxTestUtil.h"

#include "core/util/Realtime.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/GainTest.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace engine::fx;

namespace
{
constexpr int N = engine::kBlockFrames;
constexpr std::string_view kAllFlags[] = {kTestEffectFlag, "fx.tremolo"};

std::vector<float> run(EffectBase& e, const std::vector<float>& inL, const std::vector<float>& inR, double fs)
{
    const int blocks = static_cast<int>(inL.size()) / N;
    std::vector<float> out(inL.size() * 2);
    ProcessContext ctx{fs, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    alignas(16) float l[N], r[N];
    core::rt::ScopedRealtimeThread rt;
    for (int b = 0; b < blocks; ++b)
    {
        std::memcpy(l, inL.data() + b * N, sizeof l);
        std::memcpy(r, inR.data() + b * N, sizeof r);
        e.process(l, r, ctx);
        std::memcpy(out.data() + 2 * b * N, l, sizeof l);
        std::memcpy(out.data() + (2 * b + 1) * N, r, sizeof r);
    }
    return out;
}

std::vector<float> noise(int n, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> v(static_cast<std::size_t>(n));
    for (auto& x : v)
        x = d(rng);
    return v;
}

bool finite(const std::vector<float>& v)
{
    for (float x : v)
        if (! std::isfinite(x))
            return false;
    return true;
}

void setAll(EffectBase& e, float v)
{
    for (int k = 0; k < 6; ++k)
        e.setParam(k, v);
}
} // namespace

TEST_CASE("fx.contract: every registered effect in every slot kind and sample rate", "[fx][contract]")
{
    const auto& reg = EffectRegistry::builtin();
    REQUIRE(reg.find("none") != nullptr);
    REQUIRE(reg.find("gainTest") != nullptr);
    const auto ids = fxtest::builtinIds();
    for (const auto& d : reg.all())
    {
        INFO("effect " << d.id);
        CHECK(validateDescriptor(d, ids).empty());
        for (auto kind : {SlotKind::Insert, SlotKind::Send, SlotKind::Master})
        {
            if (! allows(d.slots, kind))
                continue;
            for (auto prefix : slotPrefixes(kind)) // ParamSpecs resolve; inserts lockable, buses not (FX-078/094)
                for (auto p : d.params)
                    if (! p.empty())
                    {
                        const auto* s = core::builtinParams().find(slotParamId(prefix, p));
                        REQUIRE(s != nullptr);
                        CHECK(s->lockable == (kind == SlotKind::Insert));
                    }
            for (double fs : {44100.0, 48000.0, 96000.0})
            {
                INFO("fs " << fs);
                const std::size_t heap0 = fxtest::heapInUse();
                auto fresh = d.create();
                auto e = d.create();
                fresh->prepare(fs, N);
                e->prepare(fs, N);
                const std::size_t bytes = (fxtest::heapInUse() - heap0) / 2;
                std::printf("fx.contract: %s %s %.0f Hz: %zu bytes per instance\n", std::string(d.id).c_str(),
                            kind == SlotKind::Insert ? "insert" : (kind == SlotKind::Send ? "send" : "master"), fs,
                            bytes);
                const int blocks = 32;
                const auto inL = noise(blocks * N, 1), inR = noise(blocks * N, 2);
                // no NaN/Inf at 0, 1 and random parameters (each run under the RT check)
                for (float v : {0.0f, 1.0f})
                {
                    setAll(*e, v);
                    CHECK(finite(run(*e, inL, inR, fs)));
                }
                std::mt19937 rng(7);
                std::uniform_real_distribution<float> u(0.0f, 1.0f);
                for (int k = 0; k < 6; ++k)
                    e->setParam(k, u(rng));
                CHECK(finite(run(*e, inL, inR, fs)));
                // kClearState + reset = a fresh instance (same parameters)
                setAll(*e, 0.5f);
                setAll(*fresh, 0.5f);
                e->setOption(kClearState, 1);
                setAll(*e, 0.5f);
                e->reset();
                fresh->reset();
                const auto a = run(*e, inL, inR, fs), b = run(*fresh, inL, inR, fs);
                CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0); // deterministic too
                // declared latency = measured: first output sample of an impulse at its declared position
                auto lat = d.create();
                lat->prepare(fs, N);
                setAll(*lat, 0.5f);
                lat->reset();
                std::vector<float> imp(static_cast<std::size_t>(8 * N), 0.0f);
                imp[0] = 1.0f;
                const auto y = run(*lat, imp, imp, fs);
                int first = -1;
                for (int i = 0; i < 8 * N && first < 0; ++i)
                {
                    const int blk = i / N, off = i % N;
                    if (y[static_cast<std::size_t>(2 * blk * N + off)] != 0.0f)
                        first = i;
                }
                // The delays and the reverb (their mix laws, FX-624/828): the wet signal is an echo by design, so the
                // impulse appears at the effect's delay (beyond these 8 blocks at 0.5), never before the latency.
                const auto law = static_cast<MixLaw>(d.shell.mixLaw);
                if (law == MixLaw::DelayPiecewise || law == MixLaw::ReverbPiecewise)
                    CHECK((first < 0 || first >= lat->latencySamples()));
                else
                    CHECK(first == lat->latencySamples());
            }
        }
    }
}

TEST_CASE("fx.contract: the registry lists, flags and validation (FX-040, UID-207/831)", "[fx][contract]")
{
    EffectRegistry r;
    auto desc = [](std::string_view id, std::uint8_t slots, std::string_view flag = {})
    {
        return EffectDescriptor{.id = id,
                                .name = "X",
                                .slots = slots,
                                .params = {},
                                .options = {},
                                .visual = {},
                                .shell = {},
                                .create = &engine::fx::make<effects::GainTest>,
                                .featureFlag = flag};
    };
    // registration order deliberately scrambled
    CHECK(r.add(desc("phaser", kAllSlotKinds)));
    CHECK(r.add(desc("zeta", kAllSlotKinds)));
    CHECK(r.add(desc("reverb", 2 | 4)));
    CHECK(r.add(desc("ducker", 1)));
    CHECK(r.add(desc("chorus", kAllSlotKinds)));
    CHECK(r.add(desc("alpha", 1, "fx.tremolo")));
    CHECK(r.add(desc("delayBus", 2 | 4)));
    CHECK_FALSE(r.add(desc("chorus", 1))); // duplicate
    auto names = [](const std::vector<const EffectDescriptor*>& v)
    {
        std::string s;
        for (const auto* d : v)
            s += std::string(d->id) + " ";
        return s;
    };
    CHECK(names(listFor(r, SlotKind::Insert)) == "chorus phaser ducker zeta ");
    CHECK(names(listFor(r, SlotKind::Insert, kAllFlags)) == "chorus phaser ducker alpha zeta ");
    CHECK(names(listFor(r, SlotKind::Send)) == "delayBus reverb chorus phaser zeta ");
    CHECK(names(listFor(r, SlotKind::Master)) == "delayBus reverb chorus phaser zeta ");
    const auto errs = validate(r, {});
    CHECK(errs.size() >= 1); // the duplicate, and "no parameter and no option" for each
    CHECK(errs.front().find("duplicate") != std::string::npos);
    CHECK(slotParamId("fx2", "fx.tremolo.rate") == "fx2.tremolo.rate");
    CHECK(slotParamId("send", "tremolo.rate").empty());
    // the built-in registry validates against the built-in ParamSpecs
    CHECK(validate(EffectRegistry::builtin(), fxtest::builtinIds()).empty());
    // SELECT FX never lists the test effects (no flag given); the real effects are listed once registered
    for (const auto* d : listFor(EffectRegistry::builtin(), SlotKind::Insert))
        CHECK(d->featureFlag != kTestEffectFlag);
}
