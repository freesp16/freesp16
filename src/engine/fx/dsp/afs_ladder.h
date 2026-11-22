// The analog-filter slot's digital model kernels (spec 02-features/analog-filter-slot.md AFS-004, AFS-010).
// Our own design — the reference's filter is analog hardware and is not modelled by the
// research (docs/effects/README.md "Analog filter itself: out of scope", H008), so nothing here is reconstructed:
//
//   up      2× polyphase half-band FIR (63 taps, Kaiser β 8: ≤ −0.05 dB to 19.1 kHz, ≥ 81 dB rejection above
//           26.1 kHz at 44.1 kHz), even phase = pure delay, odd phase = 32 taps, 4 samples per vector
//   ladder  4-pole zero-delay-feedback ladder of TPT one-poles (Zavalishin): the linear feedback equation is solved
//           exactly per sample — u = (x − k·S)/(1 + k·G⁴) with S = (1 − G)·(G³·s1 + G²·s2 + G·s3 + s4), evaluated as
//           u = x·inv − kio·((G³·s1 + G²·s2) + (G·s3 + s4)), inv = 1/(1 + k·G⁴), kio = k·inv·(1 − G) (a short
//           dependency chain: the A9's NEON latency, not its throughput, bounds this loop) — and the nonlinearity is
//           applied in the forward pass: one saturator on the feedback node u and one on the inputs of stages 2–4 (one
//           per stage). The saturator is a C2 quintic soft clip (no division, no libm):
//               sat(v) = w·(1 − w²·(a − b·w²)),  w = clamp(v, ±c),  c = 2.25 (+7.0 dBFS), a = 2/(3c²), b = 1/(5c⁴)
//           — slope 1 at 0, max 1.2 at ±c with zero first and second derivative there (harmonics fall as 1/n⁴, so the
//           2× rate keeps a 5 kHz sine at full drive below −60 dB of aliasing). The inputs of stages 2–4 are outputs of
//           TPT one-poles fed by |u| ≤ 1.2 with a positive impulse response (pole (1 − g)/(1 + g) > 0: g < 1 up to
//           20 kHz at 2·fs), so they stay within ±1.2 < c and their saturators skip the clamp. Output y4, or y2 (the
//           2-pole tap; the feedback stays on y4). L/R run in two NEON lanes (float32x2_t), as the reference's IIR
//           kernels do.
//   down    the same half-band, decimating (even phase 0.5·z, odd phase 32 taps), 4 samples per vector
//   hp      2-pole Butterworth high-pass, a TPT state-variable filter (Zavalishin; good numerics down to 20 Hz in
//           float), L/R lanes, coefficients per block
//
// Every coefficient that varies per internal sample (G, 1 − G, k, 1/(1 + k·G⁴)) is computed once per sample by shared
// scalar code (the slot's coefficient pass, AfsModel) into arrays both kernels read, so the only lane work is the
// element-wise arithmetic and the scalar reference keeps the NEON order of operations: products never fused
// (vmul + vadd/vsub, never vmla/vfma: APP_FX_NOFUSE), the same association, the same clamp. NEON ≡ scalar bit for bit
// on normal inputs (tests/unit/afs/test_afs_neon.cpp; scripts/neon-tests.sh under qemu-arm). The implementation lives
// in engine/fx/effects/AfsKernels.cpp so that the device build compiles it with the effects' IEEE flags
// (-fno-unsafe-math-optimizations, src/engine/CMakeLists.txt).
//
// Budget (AFS digital model: 45k A9 cycles per 64-frame stereo block): 128 internal stereo samples ×
// (4 stages + solve + 4 saturators ≈ 160 cycles) ≈ 20k, half-bands 4-wide ≈ 6k, HP ≈ 1k, coefficient pass ≈ 5k.
#pragma once

#include "engine/fx/dsp/Numerics.h"

#include <cstdint>

