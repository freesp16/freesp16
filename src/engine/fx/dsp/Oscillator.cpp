#include "engine/fx/dsp/Oscillator.h"

#include "engine/fx/dsp/Interp.h"

#include <cmath>

namespace engine::fx::dsp
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct SineTable
{
    float v[Oscillator::kTableSize + 1];
    SineTable() noexcept
    {
        for (int i = 0; i <= Oscillator::kTableSize; ++i)
            v[i] = static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / Oscillator::kTableSize));
    }
};
const SineTable kSine; // built before main() (static initialisation), read-only afterwards

// PolyBLEP residual for a unit step at phase t ∈ [0, 1) with phase increment dt.
float polyBlep(float t, float dt) noexcept
{
    APP_FX_NOFUSE
    if (dt <= 0.0f)
        return 0.0f;
    if (t < dt)
    {
        const float x = t / dt;
        return (x + x) - (x * x) - 1.0f;
    }
    if (t > 1.0f - dt)
    {
        const float x = (t - 1.0f) / dt;
        return (x * x) + (x + x) + 1.0f;
    }
    return 0.0f;
}

float toUnit(std::uint32_t phase) noexcept
{
    return static_cast<float>(phase >> 8) * (1.0f / 16777216.0f); // 24 bits: exact in float32
}
} // namespace

void Oscillator::setFrequency(double fs, double hz) noexcept
{
    const double cycles = hz / fs; // per sample
    const double inc = cycles * 4294967296.0;
    const auto s = static_cast<std::int64_t>(std::llround(inc));
    inc_ = static_cast<std::uint32_t>(s); // two's complement: negative frequencies run backwards
    dt_ = static_cast<float>(std::fabs(cycles));
}

void Oscillator::setPhase(double phase01) noexcept
{
    double p = phase01 - std::floor(phase01);
    phase_ = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p * 4294967296.0) & 0xffffffffu);
}

float Oscillator::sineAt(std::uint32_t phase) noexcept
{
    const std::uint32_t i = phase >> 21;                                         // 11 bits: table index
    const float f = static_cast<float>(phase & 0x1fffffu) * (1.0f / 2097152.0f); // 21 bits: fraction
    return interp::linear(kSine.v[i], kSine.v[i + 1], f);
}

float Oscillator::shapeAt(std::uint32_t phase) const noexcept
{
    APP_FX_NOFUSE
    const float t = toUnit(phase);
    switch (shape_)
    {
    case Shape::Sine:
        return sineAt(phase);
    case Shape::Triangle: // 0 at t = 0, +1 at 0.25, −1 at 0.75
    {
        const float u = t < 0.25f ? t : (t < 0.75f ? 0.5f - t : t - 1.0f);
        return 4.0f * u;
    }
    case Shape::SawUp:
    {
        float y = 2.0f * t - 1.0f;
        if (bandLimited_)
            y -= polyBlep(t, dt_);
        return y;
    }
    case Shape::SawDown:
    {
        float y = 1.0f - 2.0f * t;
        if (bandLimited_)
            y += polyBlep(t, dt_);
        return y;
    }
    case Shape::Square:
    {
        float y = t < 0.5f ? 1.0f : -1.0f;
        if (bandLimited_)
        {
            y += polyBlep(t, dt_);
            float t2 = t + 0.5f;
            if (t2 >= 1.0f)
                t2 -= 1.0f;
            y -= polyBlep(t2, dt_);
        }
        return y;
    }
    }
    return 0.0f;
}

float Oscillator::value() const noexcept
{
    return shapeAt(phase_);
}

void Oscillator::render(float* out, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        out[i] = shapeAt(phase_);
        phase_ += inc_;
    }
}
} // namespace engine::fx::dsp
