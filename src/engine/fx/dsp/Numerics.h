// Shared numerics of the effects framework and its DSP kit (engine/fx/dsp/; research
// docs/effects/numerics.md and effectbase.md, CC BY 4.0).
//
// Products are never fused (FX-030: out = f32(f32(x·dry) + f32(copy·wet)), two roundings). The engine module compiles
// with -ffp-contract=fast, and engine/CMakeLists.txt globs every source of the module, so a per-folder
// `-ffp-contract=off` is not available to this folder. Instead every function body whose result must stay non-fused
// starts with APP_FX_NOFUSE (a statement-scoped pragma): clang (the host, Apple silicon, whose fmla/fmadd fuse) honours
// it; on the device (ARMv7 VFPv3 + NEON v1) no fused instruction exists — GCC contracts to vmla/vmls at most, which
// round the product before the sum (chained), so the result is the same as separate operations. NEON code uses
// vmulq + vaddq, never vfmaq.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__clang__)
#define APP_FX_NOFUSE _Pragma("clang fp contract(off)")
#else
#define APP_FX_NOFUSE
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define APP_FX_HAVE_NEON 1
#else
#define APP_FX_HAVE_NEON 0
#endif

namespace engine::fx::dsp
{
constexpr float kEps = 1.1920929e-07f; // FLT_EPSILON: every |a − b| < ε test of the reference wrapper

// |f32(a − b)| < ε (the reference's "near" test).
inline bool nearlyEqual(float a, float b) noexcept
{
    float d = a - b;
    if (d <= 0.0f)
        d = -d;
    return d < kEps;
}

inline float absf(float x) noexcept
{
    return x < 0.0f ? -x : x;
}

inline std::uint32_t bitsOf(float x) noexcept
{
    std::uint32_t u;
    std::memcpy(&u, &x, sizeof u);
    return u;
}

// N_shell = trunc(f32(f32(3.3333333 · 0.001) · fs)): 147 @ 44.1 kHz, 160 @ 48 kHz, 320 @ 96 kHz (effect-shell.md).
inline int shellFadeSamples(double fs) noexcept
{
    return static_cast<int>((3.3333333f * 0.001f) * static_cast<float>(fs));
}
// N_mute = roundToInt(f32(f32(fs) · 0.001) · 3), half to even: 132 / 144 / 288 (fx-processors.md "MuteSmoother").
inline int muteFadeSamples(double fs) noexcept
{
    const float x = (static_cast<float>(fs) * 0.001f) * 3.0f;
    const auto t = static_cast<int>(x);
    const float frac = x - static_cast<float>(t);
    if (frac > 0.5f || (frac == 0.5f && (t & 1) != 0))
        return t + 1;
    return t;
}
} // namespace engine::fx::dsp
