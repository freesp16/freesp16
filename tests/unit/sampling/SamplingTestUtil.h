// Helpers for the sampling tests: an Engine with a capture hub installed through the bridge (as the
// sampling session does), a driver that feeds a synthetic input through AudioIO::in (the engine's block adapter adds its
// 64-frame input latency, a simulated device latency can be added on top), and the parameter ids.
#pragma once

#include "tests/unit/engine/EngineTestUtil.h"

#include "core/params/ParamRegistry.h"
#include "core/util/sampling/CaptureHub.h"
#include "engine/sampling/SamplingStage.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace smptest
{
inline core::ParamId id(std::string_view name) { return core::builtinParams().idOf(name).value_or(core::ParamId {}); }

inline bool param(engine::Engine& e, std::string_view name, float value, std::uint16_t scope = 0)
{
    return e.bridge().post(engine::ParamChange {id(name), scope, value, 0});
}

// A hub with `seconds` of ring, installed in the engine (the engine holds one reference, the returned pointer is the
// test's own — release it after the engine is gone, or keep it: CaptureHubRef does).
struct HubRef
{
    core::sampling::CaptureHub* hub = nullptr;
    HubRef() = default;
    explicit HubRef(core::sampling::CaptureHub* h) : hub(h) {}
    HubRef(const HubRef&) = delete;
    HubRef& operator= (const HubRef&) = delete;
    HubRef(HubRef&& o) noexcept : hub(o.hub) { o.hub = nullptr; }
    ~HubRef()
    {
        if (hub != nullptr)
            hub->release();
    }
    core::sampling::CaptureHub* operator->() const noexcept { return hub; }
};

inline HubRef installHub(engine::Engine& e, double seconds = 30.0)
{
    auto* h = new core::sampling::CaptureHub();
    h->sampleRate = e.sampleRate();
    REQUIRE(h->ring.allocate(static_cast<std::int64_t>(seconds * e.sampleRate())));
    h->retain(); // the engine's
    REQUIRE(e.bridge().post(engine::ObjectSwap {engine::sampling::kCaptureHubSlot, h}));
    return HubRef(h);
}

// Input generator: fills l / r for device frames [pos, pos + n).
using InputFn = std::function<void(std::int64_t pos, int n, float* l, float* r)>;

// Runs the engine for `frames` device frames in periods of `period`, the input from `in` (null = no input), MAIN out
// collected into `mainL` (optional). Returns the device frame position after the run.
struct Driver
{
    std::int64_t pos = 0;
    int period = 256;
    std::vector<float> inL, inR, outL, outR;
    std::vector<float> mainL; // appended MAIN L of every run (when collect)
    bool collect = false;

    void run(engine::Engine& e, std::int64_t frames, const InputFn& in)
    {
        inL.assign(static_cast<std::size_t>(period), 0.0f);
        inR.assign(static_cast<std::size_t>(period), 0.0f);
        outL.assign(static_cast<std::size_t>(period), 0.0f);
        outR.assign(static_cast<std::size_t>(period), 0.0f);
        while (frames > 0)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(frames, period));
            if (in)
                in(pos, n, inL.data(), inR.data());
            float* out[2] = {outL.data(), outR.data()};
            const float* ins[2] = {inL.data(), inR.data()};
            e.process(engine::AudioIO {out, 1, in ? ins : nullptr, in ? 2 : 0, n, 0});
            if (collect)
                mainL.insert(mainL.end(), outL.begin(), outL.begin() + n);
            pos += n;
            frames -= n;
        }
    }
};

// A click track: one 1-frame click of `amp` at every `spacing` frames from `first`, delayed by `delay` frames (the
// simulated device input latency), silence elsewhere.
inline InputFn clicks(std::int64_t first, std::int64_t spacing, std::int64_t delay, float amp = 0.5f, int width = 1)
{
    return [=](std::int64_t pos, int n, float* l, float* r) {
        for (int i = 0; i < n; ++i)
        {
            const std::int64_t t = pos + i - delay - first;
            const bool hit = t >= 0 && t % spacing < width;
            l[i] = hit ? amp : 0.0f;
            r[i] = hit ? -amp : 0.0f;
        }
    };
}

// Index of the first |x| > thr in v from `from`, −1 if none.
inline std::int64_t firstAbove(const float* v, std::int64_t n, float thr, std::int64_t from = 0)
{
    for (std::int64_t i = from; i < n; ++i)
        if (std::fabs(v[i]) > thr)
            return i;
    return -1;
}

inline std::unique_ptr<engine::Engine> make(double rate = 44100.0)
{
    auto e = enginetest::make(false, rate, 512);
    return e;
}
} // namespace smptest
