// Helpers of the modulation-FX unit tests (Chorus, Flanger, Phaser): seeded noise, a direct effect runner with a tempo,
// and a bit comparison — no registry, so the NEON test file can use them in the device NEON target too.
#pragma once

#include "engine/EngineApi.h"
#include "engine/fx/EffectBase.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace modtest
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

inline engine::fx::ProcessContext context(double fs, double bpm = 120.0)
{
    engine::fx::ProcessContext ctx{fs, bpm, true, 0u, {}, 0};
    ctx.triggerOffset.fill(-1);
    return ctx;
}

// Runs blocks of (inL, inR) through e.process; returns L then R per block: out[2·b·N …] = L of block b,
// out[(2b + 1)·N …] = R. `perBlock(b)` (optional) runs before block b.
template <typename F>
std::vector<float> runDirect(engine::fx::EffectBase& e, const std::vector<float>& inL, const std::vector<float>& inR,
                             double fs, double bpm, F&& perBlock)
{
    REQUIRE(inL.size() == inR.size());
    const int blocks = static_cast<int>(inL.size()) / N;
    std::vector<float> out(inL.size() * 2);
    auto ctx = context(fs, bpm);
    alignas(16) float l[N], r[N];
    for (int b = 0; b < blocks; ++b)
    {
        perBlock(b, ctx);
        std::memcpy(l, inL.data() + b * N, sizeof l);
        std::memcpy(r, inR.data() + b * N, sizeof r);
        e.process(l, r, ctx);
        std::memcpy(out.data() + 2 * b * N, l, sizeof l);
        std::memcpy(out.data() + (2 * b + 1) * N, r, sizeof r);
    }
    return out;
}

inline std::vector<float> runDirect(engine::fx::EffectBase& e, const std::vector<float>& inL,
                                    const std::vector<float>& inR, double fs, double bpm = 120.0)
{
    return runDirect(e, inL, inR, fs, bpm, [](int, engine::fx::ProcessContext&) {});
}

inline bool sameBits(const float* a, const float* b, std::size_t n)
{
    return std::memcmp(a, b, n * sizeof(float)) == 0;
}

inline bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && sameBits(a.data(), b.data(), a.size());
}

// The lag (samples) by which R leads L: argmin over lag in [minLag, maxLag] of sum_t |R[t] − L[t + lag]|.
inline int bestLag(const std::vector<float>& dl, const std::vector<float>& dr, int from, int window, int maxLag,
                   int minLag = 0)
{
    int best = -1;
    double bestErr = 1e300;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double e = 0;
        for (int t = from; t < from + window; ++t)
            e += std::fabs(static_cast<double>(dr[static_cast<std::size_t>(t)]) -
                           static_cast<double>(dl[static_cast<std::size_t>(t + lag)]));
        if (e < bestErr)
        {
            bestErr = e;
            best = lag;
        }
    }
    return best;
}
} // namespace modtest
