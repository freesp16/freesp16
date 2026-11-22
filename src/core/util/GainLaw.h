// The 0–127 index gain law of faders, sends, returns, master and headphones (01-architecture/audio-engine.md,
// "Gain law", [DSP] plus our own choices):
//   0        silence
//   1–9      linear in dB from −54 dB (1) to −41 dB (9)   (our interpolation of the coarse ladder)
//   10–100   (i/100)^2                                    (100 = 0 dB, the default)
//   101–127  2^(ln(i/100) / ln 1.27)                      (127 = ×2, +6.02 dB)
// Sends and headphones use the same table × 0.5 (their 127 = unity).
#pragma once

#include <array>
#include <cmath>
#include <limits>

namespace core
{
class GainLaw
{
public:
    static constexpr int maxIndex = 127;

    static double gainExact(int index) noexcept
    {
        if (index <= 0)
            return 0.0;
        if (index > maxIndex)
            index = maxIndex;
        if (index < 10)
        {
            const double db = -54.0 + (index - 1) * (13.0 / 8.0);
            return std::pow(10.0, db / 20.0);
        }
        const double x = index / 100.0;
        if (index <= 100)
            return x * x;
        return std::exp2(std::log(x) / std::log(1.27));
    }

    // Table lookup, safe on the audio thread (no libm call per sample or per block).
    static float gain(int index) noexcept
    {
        static const std::array<float, maxIndex + 1> table = [] {
            std::array<float, maxIndex + 1> t {};
            for (int i = 0; i <= maxIndex; ++i)
                t[static_cast<std::size_t>(i)] = static_cast<float>(gainExact(i));
            return t;
        }();
        return table[static_cast<std::size_t>(index < 0 ? 0 : (index > maxIndex ? maxIndex : index))];
    }

    static double decibels(int index) noexcept
    {
        const double g = gainExact(index);
        return g > 0.0 ? 20.0 * std::log10(g) : -std::numeric_limits<double>::infinity();
    }
};
} // namespace core
