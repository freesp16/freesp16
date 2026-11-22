// Distortion (spec 02-features/effects/distortion.md FX-1020…1024, FX-1060/1061, quirks FX-1080…1082; research
// docs/effects/distortion.md F069–F074, CC BY 4.0). Operation for operation as the MIT model tools/fx/distortion.py
// (classes Kernel = the reference's c_distortion, Distortion) in the Reference quirk profile, so tests/golden/distortion
// matches it through the shell.
//
//   in → [1 HPF lo freq] → [2 LPF hi freq] → [3 stereo-linked auto-gain] → [4 drive + soft clip] → [5 ×0.7]
//      → [6 LPF tone] → wet            (each stage over the whole block; the shell crossfades and adds the 10 Hz HPF)
//   one-poles   a = exp(−2π·fc/fs) (float32 argument, exp in double rounded once), b = 1 − a, fc ∈ [50, 20000] Hz;
//               HPF: lp = b·x + a·lp, y = x − lp; LPF: y = b·x + a·y                                   (FX-1020)
//   auto-gain   g ∈ [1, 100] shared by L and R; inc e-fold per 100 ms, dec per 3 ms, hold fs/50; T = 0 is off (FX-1021)
//   clipper     u = clamp(x·drive, ±1); c = u·(u·u); s = c·(−0.5) + u·1.5; t = (s ≥ 0 ? 1 − s : s + 1)³;
//               w = blend·t; y = f32(f32(w·c) + f32((1 − w)·s))·0.7  — the branch-free form of the reference's
//               clamp branches (bit-identical: at u = ±1 the formula gives s = c = ±1, t = 0)          (FX-1022/1023)
// Parameters apply at once (coefficients jump, FX-1020); every setter skips an unchanged value, as the kernel's.
//
// Knobs k: 0 drive (40v dB), 1 tone (output LPF), 2 lo freq (input HPF), 3 hi freq (pre-clip LPF), 4 dynamics
// (threshold), 5 mix (the shell's). Page order: drive, lo freq, hi freq, dynamics, tone, mix. No options, no tempo,
// no retrigger, no oscillator (the reference builds one it never runs, FX-1061).
//
// Quirk profile (FX-052; the Reference profile sets every bit):
//   kDistQuirkFixed44k     the reference builds and re-initialises its kernel at a literal 44,100 Hz whatever fs is
//                          (research distortion.md "Lifecycle", F073; our quirk bit); Sixteen: the real fs.
//   kDistQuirkCtorCoefs    FX-1082: the kernel's initial coefficients come from the constructor's −2π·50 / −2π·20000
//                          literals, 1 ulp off the setters' values; Sixteen: the setters' formula.
// FX-1080 (dead zone at ≥ 20 dB drive) is kept as character in both profiles; FX-1081 (initial threshold 0.1) is
// unreachable (the shell always sends the parameters first, FX-056).
//
// NEON: the three one-poles run L/R in two lanes, the clipper 4 samples per vector; the auto-gain is scalar
// and branchy. Budget 14k cycles / 64-frame block on the A9, the dearest insert (FX-051; tests/bench/fx/distortion.cpp).
#pragma once

#include "engine/fx/EffectBase.h"

#include <cstdint>

namespace engine::fx
{
constexpr std::uint32_t kDistQuirkFixed44k = 1u << 0;  // research F073 (our quirk bit)
constexpr std::uint32_t kDistQuirkCtorCoefs = 1u << 1; // FX-1082

class Distortion final : public EffectBase
{
public:
    void prepare(double sampleRate, int maxBlock) override;
    void reset() noexcept override; // FX-1060: full kernel re-init, then the six stored values again
    void setParam(int index, float value01) noexcept override;
    void setOption(int which, int value) noexcept override; // kClearState only
    void setQuirkProfile(std::uint32_t mask) noexcept override { quirks_ = mask; }
    void process(float* left, float* right, const ProcessContext&) noexcept override;

    void useScalarKernels(bool on) noexcept { scalar_ = on; } // test hook (neon_vs_scalar_distortion)
    float autoGain() const noexcept { return g_; }
    int kernelRate() const noexcept { return kfs_; }

    // The clipper of one sample (FX-1022/1023), exposed for tests.
    static float shape(float y, float drive, float blend) noexcept;

private:
    struct OnePole
    {
        float p = 0.0f, fc = 0.0f, a = 0.0f, b = 0.0f; // change copy, clamped cutoff, coefficients
        float z[2]{};
    };
    void initKernel(int fs) noexcept; // 0xfa36c
    void setDriveDb(float db) noexcept;
    void setCutoff(OnePole& f, float hz) noexcept;
    void setThreshold(float v) noexcept;
    void apply(int k, float v) noexcept;
    float coefOf(float fc) const noexcept;

    void highPass(float* l, float* r) noexcept;
    void lowPass(OnePole& f, float* l, float* r) noexcept;
    void autoGainStage(float* l, float* r) noexcept;
    void clipStage(float* x) noexcept;

    double fs_ = 44100.0;
    int kfs_ = 44100;      // the kernel's rate (44,100 in the Reference profile)
    std::uint32_t quirks_ = 0;
    float stored_[6]{};    // the six stored slot-14 values (FX-1060)
    float invFs_ = 0.0f;
    float pDrive_ = 0.0f, drive_ = 1.0f, blend_ = 0.0f;
    float pThr_ = 0.1f, hi_ = 0.1f, lo_ = 0.0f;
    float g_ = 1.0f, inc_ = 1.0f, dec_ = 1.0f;
    int reload_ = 882, hold_ = 882;
    OnePole out_, hp_, pre_;
    bool scalar_ = false;
};
} // namespace engine::fx
