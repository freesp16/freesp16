// The streaming frame of the own time-stretch backends (an own implementation instead of a vendored library, see
// THIRD_PARTY.md). A GrainStretcher is a TimeStretcher that turns planar input into overlap-added grains:
//
//   input ──(pitch resampler, cubic Hermite, Q32.32 position, bypassed at pitch 1)──▶ core ring
//   core ring ──(analysis frame every Ha = Hs / R core frames; R = timeRatio · pitchScale)──▶ synthesise() ──▶ OLA
//   OLA (window W, synthesis hop Hs) ──▶ output FIFO ──▶ out
//
// Frames are centre-mapped: synthesis centre τ ↔ analysis centre τ / R, and the first frames start before the
// input (zeros), so an onset at input frame 0 sounds at output frame 0: latencySamples() == 0. The price is a
// look-ahead — the first process() after reset() pulls about W/2·(1 + 1/R) + search core frames and computes W/Hs
// frames (the "priming burst" the StretcherBank budgets). Frames are computed lazily, only when
// the FIFO cannot serve the request, so the cost per 64-frame block is bursty: one grain every Hs output frames.
//
// framesNeeded(out) is exact: it simulates the same Q32.32 positions process() will use, so a caller that feeds
// exactly framesNeeded(out) frames always gets `out` frames without an underrun, and Σ in / Σ out → 1 / timeRatio.
// The time ratio is clamped to [0.25, 4] and the pitch scale to [0.5, 2] (MASTER TEMPO's ±12 st, PLY-020).
//
// Quality tiers (governor): tier 0 normal, tier 1 cheaper; a tier change applies at the next
// reset() (the next note), so no grain is cut. Real-time: everything but prepare() is noexcept and allocation-free.
#pragma once

#include "engine/voice/TimeStretcher.h"

#include <cstdint>
#include <vector>

namespace engine::voice::stretch
{
class GrainStretcher : public TimeStretcher
{
public:
    static constexpr double kMinTimeRatio = 0.25, kMaxTimeRatio = 4.0;
    static constexpr double kMinPitch = 0.5, kMaxPitch = 2.0;
    static constexpr int kTiers = 2;

    struct Geometry
    {
        int window = 1024; // W: grain length (≤ fft)
        int hop = 256;     // Hs: synthesis hop; W % Hs == 0
        int search = 0;    // Δ: extra core frames each side the backend may read (WSOLA alignment)
        int fft = 0;       // FFT size (0 = none)
    };

    void prepare(double sampleRate, int maxBlock, int channels) override;
    void reset() noexcept override;
    void setRatios(double timeRatio, double pitchScale) noexcept override;
    int latencySamples() const noexcept override { return 0; }
    int process(const float* const* in, int inFrames, float* const* out, int outFrames) noexcept override;
    int framesNeeded(int outFrames) const noexcept override;

    void setQuality(int tier) noexcept { pendingTier_ = tier <= 0 ? 0 : 1; } // from the next reset()
    // Grain staggering (the bank sets lane · 64 frames): grains are computed `frames` output frames earlier than
    // needed (modulo the hop), so voices started in the same block do not all synthesise in the same block. The
    // output timing is unchanged; only when the work happens moves.
    void setComputeAhead(int frames) noexcept { ahead_ = frames < 0 ? 0 : frames; }
    int quality() const noexcept { return tier_; }
    const Geometry& geometry() const noexcept { return geo_; }
    std::uint64_t grains() const noexcept { return grains_; } // frames synthesised since prepare (bench, tests)
    // Upper bound of framesNeeded(maxBlock) for any ratios (the caller's input scratch).
    int maxInputFrames() const noexcept { return maxIn_; }

protected:
    virtual Geometry geometryFor(int tier, double sampleRate) const noexcept = 0;
    virtual void allocateCore(int channels) = 0; // after prepare() sized the common buffers (message thread)
    virtual void resetCore() noexcept = 0;       // geometry() is the new tier's
    // One grain: the nominal analysis start `anaStart` (core frames, may be negative), `anaHop` core frames after the
    // previous grain's nominal start (0 for the first). The backend reads core frames with readCore and adds `window`
    // windowed samples per channel into acc(c) (already normalised for the overlap-add).
    virtual void synthesise(std::int64_t anaStart, int anaHop) noexcept = 0;

    // Core frames [start, start + n) of channel c into dst (zeros before the first frame); start ≥ the oldest kept.
    void readCore(int c, std::int64_t start, int n, float* dst) const noexcept;
    float* acc(int c) noexcept { return acc_[static_cast<std::size_t>(c)].data(); }
    int channels() const noexcept { return channels_; }
    double sampleRate() const noexcept { return sampleRate_; }
    double coreRatio() const noexcept { return ratio_; }
    bool firstGrain() const noexcept { return grains_ == firstGrainIndex_; }
    std::uint64_t grainIndex() const noexcept { return grains_ - firstGrainIndex_; } // since reset()

private:
    using Fixed = std::int64_t; // Q32.32
    static constexpr int kFrac = 32;
    static constexpr Fixed kOne = Fixed {1} << kFrac;
    static std::int64_t floorOf(Fixed x) noexcept { return x >> kFrac; } // arithmetic shift: floor for negatives
    static Fixed toFixed(double v) noexcept { return static_cast<Fixed>(v * 4294967296.0); }

    void emitGrain() noexcept;
    void resample() noexcept;
    int fifoLevel() const noexcept { return static_cast<int>(fifoWrite_ - fifoRead_); }
    int ahead() const noexcept { return ahead_ % geo_.hop; }

    double sampleRate_ = 44100.0;
    int channels_ = 2, maxBlock_ = 64, maxIn_ = 0;
    int tier_ = 0, pendingTier_ = 0, ahead_ = 0;
    Geometry geo_ {};
    double timeRatio_ = 1.0, pitch_ = 1.0, ratio_ = 1.0;
    Fixed anaHop_ = 0, step_ = kOne; // Ha (core frames), resampler step (input frames per core frame)

    // Input staging (original frames not yet resampled): stage_[c][i] = input frame stageBase_ + i.
    std::vector<float> stage_[2];
    std::int64_t stageBase_ = 0, inWritten_ = 0;
    Fixed rpos_ = 0; // input position of the next core frame
    // Core ring (power of two).
    std::vector<float> core_[2];
    std::int64_t coreMask_ = 0, coreWritten_ = 0;
    // Grains.
    Fixed anaPos_ = 0;          // nominal analysis start of the next grain
    std::int64_t prevAna_ = 0;  // integer start of the previous grain
    int warm_ = 0;              // grains whose first hop lies before output frame 0 (discarded)
    std::vector<float> acc_[2]; // overlap-add accumulator, window frames
    std::vector<float> fifo_[2];
    std::int64_t fifoMask_ = 0, fifoRead_ = 0, fifoWrite_ = 0;
    std::uint64_t grains_ = 0, firstGrainIndex_ = 0;
};
} // namespace engine::voice::stretch
