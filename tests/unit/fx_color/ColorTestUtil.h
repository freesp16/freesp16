// Helpers of the colour-effect unit tests (LoFi, Distortion, oscillator): seeded signals, a direct effect runner and
// an FxShell runner (Insert slot) — no registry, so the NEON test files can use them in the device NEON target too.
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/FxShell.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

namespace colortest
{
constexpr int N = engine::kBlockFrames;

inline std::vector<float> noise(int n, std::uint32_t seed, float amp = 0.8f)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    std::uint32_t x = seed;
    for (auto& s : v)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = amp * (static_cast<float>(x >> 8) * (1.0f / 8388608.0f) - 1.0f);
    }
    return v;
}

inline engine::fx::ProcessContext context(double fs)
{
    engine::fx::ProcessContext ctx{fs, 120.0, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    return ctx;
}

// Runs `blocks` blocks of (inL, inR) through e.process (in place copies); returns L then R per block, interleaved by
// block: out[2·b·N …] = L of block b, out[(2b + 1)·N …] = R.
inline std::vector<float> runDirect(engine::fx::EffectBase& e, const std::vector<float>& inL,
                                    const std::vector<float>& inR, double fs)
{
    REQUIRE(inL.size() == inR.size()); // both channels are read for every block
    const int blocks = static_cast<int>(inL.size()) / N;
    std::vector<float> out(inL.size() * 2);
    const auto ctx = context(fs);
    alignas(16) float l[N], r[N];
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

// The left channel as one contiguous signal.
inline std::vector<float> leftOf(const std::vector<float>& interleavedBlocks)
{
    std::vector<float> l;
    for (std::size_t b = 0; b + 2 * N <= interleavedBlocks.size(); b += 2 * N)
        l.insert(l.end(), interleavedBlocks.begin() + static_cast<std::ptrdiff_t>(b),
                 interleavedBlocks.begin() + static_cast<std::ptrdiff_t>(b + N));
    return l;
}

// An effect in an Insert FxShell with the effects' common LoFi/Distortion policy (gate 0, 10 Hz HPF, crossfade mix).
struct ShellRig
{
    engine::fx::FxShell shell;
    alignas(16) float cl[N]{};
    alignas(16) float cr[N]{};
    double fs;
    ShellRig(engine::fx::EffectBase& e, double sampleRate, std::uint32_t quirks) : fs(sampleRate)
    {
        engine::fx::ShellPolicy pol{};
        pol.gateMode = 0;
        pol.mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5);
        pol.wetHighPass = true;
        e.prepare(fs, N);
        shell.prepare(e, pol, fs, engine::fx::SlotKind::Insert, cl, cr);
        shell.setQuirkProfile(quirks);
    }
    std::vector<float> run(const std::vector<float>& inL, const std::vector<float>& inR)
    {
        REQUIRE(inL.size() == inR.size()); // both channels are read for every block
    const int blocks = static_cast<int>(inL.size()) / N;
        std::vector<float> out(inL.size());
        const auto ctx = context(fs);
        alignas(16) float l[N], r[N];
        for (int b = 0; b < blocks; ++b)
        {
            std::memcpy(l, inL.data() + b * N, sizeof l);
            std::memcpy(r, inR.data() + b * N, sizeof r);
            shell.process(l, r, ctx);
            std::memcpy(out.data() + b * N, l, sizeof l);
        }
        return out; // left channel
    }
};

inline bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
} // namespace colortest
