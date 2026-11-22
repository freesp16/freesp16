// Test harness for the sample voices (tests only): owned, zero-padded SampleBuffers and a block-by-block driver for
// TrackVoice / PreviewVoice, the way the TrackSource calls them (setSample → events → render, 64 frames).
// The voice calls of every block run on a thread marked real-time: under APP_RT_CHECKS (host preset) any allocation
// aborts the test.
#pragma once

#include "core/util/Realtime.h"
#include "engine/voice/PreviewVoice.h"
#include "engine/voice/TrackVoice.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace voicetest
{
using core::audio::SampleBuffer;
using engine::kBlockFrames;
constexpr double kRate = 44100.0;
constexpr int kPad = SampleBuffer::pad;

// A SampleBuffer over test-owned planar storage: exactly `pad` zero frames before 0 and after the end, ch[c]
// 16-byte aligned.
class TestBuffer
{
public:
    explicit TestBuffer(const std::vector<float>& left, const std::vector<float>& right = {}, double rate = kRate)
    {
        const int channels = right.empty() ? 1 : 2;
        buf_.frames = static_cast<std::int64_t>(left.size());
        buf_.channels = static_cast<std::uint8_t>(channels);
        buf_.sampleRate = rate;
        buf_.contentId = 0;
        buf_.refs.store(0);
        buf_.ch[0] = buf_.ch[1] = nullptr;
        for (int c = 0; c < channels; ++c)
        {
            const auto& src = c == 0 ? left : right;
            auto& v = storage_[c];
            v.assign(src.size() + 2 * kPad + 3, 0.0f);
            std::size_t offset = 0;
            while ((reinterpret_cast<std::uintptr_t>(v.data() + offset + kPad) & 15u) != 0)
                ++offset;
            float* base = v.data() + offset + kPad;
            for (std::size_t i = 0; i < src.size(); ++i)
                base[i] = src[i];
            buf_.ch[c] = base;
        }
    }
    TestBuffer(const TestBuffer&) = delete;
    TestBuffer& operator= (const TestBuffer&) = delete;

    const SampleBuffer* get() const noexcept { return &buf_; }
    std::uint32_t refs() const noexcept { return buf_.refs.load(); }
    std::int64_t frames() const noexcept { return buf_.frames; }
    float at(int c, std::int64_t i) const noexcept
    {
        return i >= -kPad && i < buf_.frames + kPad ? buf_.ch[c][i] : 0.0f;
    }

private:
    std::vector<float> storage_[2];
    SampleBuffer buf_;
};

inline std::vector<float> sine(int frames, double hz, double rate = kRate, float amp = 1.0f, double phase = 0.0)
{
    std::vector<float> v(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<std::size_t>(i)] = amp * static_cast<float>(std::sin(phase + 2.0 * M_PI * hz * i / rate));
    return v;
}

inline std::vector<float> ramp(int frames, float from = 0.0f, float stepPerFrame = 1.0f)
{
    std::vector<float> v(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<std::size_t>(i)] = from + stepPerFrame * static_cast<float>(i);
    return v;
}

// What a run produced, frame by frame: the main voice (pre-envelope) and the tails, per channel.
struct Rendered
{
    std::vector<float> mainL, mainR, tailL, tailR;
    float sum(int c, std::size_t i) const { return c == 0 ? mainL[i] + tailL[i] : mainR[i] + tailR[i]; }
    std::size_t size() const { return mainL.size(); }
};

// Renders `blocks` blocks. Before each block, `hook(block)` runs (not real-time: it may use Catch macros) and then
// `rtHook(block)` runs with the voice calls of that block on the real-time-marked thread.
inline Rendered run(engine::voice::TrackVoice& v, int blocks, const std::function<void(int)>& rtHook = {},
                    const std::function<void(int)>& hook = {})
{
    Rendered out;
    const auto n = static_cast<std::size_t>(blocks * kBlockFrames);
    out.mainL.assign(n, 0.0f);
    out.mainR.assign(n, 0.0f);
    out.tailL.assign(n, 0.0f);
    out.tailR.assign(n, 0.0f);
    for (int b = 0; b < blocks; ++b)
    {
        if (hook)
            hook(b);
        const auto at = static_cast<std::size_t>(b * kBlockFrames);
        core::rt::ScopedRealtimeThread realtime;
        if (rtHook)
            rtHook(b);
        v.render(&out.mainL[at], &out.mainR[at], &out.tailL[at], &out.tailR[at]);
    }
    return out;
}

inline engine::voice::PlaybackParams params(std::int64_t start = 0, std::int64_t length = engine::voice::kWholeSample,
                                            bool loop = false, std::int64_t loopStart = 0, bool reverse = false,
                                            float cents = 0.0f)
{
    engine::voice::PlaybackParams p;
    p.region = {start, loopStart, length, loop, reverse};
    p.pitchCents = cents;
    return p;
}

// Frequency of a periodic signal from its rising zero crossings (linear interpolation between frames).
inline double frequency(const std::vector<float>& x, std::size_t from, std::size_t to, double rate = kRate)
{
    double first = -1, last = -1;
    int crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i)
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
} // namespace voicetest
