// Filters of the effects DSP kit (consumed by the shell and the effects). Coefficients are
// computed in double on set-up and stored as float32; the per-sample paths are float32 and never fused (Numerics.h).
// Every filter is stereo (planar L/R) and processes in place, any n <= kMaxBlock.
//
//   Hpf1     the shell's first-order bilinear high-pass (FX-028, research effectbase.md "Post-stage", F147):
//            w = tan(π·fc/fs), w = (w·(1/2π))·2π, b0 = 1/(w + 1), b1 = −b0, a1 = (w − 1)/(w + 1);
//            y = f32(f32(f32(b0·x) + f32(b1·x₋₁)) − f32(a1·y₋₁)); fc clamped to [5 Hz, fs/2]. L and R run in two NEON
//            lanes; the scalar reference is bit-equal (tests/unit/fx/test_fx_dsp.cpp).
//   Biquad2  direct form 1, L/R in two NEON lanes (scalar reference bit-equal); RBJ cookbook designs.
//   OnePole  topology-preserving one-pole (low-pass and high-pass outputs), scalar.
//   Svf      topology-preserving state-variable filter (Simper/Zavalishin), low/band/high/notch outputs, scalar.
#pragma once

#include "engine/fx/dsp/Numerics.h"

namespace engine::fx::dsp
{
class Hpf1
{
public:
    void prepare(double fs) noexcept;  // fs/2 limit (as int fs >> 1), state zeroed
    void setCutoff(float fc) noexcept; // clamps to [5, fs/2], recomputes b0, b1, a1
    void zero() noexcept;
    void process(float* l, float* r, int n) noexcept;       // active path (NEON when available)
    void processScalar(float* l, float* r, int n) noexcept; // scalar reference
    float b0() const noexcept { return b0_; }
    float b1() const noexcept { return b1_; }
    float a1() const noexcept { return a1_; }

private:
    double fs_ = 44100.0;
    float fmax_ = 22050.0f, fc_ = 10.0f;
    float b0_ = 0, b1_ = 0, a1_ = 0;
    float v1_[2]{}, y1_[2]{};
};

struct BiquadCoefs
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // normalised by a0
    enum class Type : std::uint8_t
    {
        LowPass,
        HighPass,
        BandPass,
        Notch,
        AllPass,
        Peaking,
        LowShelf,
        HighShelf
    };
    // RBJ "Audio EQ Cookbook" in double; fc clamped to [5 Hz, 0.49·fs], q >= 0.01.
    static BiquadCoefs design(Type, double fs, double fc, double q, double gainDb = 0.0) noexcept;
};

class Biquad2
{
public:
    void setCoefs(const BiquadCoefs& c) noexcept { c_ = c; }
    const BiquadCoefs& coefs() const noexcept { return c_; }
    void zero() noexcept;
    void process(float* l, float* r, int n) noexcept;
    void processScalar(float* l, float* r, int n) noexcept;

private:
    BiquadCoefs c_{};
    float x1_[2]{}, x2_[2]{}, y1_[2]{}, y2_[2]{};
};

class OnePole
{
public:
    void setCutoff(double fs, double fc) noexcept; // fc clamped to [1 Hz, 0.49·fs]
    void zero() noexcept { s_[0] = s_[1] = 0.0f; }
    void lowPass(float* l, float* r, int n) noexcept;
    void highPass(float* l, float* r, int n) noexcept;

private:
    float g_ = 0.0f; // G = g / (1 + g)
    float s_[2]{};
};

class Svf
{
public:
    enum class Mode : std::uint8_t
    {
        LowPass,
        BandPass,
        HighPass,
        Notch
    };
    void setParams(double fs, double fc, double q) noexcept; // fc clamped to [5 Hz, 0.49·fs], q >= 0.01
    void zero() noexcept;
    void process(Mode, float* l, float* r, int n) noexcept;

private:
    float a1_ = 0, a2_ = 0, a3_ = 0, k_ = 1;
    float ic1_[2]{}, ic2_[2]{};
};
} // namespace engine::fx::dsp
