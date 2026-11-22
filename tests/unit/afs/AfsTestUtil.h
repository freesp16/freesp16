// Helpers of the analog-filter slot tests: signals, a small FFT, the slot driven like the engine drives
// it (one 64-frame block per call, the external loop's ports and return adapter), and an engine with a loopback
// device for the external-loop latency.
#pragma once

#include "engine/Engine.h"
#include "engine/afs/AnalogFilterSlot.h"

#include "core/params/ParamRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace afstest
{
constexpr int N = engine::kBlockFrames;
constexpr double kPi = 3.14159265358979323846;
using Vec = std::vector<float>;
using engine::afs::AfsField;
using engine::afs::AnalogFilterSlot;
using engine::afs::Route;

inline Vec sine(int frames, double hz, double amp, double fs = 44100.0, double phase = 0.0)
{
    Vec v(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        v[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / fs + phase));
    return v;
}

inline Vec noise(int frames, std::uint32_t seed, float amp = 0.5f)
{
    Vec v(static_cast<std::size_t>(frames));
    std::uint32_t x = seed * 2654435761u + 1u;
    for (auto& s : v)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = amp * (static_cast<float>(x >> 8) * (2.0f / 16777216.0f) - 1.0f);
    }
    return v;
}

inline bool sameBits(const Vec& a, const Vec& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), sizeof(float) * a.size()) == 0;
}

// Magnitude spectrum of `x` (power-of-two length) through a 4-term Blackman–Harris window (−92 dB side lobes).
inline std::vector<double> spectrum(const Vec& x)
{
    const std::size_t n = x.size();
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        a[i] = static_cast<double>(x[i]) * w;
    }
    for (std::size_t i = 1, j = 0; i < n; ++i) // bit reversal
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const std::complex<double> wl = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<double> w = 1.0;
            for (std::size_t k = 0; k < len / 2; ++k, w *= wl)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
    std::vector<double> m(n / 2 + 1);
    for (std::size_t k = 0; k < m.size(); ++k)
        m[k] = std::abs(a[k]);
    return m;
}

// RMS of x[from, to).
inline double rms(const Vec& x, std::size_t from, std::size_t to)
{
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i)
        s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return std::sqrt(s / static_cast<double>(to - from));
}

// Frequency of a periodic signal from its rising zero crossings (linear interpolation).
inline double zeroCrossingHz(const Vec& x, std::size_t from, double fs = 44100.0)
{
    double first = -1, last = -1;
    int count = 0;
    for (std::size_t i = from + 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + static_cast<double>(x[i - 1]) / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0)
                first = t;
            last = t;
            ++count;
        }
    return count > 1 ? (count - 1) * fs / (last - first) : 0.0;
}

// The slot as the engine drives it, without the rest of the engine: one block = beginPeriod (64-frame period),
// beginBlock, the point(s), routeOutputs, endBlock, then the return capture (after the render, as Engine::process).
struct SlotRig
{
    AnalogFilterSlot slot;
    bool loopPorts = false;                  // the device has FilterSend + return inputs 2/3
    std::array<float, N> sendL {}, sendR {}; // what reached the FilterSend port this block (0 = none)
    std::array<float, N> inL {}, inR {}, ret2 {}, ret3 {};
    std::array<float, N> out[2 * engine::stages::kPorts] {};
    engine::stages::BlockBuffers bb {};

    double fs = 44100.0;
    explicit SlotRig(bool ports = false, double rate = 44100.0) : loopPorts(ports), fs(rate) { settle(); }
    // The configuration in effect at once (no dip): the ports announced, then prepare() — after the set-up's fields.
    void settle()
    {
        slot.beginPeriod(io());
        slot.prepare(fs, N);
    }
    void set(AfsField f, float v) { slot.setField(f, v); }

    engine::AudioIO io()
    {
        static thread_local std::array<float*, 2 * engine::stages::kPorts> o {};
        static thread_local std::array<const float*, 4> in {};
        for (int c = 0; c < 2 * engine::stages::kPorts; ++c)
            o[static_cast<std::size_t>(c)] = (c < 2 || loopPorts) ? out[c].data() : nullptr;
        in = {inL.data(), inR.data(), ret2.data(), ret3.data()};
        return {o.data(), engine::stages::kPorts, in.data(), loopPorts ? 4 : 2, N, 0};
    }

    // One block at the master point, in place. `retNext`: the return frames the device delivers during this period.
    void master(float* l, float* r, const float* retNextL = nullptr, const float* retNextR = nullptr,
                std::span<const engine::trigger::TriggerEvent> events = {}, float lfo = 0.0f)
    {
        begin(retNextL, retNextR, events, lfo);
        slot.processAt(Route::Master, -1, l, r, N);
        end();
    }
    // One block through track t's hook (pre point → `inner` → post point).
    void track(int t, float* l, float* r, engine::mixer::BusInsert* inner = nullptr, const float* retNextL = nullptr,
               const float* retNextR = nullptr)
    {
        begin(retNextL, retNextR, {}, 0.0f);
        bb.activeTracks = 1u << t;
        slot.claimTrack(bb);
        engine::mixer::Hooks h {};
        h.insert[static_cast<std::size_t>(t)] = inner;
        slot.wrapHooks(h);
        if (h.insert[static_cast<std::size_t>(t)] != nullptr)
            h.insert[static_cast<std::size_t>(t)]->process(l, r);
        end();
    }

private:
    const float* pendL_ = nullptr;
    const float* pendR_ = nullptr;
    void begin(const float* retNextL, const float* retNextR, std::span<const engine::trigger::TriggerEvent> events,
               float lfo)
    {
        if (retNextL != nullptr)
        {
            std::copy(retNextL, retNextL + N, ret2.begin());
            std::copy(retNextR, retNextR + N, ret3.begin());
        }
        else
        {
            ret2.fill(0.0f);
            ret3.fill(0.0f);
        }
        slot.beginPeriod(io());
        slot.beginBlock(events, lfo);
    }
    void end()
    {
        bb.port.fill(nullptr);
        slot.routeOutputs(bb);
        constexpr int fsp = static_cast<int>(engine::OutPort::FilterSend);
        if (bb.port[2 * fsp] != nullptr)
        {
            std::copy(bb.port[2 * fsp], bb.port[2 * fsp] + N, sendL.begin());
            std::copy(bb.port[2 * fsp + 1], bb.port[2 * fsp + 1] + N, sendR.begin());
        }
        else
        {
            sendL.fill(0.0f);
            sendR.fill(0.0f);
        }
        slot.endBlock();
        slot.capture(0, 0, N);
    }
};

// Runs a stereo signal through the slot at the master point, block by block (length a multiple of 64).
inline void runMaster(SlotRig& rig, Vec& l, Vec& r)
{
    for (std::size_t b = 0; b + N <= l.size(); b += N)
        rig.master(l.data() + b, r.data() + b);
}

inline core::ParamId paramId(std::string_view id) { return core::builtinParams().idOf(id).value_or(core::ParamId {}); }
} // namespace afstest
