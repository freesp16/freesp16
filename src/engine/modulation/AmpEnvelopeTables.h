// Per-sample-rate tables of the amp envelope (std::exp / std::pow only in
// set-up and on a tempo change, never per block). Built from core::ampenv's time map:
//   frames  segment length in samples for every stage × display index (INFINITY = kInfinite); sync on: per sync step
//   rho     per-sample ratio of the CURVE shapes, in double (the envelope keeps its geometric state in double per run,
//           so a 32 s curve does not drift): attack residual reaches −60 dB at the attack time (ρ^N = 1e-3), release
//           reaches −80 dB at the release time (ρ^N = 1e-4), then cut
// A release is never shorter than kDeclickFrames (RELEASE 0 = 0 s in the time map, but a gate release or a hold end
// must not click: NFR-031); an attack that restarts from a level above zero has the same floor.
#pragma once

#include "core/util/ampenv/EnvTime.h"

#include <array>
#include <cstdint>

namespace engine::modulation
{
class AmpEnvTables
{
public:
    static constexpr std::int32_t kInfinite = -1;
    static constexpr int kDeclickFrames = 32; // ≈ 0.73 ms at 44.1 kHz (engine::stages::ParamSmoother::kMinRamp)
    static constexpr int kKillFrames = 64;    // kill(): 64-frame de-click to 0 (choke, panic)
    static constexpr double kBypassRampSeconds = 0.002; // ENV-009: 2 ms ramps at start and stop
    static constexpr double kAttackResidual = 1.0e-3;   // −60 dB (ENV-004)
    static constexpr double kReleaseResidual = 1.0e-4;  // −80 dB (ENV-004)

    struct Segment
    {
        std::int32_t frames = 0; // kInfinite = never ends
        double rho = 0.0;        // CURVE ratio per sample (attack: of the residual; release: of the level)
    };

    // Message thread / before the device starts: computes every table (≈ 400 libm calls).
    void prepare(double sampleRate) noexcept;
    // At most once per block, only when the tempo changed: the 16 sync steps (32 exp). Returns true when recomputed.
    bool setTempo(double bpm) noexcept;

    double sampleRate() const noexcept { return sampleRate_; }
    double bpm() const noexcept { return bpm_; }
    int bypassRampFrames() const noexcept { return bypassFrames_; }
    const Segment& declickAttack() const noexcept { return declickAttack_; }

    // Lookups (index already clamped by core::ampenv::clampIndex).
    Segment attack(std::uint8_t index, bool sync) const noexcept;
    std::int32_t hold(std::uint8_t index, bool sync) const noexcept;
    Segment release(std::uint8_t index, bool sync) const noexcept;

private:
    static constexpr int kIndices = core::ampenv::kInfinityIndex + 1;
    double sampleRate_ = 0.0, bpm_ = 0.0;
    int bypassFrames_ = 88;
    Segment declickAttack_ {};
    std::array<Segment, kIndices> attack_ {}, release_ {};
    std::array<std::int32_t, kIndices> hold_ {};
    std::array<Segment, core::ampenv::kSyncSteps> syncAttack_ {}, syncRelease_ {};
    std::array<std::int32_t, core::ampenv::kSyncSteps> syncHold_ {};
};
} // namespace engine::modulation
