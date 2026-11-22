// Test harness for TimeStretcher implementations (tests only): drives a stretcher block by block the way the voice
// does (framesNeeded(n) frames in, n frames out) on a thread marked real-time (any allocation aborts under
// APP_RT_CHECKS), and measures what the contract asks: consumption rate, output length, frequency, onset, level.
#pragma once

#include "core/util/Realtime.h"
#include "engine/voice/TimeStretcher.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace stretchtest
{
constexpr double kRate = 44100.0;

inline std::vector<float> sine(int frames, double hz, float amp = 0.5f, int startAt = 0, double rate = kRate)
{
    std::vector<float> v(static_cast<std::size_t>(frames), 0.0f);
    for (int i = startAt; i < frames; ++i)
        v[static_cast<std::size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * M_PI * hz * (i - startAt) / rate));
    return v;
}

struct Output
{
    std::vector<float> l, r;
    std::vector<long> consumedAt; // input frames consumed after each block
    long consumed = 0;
    bool allocated = false;
};

// Streams `in` (planar, zeros past its end) through `s` until `outFrames` frames came out. `block` frames per call;
// `perBlock(b, s)` may change ratios before block b (not real-time).
inline Output run(engine::voice::TimeStretcher& s, const std::vector<float>& inL, const std::vector<float>& inR,
                  int outFrames, int block = 64, const std::function<void(int, engine::voice::TimeStretcher&)>& perBlock = {})
{
    Output o;
    o.l.assign(static_cast<std::size_t>(outFrames), 0.0f);
    o.r.assign(static_cast<std::size_t>(outFrames), 0.0f);
    std::vector<float> a(1 << 16), b(1 << 16);
    const std::size_t violations = core::rt::violationCount();
    int b0 = 0;
    for (int at = 0; at < outFrames; at += block, ++b0)
    {
        if (perBlock)
            perBlock(b0, s);
        const int n = std::min(block, outFrames - at);
        int need = 0;
        {
            core::rt::ScopedRealtimeThread realtime;
            need = s.framesNeeded(n);
        }
        need = std::min<int>(need, static_cast<int>(a.size()));
        for (int i = 0; i < need; ++i)
        {
            const auto k = static_cast<std::size_t>(o.consumed + i);
            a[static_cast<std::size_t>(i)] = k < inL.size() ? inL[k] : 0.0f;
            b[static_cast<std::size_t>(i)] = k < inR.size() ? inR[k] : 0.0f;
        }
        const float* in[2] = {a.data(), b.data()};
        float* out[2] = {o.l.data() + at, o.r.data() + at};
        {
            core::rt::ScopedRealtimeThread realtime;
            s.process(in, need, out, n);
        }
        o.consumed += need;
        o.consumedAt.push_back(o.consumed);
    }
    o.allocated = core::rt::violationCount() != violations;
    return o;
}

// Frequency from rising zero crossings (linear interpolation) over [from, to).
inline double frequency(const std::vector<float>& x, std::size_t from, std::size_t to, double rate = kRate)
{
    double first = -1, last = -1;
    int crossings = 0;
    for (std::size_t i = from + 1; i < to && i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + static_cast<double>(x[i - 1]) / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0)
                first = t;
            last = t;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0.0;
}

inline double rms(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double s = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < to && i < x.size(); ++i, ++n)
        s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return n > 0 ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

// Envelope (RMS over `win` frames, centred) crossing `level` upwards (onset) or the last frame above it (end).
inline long onset(const std::vector<float>& x, double level, int win = 256)
{
    for (std::size_t i = static_cast<std::size_t>(win); i + static_cast<std::size_t>(win) < x.size(); i += 8)
        if (rms(x, i - static_cast<std::size_t>(win / 2), i + static_cast<std::size_t>(win / 2)) >= level)
            return static_cast<long>(i);
    return -1;
}
inline long endOf(const std::vector<float>& x, double level, int win = 256)
{
    for (std::size_t i = x.size() - static_cast<std::size_t>(win); i > static_cast<std::size_t>(win); i -= 8)
        if (rms(x, i - static_cast<std::size_t>(win / 2), i + static_cast<std::size_t>(win / 2)) >= level)
            return static_cast<long>(i);
    return -1;
}

inline bool finite(const std::vector<float>& x)
{
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}
} // namespace stretchtest
