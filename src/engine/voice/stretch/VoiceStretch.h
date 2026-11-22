// The stretch stage of one TrackVoice (behind the TrackVoice hooks). TrackVoice owns one and
// calls it for its main note when TIME STRETCH is RESAMPLE or MASTER TEMPO:
//
//   attach(bank, lane)        message thread: the engine's StretcherBank and this voice's lane (no bank = no
//                             stretcher: MASTER TEMPO plays as RESAMPLE)
//   setTempo(project, sample) audio thread, every block: the transport BPM and the sample's BPM (0 = unknown → the
//                             PLY-031 estimate from the buffer length at the project tempo, cached per buffer)
//   plan(mode, cents, buffer) the StretchPlan of the note (StretchMode.h)
//   begin(plan) / active()    MASTER TEMPO note start: resets the lane and sets its ratios (Start below)
//   render(head, out, n)      n ≤ 64 output frames: pulls framesNeeded(n) frames from the head (64-frame chunks into
//                             the bank's shared scratch) through the stretcher; true once a non-looping region has
//                             played out completely (the input mapped by the output covers every valid head frame)
//   handOff(head, gain, fade) retrigger of a stretched note: the next `fade` output frames of the old note are
//                             rendered now, faded linearly from `gain` to 0, into the stretch tail (the stretcher is
//                             reset by the next note); a tail still sounding is kept and summed (no cut)
//   addTail(tail, n)          adds and advances the stretch tail
// Real-time: everything but attach() is noexcept and allocation-free.
#pragma once

#include "engine/voice/VoiceHead.h"
#include "engine/voice/stretch/StretchMode.h"

#include <cstdint>

namespace engine::voice
{
class TimeStretcher;
}

namespace engine::voice::stretch
{
class StretcherBank;

class VoiceStretch
{
public:
    static constexpr int kMaxTail = 256; // ≥ 2 ms at 96 kHz (PLY-042)
    // A stretched note's first frames are faded in (≈ 0.7 ms at 44.1 kHz): the grains around output frame 0
    // read the input around it (at R ≠ 1 not exactly frame 0), so the first output frame is not the sample's first
    // frame and would click on a retrigger.
    static constexpr int kFadeIn = 32;

    void attach(StretcherBank* bank, int lane) noexcept
    {
        bank_ = bank;
        lane_ = lane;
    }
    void prepare() noexcept;
    void setTempo(double projectBpm, float sampleBpm) noexcept
    {
        projectBpm_ = projectBpm;
        sampleBpm_ = sampleBpm;
    }
    double projectBpm() const noexcept { return projectBpm_; }
    double sampleBpm(const SampleBuffer* b) noexcept; // the known or estimated BPM of `b` (0 without a buffer)
    StretchPlan plan(StretchMode mode, float cents, const SampleBuffer* b) noexcept;

    enum class Start : std::uint8_t { Stretched, Resample, Deferred };
    // Resample: no lane ("none" backend, no bank) or the governor holds new MASTER TEMPO notes; Deferred: this
    // block's priming budget is spent (try again at the next block).
    Start begin(const StretchPlan& p) noexcept;
    // Every block of a stretched note: the ratios (tempo, pitch), the governor's player count; a backend swap
    // since begin() resets the new lane (the note continues from the head's position).
    void blockStart(const StretchPlan& p) noexcept;
    bool active() const noexcept { return active_; }
    void end() noexcept { active_ = false; }

    bool render(Head& h, float* const out[2], int n, float* const scratch[2]) noexcept;
    void handOff(Head& h, float gain, int fadeFrames, float* const scratch[2]) noexcept;
    void addTail(float* const tail[2], int n) noexcept;
    bool tailActive() const noexcept { return tailPos_ < tailLen_; }

private:
    TimeStretcher* stretcher() const noexcept;

    StretcherBank* bank_ = nullptr;
    int lane_ = 0;
    double projectBpm_ = kDefaultBpm;
    float sampleBpm_ = 0.0f;
    const SampleBuffer* estimatedFor_ = nullptr;
    double estimatedBpm_ = 0.0;

    bool active_ = false, draining_ = false;
    int fadeIn_ = kFadeIn; // frames of the fade-in played
    std::uint32_t generation_ = 0;
    double timeRatio_ = 1.0, pitch_ = 1.0;
    double inValid_ = 0.0, inMapped_ = 0.0; // head frames fed before the region end / input covered by the output

    alignas(16) float tail_[2][kMaxTail] {};
    int tailLen_ = 0, tailPos_ = 0;
};
} // namespace engine::voice::stretch
