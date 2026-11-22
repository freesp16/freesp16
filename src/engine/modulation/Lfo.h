// One track LFO (spec 02-features/lfo.md LFO-001…011). A phase accumulator
// evaluated once per 64-frame block (never per sample): the value at the block start feeds the ModMatrix.
//
// Phase. elapsed = cycles since the last restart; phase = frac(phaseOffset + elapsed).
//   free speed (Hz)          elapsed advances by hz · 64 / fs per block (double: no drift worth a sample in hours)
//   SYNC, transport running  elapsed = (ppq − anchor) / quarters per cycle — derived from the transport PPQ, so it
//                            never drifts; anchor = the PPQ of the last restart (transport start or a trigger)
//   SYNC, transport stopped  advances at the tempo (bpm / 60 · 64 / fs / quarters per block), so pads still modulate
// Switching between these keeps the phase continuous (the anchor moves); a restart sets elapsed = 0 (start phase = the
// phase offset, LFO-004/007). One-shot (lfo.triggerMode ONE SHOT, our addition, OQ-19): after one cycle the LFO holds
// the value at the cycle's end.
//
// Shapes (LFO-001; 03-ui/enumerations.md "Waveform"): sine, triangle (0 → +1 → −1 → 0, in phase with the sine), saw
// (falling, +1 → −1), pulses of 50 / 25 / 12.5 % (+1 for the first part of the cycle, −1 after), and Random = sample
// and hold of the app's own xorshift32 (core/util/lfo/LfoLaw.h): the state restarts from the seed at every restart,
// steps once per new cycle, so the same seed renders the same sequence (LFO-008). No firmware table.
// Smoothing (LFO-011): sine, triangle and saw pass a one-pole 50 Hz smoother per block; pulses and Random do not.
#pragma once

#include <cstdint>

namespace engine::modulation
{
enum class LfoShape : std::uint8_t { Sine, Triangle, Saw, Pulse50, Pulse25, Pulse12, Random }; // LFO-001
constexpr int kLfoShapes = 7;

// The analytic shape at a phase in turns (any value; reduced to [0, 1)), Random excepted (its value is the held one).
float lfoShapeValue(LfoShape shape, double phase) noexcept;
constexpr bool lfoShapeSmoothed(LfoShape s) noexcept
{
    return s == LfoShape::Sine || s == LfoShape::Triangle || s == LfoShape::Saw;
}

class Lfo
{
public:
    void prepare(double sampleRate) noexcept;   // smoother coefficient exp(−2π·50·64/fs), resets the phase
    void setShape(LfoShape) noexcept;
    void setSpeedHz(float) noexcept;            // free speed, 0.01–40 Hz (clamped)
    void setSyncDivision(int) noexcept;         // −1 = free (Hz); else core::lfo::kSyncDivisions index
    void setDepth(float d01) noexcept;          // 0–1 (depth/127)
    void setPhaseOffset(float p01) noexcept;    // 0–1 of a cycle
    void setSeed(std::uint32_t) noexcept;       // Random's xorshift32 seed (non-zero; 0 is replaced)
    void setOneShot(bool) noexcept;
    void restart() noexcept;                    // at the next block start (trigger, transport start, LFO-007)
    void restartAt(double ppq) noexcept;        // SYNC: anchored at that PPQ (a trigger's frame); free: as restart()
    // The block's value ∈ [−1, 1] at its start (smoothed for continuous shapes), then advances to the next block.
    float advanceBlock(double bpm, double ppqAtBlockStart, bool running) noexcept;

    float value() const noexcept { return value_; }              // last advanceBlock() result
    float rawValue() const noexcept { return raw_; }             // the same, unsmoothed
    float amount() const noexcept { return value_ * depth_; }    // the ModMatrix Offset (FX-084: lfoOut · depth)
    double phase() const noexcept { return phase_; }             // phase at the last block start, [0, 1)
    double elapsed() const noexcept { return elapsedNow_; }      // cycles since the restart at the last block start
    float depth() const noexcept { return depth_; }
    LfoShape shape() const noexcept { return shape_; }
    int syncDivision() const noexcept { return division_; }

private:
    double sampleRate_ = 44100.0;
    double blockSeconds_ = 64.0 / 44100.0;
    double freeRate_ = 64.0 / 44100.0; // free: cycles per block (hz · 64 / fs)
    double q_ = 1.0, invQ_ = 1.0;      // SYNC: quarters per cycle of the division, and its inverse
    double elapsed_ = 0.0;     // free / stopped: elapsed at the next block start
    double elapsedNow_ = 0.0;  // at the last block start
    double anchor_ = 0.0;      // SYNC running: PPQ of elapsed 0
    double quarters_ = 4.0;    // SYNC: quarters per cycle of the anchored division
    double phase_ = 0.0;
    double restartPpq_ = 0.0;
    std::int32_t cycle_ = 0;   // floor(offset + elapsed) of the held Random value
    float hz_ = 1.0f, depth_ = 0.0f, offset_ = 0.0f, smoothK_ = 1.0f, value_ = 0.0f, raw_ = 0.0f, smoothY_ = 0.0f;
    std::uint32_t seed_ = 1u, state_ = 1u;
    float held_ = 0.0f;
    int division_ = -1;
    LfoShape shape_ = LfoShape::Sine;
    bool oneShot_ = false;
    bool restartPending_ = false, restartExact_ = false;
    bool anchored_ = false;    // the last block ran SYNC with the transport running (anchor_ valid)
};
} // namespace engine::modulation
