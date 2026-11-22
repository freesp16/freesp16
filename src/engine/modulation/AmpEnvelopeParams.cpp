#include "engine/modulation/AmpEnvelopeParams.h"

#include "core/model/enums/Enums.h"

namespace engine::modulation
{
static_assert(static_cast<int>(EnvCurve::Linear) == static_cast<int>(core::model::EnvelopeCurve::Linear) &&
                  static_cast<int>(EnvCurve::Curve) == static_cast<int>(core::model::EnvelopeCurve::Curve),
              "core::ampenv::EnvCurve mirrors the model's EnvelopeCurve");

namespace
{
template <typename T> bool store(T& field, T value) noexcept
{
    if (field == value)
        return false;
    field = value;
    return true;
}
} // namespace

bool applyParam(AmpEnvParams& p, AmpEnvField field, float v) noexcept
{
    using core::ampenv::EnvStage;
    const bool on = v >= 0.5f;
    const auto curve = on ? EnvCurve::Curve : EnvCurve::Linear;
    switch (field)
    {
        case AmpEnvField::Velocity:
            return store(p.velocity, core::ampenv::clampIndex(EnvStage::Attack, v));
        case AmpEnvField::Attack:
            return store(p.attack, core::ampenv::clampIndex(EnvStage::Attack, v));
        case AmpEnvField::Hold:
            return store(p.hold, core::ampenv::clampIndex(EnvStage::Hold, v));
        case AmpEnvField::Release:
            return store(p.release, core::ampenv::clampIndex(EnvStage::Release, v));
        case AmpEnvField::Sync:
            return store(p.sync, on);
        case AmpEnvField::AttackCurve:
            return store(p.attackCurve, curve);
        case AmpEnvField::ReleaseCurve:
            return store(p.releaseCurve, curve);
        case AmpEnvField::Retrigger:
            return store(p.retrigger, on);
        case AmpEnvField::Bypass:
            return store(p.bypass, on);
        case AmpEnvField::VelocitySensitive:
            return store(p.velocitySensitive, on);
        case AmpEnvField::Count:
            break;
    }
    return false;
}
} // namespace engine::modulation
