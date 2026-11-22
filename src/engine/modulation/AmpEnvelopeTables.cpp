#include "engine/modulation/AmpEnvelopeTables.h"

#include <cmath>

namespace engine::modulation
{
namespace
{
using core::ampenv::EnvStage;

std::int32_t framesOf(double seconds, double sampleRate) noexcept
{
    if (std::isinf(seconds))
        return AmpEnvTables::kInfinite;
    const double f = std::floor(seconds * sampleRate + 0.5);
    return f > 2.0e9 ? 2000000000 : static_cast<std::int32_t>(f);
}

double rhoFor(std::int32_t frames, double residual) noexcept
{
    return frames > 0 ? std::exp(std::log(residual) / static_cast<double>(frames)) : 0.0;
}

AmpEnvTables::Segment attackSegment(double seconds, double sampleRate) noexcept
{
    const auto n = framesOf(seconds, sampleRate);
    return {n, rhoFor(n, AmpEnvTables::kAttackResidual)};
}

AmpEnvTables::Segment releaseSegment(double seconds, double sampleRate) noexcept
{
    auto n = framesOf(seconds, sampleRate);
    if (n != AmpEnvTables::kInfinite && n < AmpEnvTables::kDeclickFrames)
        n = AmpEnvTables::kDeclickFrames;
    return {n, n == AmpEnvTables::kInfinite ? 1.0 : rhoFor(n, AmpEnvTables::kReleaseResidual)};
}
} // namespace

void AmpEnvTables::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    const auto bf = framesOf(kBypassRampSeconds, sampleRate_);
    bypassFrames_ = bf < 1 ? 1 : bf;
    declickAttack_ = {kDeclickFrames, rhoFor(kDeclickFrames, kAttackResidual)};
    for (int i = 0; i < kIndices; ++i)
    {
        const auto idx = static_cast<std::uint8_t>(i);
        attack_[static_cast<std::size_t>(i)] =
            attackSegment(core::ampenv::envSecondsExact(EnvStage::Attack, idx, false, 120.0), sampleRate_);
        hold_[static_cast<std::size_t>(i)] =
            framesOf(core::ampenv::envSecondsExact(EnvStage::Hold, idx, false, 120.0), sampleRate_);
        release_[static_cast<std::size_t>(i)] =
            releaseSegment(core::ampenv::envSecondsExact(EnvStage::Release, idx, false, 120.0), sampleRate_);
    }
    bpm_ = 0.0;
    setTempo(120.0);
}

bool AmpEnvTables::setTempo(double bpm) noexcept
{
    if (bpm == bpm_ || ! (bpm > 0.0))
        return false;
    bpm_ = bpm;
    for (int s = 0; s < core::ampenv::kSyncSteps; ++s)
    {
        // Any finite index of step s gives the step's length; the smallest one is s × 8.
        const auto idx = static_cast<std::uint8_t>(s * 128 / core::ampenv::kSyncSteps);
        const double sec = core::ampenv::envSecondsExact(EnvStage::Attack, idx, true, bpm);
        const auto i = static_cast<std::size_t>(s);
        syncAttack_[i] = attackSegment(sec, sampleRate_);
        syncHold_[i] = framesOf(sec, sampleRate_);
        syncRelease_[i] = releaseSegment(sec, sampleRate_);
    }
    return true;
}

AmpEnvTables::Segment AmpEnvTables::attack(std::uint8_t index, bool sync) const noexcept
{
    if (index > core::ampenv::kMaxFiniteIndex)
        index = core::ampenv::kMaxFiniteIndex;
    return sync ? syncAttack_[static_cast<std::size_t>(core::ampenv::syncStep(index))] : attack_[index];
}

std::int32_t AmpEnvTables::hold(std::uint8_t index, bool sync) const noexcept
{
    if (index >= core::ampenv::kInfinityIndex)
        return kInfinite;
    return sync ? syncHold_[static_cast<std::size_t>(core::ampenv::syncStep(index))] : hold_[index];
}

AmpEnvTables::Segment AmpEnvTables::release(std::uint8_t index, bool sync) const noexcept
{
    if (index >= core::ampenv::kInfinityIndex)
        return release_[core::ampenv::kInfinityIndex];
    return sync ? syncRelease_[static_cast<std::size_t>(core::ampenv::syncStep(index))] : release_[index];
}
} // namespace engine::modulation
