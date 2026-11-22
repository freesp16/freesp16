// The mockups' pseudo waveforms (07-ui-design/_tools/kit_track.py amplitudes()) as a WaveOverview, so the waveform
// widgets draw the same columns the mockups do: one level per column count a mockup uses (editor 314 columns of 3
// lpx, overview 456 of 2 lpx, thumbnails 70 of 2 lpx). Generated data only.
#pragma once

#include "ui/screens/playback/SampleViews.h"

#include <cmath>
#include <string_view>
#include <vector>

namespace m1test
{
inline double rnd(int i, int seed = 1)
{
    const double v = std::sin(i * 12.9898 + seed * 78.233) * 43758.5453;
    return v - std::floor(v);
}

inline std::vector<float> amplitudes(int n, std::string_view kind, int seed = 1)
{
    std::vector<float> out;
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / n;
        double a;
        if (kind == "kick")
            a = std::exp(-t * 3.2) * (t > 0.004 ? 0.95 : t / 0.004);
        else if (kind == "snare")
            a = std::exp(-t * 6.0);
        else if (kind == "hat")
            a = std::exp(-t * 14.0);
        else if (kind == "bass")
        {
            const double k = std::fmod(t * 16, 1.0);
            a = 0.55 * std::exp(-k * 1.4) + 0.1;
        }
        else
        {
            const double k = t * 8, pos = k - std::floor(k);
            static constexpr double hs[] = {1.0, 0.82, 0.95, 0.85};
            a = hs[static_cast<int>(std::floor(k)) % 4] * std::exp(-pos * 5.0);
            const double off = std::fmod(k + 0.5, 1.0);
            a = std::max(a, 0.18 * std::exp(-off * 9.0));
        }
        out.push_back(static_cast<float>(std::max(0.02, a * (0.55 + 0.45 * rnd(i, seed)))));
    }
    return out;
}

struct DemoSample
{
    std::int64_t frames;
    std::vector<std::vector<float>> mins, maxs;
    std::vector<ui::WaveOverview::Level> levels;
    ui::WaveOverview overview;

    explicit DemoSample(std::string_view kind = "beat", std::int64_t f = 176400,
                        std::initializer_list<int> counts = {314, 456, 70})
        : frames(f)
    {
        for (int n : counts)
        {
            auto a = amplitudes(n, kind);
            std::vector<float> lo(a.size());
            for (std::size_t i = 0; i < a.size(); ++i)
                lo[i] = -a[i];
            maxs.push_back(std::move(a));
            mins.push_back(std::move(lo));
        }
        for (std::size_t k = 0; k < maxs.size(); ++k)
            levels.push_back({mins[k].data(), maxs[k].data(), static_cast<std::int64_t>(maxs[k].size()),
                              static_cast<double>(frames) / static_cast<double>(maxs[k].size())});
        overview.levels = levels;
        overview.frames = frames;
    }
    ui::m1::SampleView view(std::string_view name = {}) const { return {&overview, frames, 44100, name}; }
};
} // namespace m1test
