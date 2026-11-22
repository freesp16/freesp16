// The band-limited / table oscillator of the effects DSP kit, shared by the track LFO and the modulation
// effects (Chorus, Flanger, Phaser).
//
// - Phase is a 32-bit unsigned accumulator (exact wrap-around, no drift, no fmod); the increment is computed in double
//   on set-up (setFrequency) and stored as an integer.
// - Sine: a 2048-point table (+1 guard point, built once before main()) read with linear interpolation.
// - Triangle: exact (piecewise linear), Saw/Square: naive shapes with a PolyBLEP residual at each discontinuity (audio
//   rate use); for LFO use (rates far below fs/2) the residual is negligible and `bandLimited` may be false.
// - value() is the current output, advance(n) steps n samples (control rate: one value per 64-frame block);
//   render() writes n audio-rate samples. Real-time safe, scalar (the recursion is per sample).
#pragma once

#include "engine/fx/dsp/Numerics.h"

namespace engine::fx::dsp
{
class Oscillator
{
public:
    enum class Shape : std::uint8_t
    {
        Sine,
        Triangle,
        SawUp,
        SawDown,
        Square
    };
    static constexpr int kTableSize = 2048;

    void setFrequency(double fs, double hz) noexcept; // |hz| < fs/2; negative runs backwards
    void setShape(Shape s) noexcept { shape_ = s; }
    void setBandLimited(bool on) noexcept { bandLimited_ = on; }
    void setPhase(double phase01) noexcept; // 0…1 of a cycle
    void retrigger(double phase01 = 0.0) noexcept { setPhase(phase01); }
    double phase() const noexcept { return static_cast<double>(phase_) * (1.0 / 4294967296.0); }
    std::uint32_t phaseBits() const noexcept { return phase_; }

    float value() const noexcept; // −1…+1 at the current phase
    void advance(int n) noexcept { phase_ += inc_ * static_cast<std::uint32_t>(n); }
    void render(float* out, int n) noexcept; // n samples, advances

    static float sineAt(std::uint32_t phase) noexcept; // the table read (exposed for tests and other kernels)

private:
    float shapeAt(std::uint32_t phase) const noexcept;
    std::uint32_t phase_ = 0, inc_ = 0;
    float dt_ = 0.0f; // |inc| / 2^32, for PolyBLEP
    Shape shape_ = Shape::Sine;
    bool bandLimited_ = true;
};
} // namespace engine::fx::dsp
