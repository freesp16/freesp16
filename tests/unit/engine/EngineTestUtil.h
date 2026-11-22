// Helpers for the engine unit tests: a prepared Engine bound to the built-in parameter registry, and an offline render
// of the Main pair through engine::OfflineDriver (the calling thread is marked real-time: any allocation aborts).
#pragma once

#include "engine/Engine.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace enginetest
{
inline engine::EngineSetup setup()
{
    return {[](std::string_view id) { return core::builtinParams().idOf(id).value_or(core::ParamId {}); }};
}

// The composition root's set-up: ids for the parameter bindings and the FX slot bank's route table.
inline engine::EngineSetup fullSetup()
{
    auto s = setup();
    for (const auto& spec : core::builtinParams().all())
        s.paramIds.push_back(spec.id);
    s.fxInternalIndex = core::fx::internalIndexTable(core::builtinParams());
    return s;
}

inline std::unique_ptr<engine::Engine> make(bool testTone = true, double rate = 44100.0, int maxFrames = 512)
{
    engine::EngineOptions o;
    o.testTone = testTone;
    auto e = std::make_unique<engine::Engine>(setup(), o);
    e->prepare(rate, maxFrames);
    return e;
}

inline core::ParamId volumeId() { return core::builtinParams().idOf("track.volume").value_or(core::ParamId {}); }

struct Stereo
{
    std::vector<float> l, r;
};

// Renders `frames` frames of Main in periods of `period` frames.
inline Stereo render(engine::Engine& e, int frames, int period = 256)
{
    Stereo s {std::vector<float>(static_cast<std::size_t>(frames)), std::vector<float>(static_cast<std::size_t>(frames))};
    float* const out[2] = {s.l.data(), s.r.data()};
    engine::OfflineDriver d;
    d.periodFrames = period;
    d.run(e, frames, out);
    return s;
}

inline float peak(const std::vector<float>& v, std::size_t from = 0, std::size_t to = ~std::size_t {0})
{
    float p = 0;
    for (std::size_t i = from; i < std::min(to, v.size()); ++i)
        p = std::max(p, std::fabs(v[i]));
    return p;
}

inline float maxStep(const std::vector<float>& v, std::size_t from = 1, std::size_t to = ~std::size_t {0})
{
    float m = 0;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, v.size()); ++i)
        m = std::max(m, std::fabs(v[i] - v[i - 1]));
    return m;
}
} // namespace enginetest
