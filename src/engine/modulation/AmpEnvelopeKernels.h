// Gain-ramp kernels of the amp envelope (curves): a block's gain curve is written run by
// run (one run = the samples of one envelope segment inside the block), then applied to the planar stereo block once.
// Every kernel has a scalar reference (`scalar::`) and a NEON version (`neon::`, ARMv7 intrinsics subset), tested
// against each other (tests/unit/modulation/test_ampenv_kernels.cpp). Any n >= 0, any alignment (4-wide body plus a
// scalar tail); no allocation, no libm.
//
//   fillLinear     g[i] = a + d × (k0 + i)              k0 = samples into the segment + 1 (index-based: no drift)
//   fillGeometric  g[i] = a + t × ρ^(i + 1)             4 lanes seeded {tρ, tρ², tρ³, tρ⁴}, stepped by ρ⁴; the scalar
//                                                       reference runs the same four recurrences, so both agree to ulps
//   applyGain      l[i] *= g[i], r[i] *= g[i]
//   applyConstant  l[i] *= v, r[i] *= v
#pragma once

#include "engine/dsp/simd/Kernels.h" // APP_ENGINE_HAVE_NEON

namespace engine::modulation::envk
{
// Lane seeds of a geometric run: t × ρ^1…4 and the lane step ρ^4 (computed by the caller from its double state).
struct GeoSeed
{
    float lane[4];
    float step;
};

namespace scalar
{
void fillConstant(float* g, int n, float v) noexcept;
void fillLinear(float* g, int n, float a, float d, float k0) noexcept;
void fillGeometric(float* g, int n, float a, const GeoSeed& s) noexcept;
void applyGain(float* l, float* r, const float* g, int n) noexcept;
void applyConstant(float* l, float* r, float v, int n) noexcept;
} // namespace scalar

#if APP_ENGINE_HAVE_NEON
namespace neon
{
void fillConstant(float* g, int n, float v) noexcept;
void fillLinear(float* g, int n, float a, float d, float k0) noexcept;
void fillGeometric(float* g, int n, float a, const GeoSeed& s) noexcept;
void applyGain(float* l, float* r, const float* g, int n) noexcept;
void applyConstant(float* l, float* r, float v, int n) noexcept;
} // namespace neon
#endif

// A kernel set the envelope calls through (one indirect call per run, never per sample): NEON where available, the
// scalar reference on request (tests compare whole envelopes both ways).
struct KernelSet
{
    void (*fillConstant)(float*, int, float) noexcept;
    void (*fillLinear)(float*, int, float, float, float) noexcept;
    void (*fillGeometric)(float*, int, float, const GeoSeed&) noexcept;
    void (*applyGain)(float*, float*, const float*, int) noexcept;
    void (*applyConstant)(float*, float*, float, int) noexcept;
};
const KernelSet& scalarKernels() noexcept;
const KernelSet& activeKernels() noexcept; // NEON when compiled in, else the scalar reference
} // namespace engine::modulation::envk
