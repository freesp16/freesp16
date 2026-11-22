// Helpers of the amp-envelope tests: events, block rendering into a gain trace, closed-form references.
#pragma once

#include "engine/modulation/AmpEnvelope.h"

#include <cstdint>
#include <vector>

namespace ampenv_test
{
constexpr double kRate = 44100.0;
constexpr int kBlock = engine::kBlockFrames;

inline engine::trigger::TriggerEvent hit(int frame, int velocity = 127,
                                         engine::trigger::TriggerKind kind = engine::trigger::TriggerKind::Full,
                                         engine::trigger::TriggerSource source = engine::trigger::TriggerSource::Pad,
                                         int track = 0, const engine::trigger::LockSet* locks = nullptr)
{
    engine::trigger::TriggerEvent e {};
    e.track = static_cast<std::uint8_t>(track);
    e.velocity = static_cast<std::uint8_t>(velocity);
    e.semitones = 0;
    e.kind = kind;
    e.source = source;
    e.frame = static_cast<std::uint8_t>(frame);
    e.ducks = false;
    e.locks = locks;
    return e;
}

// Appends `blocks` blocks of gains to `out`.
inline void render(engine::modulation::AmpEnvelope& env, std::vector<float>& out, int blocks)
{
    alignas(16) float g[kBlock];
    for (int b = 0; b < blocks; ++b)
    {
        env.renderGain(g);
        out.insert(out.end(), g, g + kBlock);
    }
}

inline engine::modulation::AmpEnvParams params(int attack, int hold, int release, bool curveA = false,
                                               bool curveR = false)
{
    engine::modulation::AmpEnvParams p;
    p.attack = static_cast<std::uint8_t>(attack);
    p.hold = static_cast<std::uint8_t>(hold);
    p.release = static_cast<std::uint8_t>(release);
    p.velocitySensitive = false;
    p.attackCurve = curveA ? engine::modulation::EnvCurve::Curve : engine::modulation::EnvCurve::Linear;
    p.releaseCurve = curveR ? engine::modulation::EnvCurve::Curve : engine::modulation::EnvCurve::Linear;
    return p;
}

inline std::int64_t framesOf(double seconds)
{
    return static_cast<std::int64_t>(seconds * kRate + 0.5);
}
} // namespace ampenv_test
