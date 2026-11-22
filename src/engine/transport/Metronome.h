// The metronome (spec 02-features/sequencer.md SEQ-045 click on beats, accent on bar starts; SEQ-044 the recording
// pre-count clicks). Its own sound, synthesised in prepare() — no resource file, nothing
// taken from a device: two 12 ms decaying sines, 1.5 kHz (bar start) and 1 kHz (other beats).
//
// A click starts on the exact frame of its beat (core::musictime::forEachGridPoint / frameOf, the rule every engine
// stage uses) and is added into the aux pair (BlockBuffers::aux, post-master: master + headphones).
// A beat is a pre-count beat when its PPQ is < 0 (the transport's pre-count runs from −4·bars to 0).
// Audio thread: render() costs nothing while silent and one 64-frame table read + add per channel while a click
// sounds (simd kernels).
#pragma once

#include "engine/EngineApi.h"

#include <array>
#include <cstdint>

namespace engine::transport
{
class Metronome
{
public:
    static constexpr int kMaxClickFrames = 1152;  // 12 ms at 96 kHz; longer rates keep 1152 frames
    static constexpr double kClickSeconds = 0.012;
    static constexpr float kPeak = 0.5f;          // click peak at metronome volume 100 (0 dB), −6 dBFS
    static constexpr float kBeatLevel = 0.7f;     // the beat click relative to the accent

    void prepare(double sampleRate) noexcept;   // synthesises the two clicks (message thread; no allocation)
    // on: the metronome may sound; gain: linear (GainLaw of transport.metronomeVolume × kPeak); onlyPreCount: only the
    // pre-count beats click (the recording pre-count with the metronome off).
    void set(bool on, float gain, bool onlyPreCount) noexcept;
    // PPQ of a bar start (the current pattern's start, BlockBuffers::timing.patternStartPpq): accents fall on it ± n·4.
    void setBarOrigin(double ppq) noexcept { barOrigin_ = ppq; }
    void reset() noexcept { pos_ = len_; } // silence a sounding click (hard stop)

    // Adds this block's click into auxL/auxR (64 frames).
    void render(const BlockContext& ctx, float* auxL, float* auxR) noexcept;
    // The two halves of render(): renderMono fills the mono click of this block and returns false when it is silent
    // (then nothing needs to be added); addTo adds that block into a stereo pair.
    bool renderMono(const BlockContext& ctx) noexcept;
    void addTo(float* l, float* r) const noexcept;
    void copyTo(float* l, float* r) const noexcept; // the same into a pair that held nothing yet

    bool sounding() const noexcept { return pos_ < len_; }
    int clickFrames() const noexcept { return len_; }
    const float* accentTable() const noexcept { return accent_.data(); }
    const float* beatTable() const noexcept { return beat_.data(); }

private:
    void play(int from, int to) noexcept; // adds the sounding click's frames [from, to) into block_

    alignas(16) std::array<float, kMaxClickFrames> accent_ {}, beat_ {};
    alignas(16) std::array<float, kBlockFrames> block_ {};
    const float* table_ = beat_.data();
    int len_ = 0, pos_ = 0;
    bool on_ = false, onlyPreCount_ = false;
    float gain_ = 0.0f;
    double barOrigin_ = 0.0;
};
} // namespace engine::transport
