// TEMPORARY test-tone stage (the walking skeleton's "plays the test tone" behaviour; once the smoke checks no longer
// need it, delete this file and its call in Engine::renderBlock).
//
//   - a 440 Hz sine at −12 dBFS on track 1 (0.25 × GainLaw(volume index)), the level the emulator smoke checks on
//     MAIN (WAV ch 0/2, tools/analyze-wav.py)
//   - "track.volume" of track 1 (scope (0 << 4) | 0) sets the tone's gain through a ParamSmoother (click-free, NFR-031)
//   - a live trigger on any track adds an 880 Hz ping (velocity/127 × 0.5, −60 dB in 300 ms)
// Rendered into track 1's block (BlockBuffers::track[0]) so it runs through the track loop, the master and the meters
// like a real voice will.
#pragma once

#include "engine/stages/BlockBuffers.h"
#include "engine/stages/ParamSmoother.h"

namespace engine::stages
{
class TestToneStage
{
public:
    static constexpr float kFrequency = 440.0f;
    static constexpr float kLevel = 0.25f; // −12 dBFS at volume index 100 (0 dB)

    void setEnabled(bool on) noexcept { toneEnabled_ = on; } // message thread, before start
    bool enabled() const noexcept { return toneEnabled_; }
    void prepare(double sampleRate) noexcept;               // message thread (libm allowed)
    void setVolumeIndex(float index, int rampSamples) noexcept;
    void trigger(std::uint8_t velocity) noexcept;
    // True when it wrote the block (tone on or a ping ringing); false = the track stays silent.
    bool process(StereoBlock& out) noexcept;

private:
    struct Rotor // quadrature oscillator: no libm on the audio thread
    {
        float c = 1.0f, s = 0.0f, dc = 1.0f, ds = 0.0f;
        void setFrequency(double hz, double sampleRate) noexcept;
        void renormalise() noexcept;
    };

    Rotor tone_, ping_;
    ParamSmoother gain_;
    float pingEnv_ = 0.0f, pingDecay_ = 0.0f;
    bool toneEnabled_ = true;
    alignas(16) float gainBuf_[kBlockFrames] {};
};
} // namespace engine::stages