namespace engine::fx::dsp::afs
{
constexpr int kBlock = 64;            // base-rate frames per call (engine block)
constexpr int kOver = 2 * kBlock;     // internal frames per block
constexpr int kHalfK = 16;            // half-band: 2K odd taps per phase, 4K − 1 = 63 taps
constexpr int kPhaseTaps = 2 * kHalfK; // 32
constexpr int kLatency = 2 * kHalfK - 1; // up (K) + down (K − 1) base-rate samples = 31

// The odd-phase taps of the half-band: kHalfbandOdd[j] = h[2j] of the 63-tap prototype (symmetric, Σ = 0.5); the
// upsampler uses 2·h. Generated once (Kaiser window, β 8) and committed as constants (AfsKernels.cpp).
extern const float kHalfbandOdd[kPhaseTaps];

// Saturator constants (see the file comment).
constexpr float kSatClamp = 2.25f;
constexpr float kSatA = 0.13168724f;  // 2 / (3 c²)
constexpr float kSatB = 0.0078036881f; // 1 / (5 c⁴)
constexpr float kSatMax = 1.2f;         // sat(±c)

// Scalar saturators (the reference; the NEON lanes do the same operations in the same order). saturateInner: for
// |v| ≤ c only (the inputs of stages 2–4, see the file comment).
inline float saturateInner(float w) noexcept
{
    APP_FX_NOFUSE
    const float w2 = w * w;
    const float t = kSatA - kSatB * w2;
    const float u = 1.0f - w2 * t;
    return w * u;
}
inline float saturate(float v) noexcept
{
    return saturateInner(v < -kSatClamp ? -kSatClamp : (v > kSatClamp ? kSatClamp : v));
}

// Per-internal-sample coefficients of one block (written by the coefficient pass, read by the ladder kernels).
struct alignas(16) LadderCoefs
{
    float g[kOver];     // G = g/(1 + g), g = tan(π·fc/fs2)
    float g2[kOver];    // G²
    float g3[kOver];    // G³
    float inv[kOver];   // 1/(1 + k·G⁴), k = feedback 0 … 4.6
    float kio[kOver];   // k·inv·(1 − G)
};

// Ladder state, L/R: s[stage][channel].
struct LadderState
{
    float s[4][2] {};
    void clear() noexcept
    {
        for (auto& st : s)
            st[0] = st[1] = 0.0f;
    }
};

// Half-band histories (per channel). Up: the last 2K − 1 inputs before the block, then the block (kBlock). Down: the
// odd-phase history (2K − 1 + kBlock) and the even-phase delay line (K − 1 + kBlock).
struct alignas(16) HalfbandState
{
    float up[2][kPhaseTaps - 1 + kBlock] {};
    float downOdd[2][kPhaseTaps - 1 + kBlock] {};
    float downEven[2][kHalfK - 1 + kBlock] {};
    void clear() noexcept;
};

// 2-pole high-pass, TPT SVF, L/R: hp = (x − r2g·s1 − s2)·h; b = g·hp + s1; s1 = g·hp + b; l = g·b + s2; s2 = g·b + l
// with g = tan(π·f/fs), r2g = 2R + g (R = 1/√2), h = 1/(1 + 2R·g + g²).
struct HighPass
{
    float g = 0, r2g = 0, h = 1;
    float s1[2] {}, s2[2] {};
    void setCutoff(double hz, double sampleRate) noexcept; // message or audio thread, once per block (libm, double)
    void clear() noexcept;
};

namespace scalar
{
// in: kBlock frames per channel; out: kOver interleaved L/R frames (out[2i + c]).
void upsample(HalfbandState& st, const float* inL, const float* inR, float* out) noexcept;
// in/out: kOver interleaved L/R frames; noise[2i + c] is added to the ladder input (−120 dBFS seed, kick-starts
// self-oscillation and keeps the states out of the denormal range).
void ladder(LadderState& st, const LadderCoefs& c, const float* in, const float* noise, float* out, bool twoPole) noexcept;
// in: kOver interleaved L/R frames; out: kBlock frames per channel.
void downsample(HalfbandState& st, const float* in, float* outL, float* outR) noexcept;
void highPass(HighPass& hp, float* l, float* r, int n) noexcept;
} // namespace scalar

#if APP_FX_HAVE_NEON
namespace neon
{
void upsample(HalfbandState& st, const float* inL, const float* inR, float* out) noexcept;
void ladder(LadderState& st, const LadderCoefs& c, const float* in, const float* noise, float* out, bool twoPole) noexcept;
void downsample(HalfbandState& st, const float* in, float* outL, float* outR) noexcept;
void highPass(HighPass& hp, float* l, float* r, int n) noexcept;
} // namespace neon
#endif
} // namespace engine::fx::dsp::afs
