// One playing read head over a SampleBuffer — the shared core of TrackVoice (main + release tails) and PreviewVoice
// (the playback loop). Internal to engine/voice.
//
// Positions are direction-normalised: d = position (forward) or −position (reverse), Q32.32, so the forward and the
// reverse region, loop and crossfade logic is one code path with a positive step:
//   forward  plays [start, end)          d from start,      stops at end;       loops [loopStart, end)
//   reverse  plays (start − 1, end − 1]  d from 1 − end,    stops at 1 − start; loops (loopStart − 1, end − 1]
// Each render splits the frames at the next boundary (crossfade zone, loop end, region end), so the kernels never
// branch per sample. The loop seam (PLY-011) is a linear crossfade over the last X source frames before the loop end
// (X = min(64, loop length / 2)) between the outgoing audio and its continuation one loop length earlier, which the
// jump then continues seamlessly; without X frames of audio before the loop start (forward) or after the region end
// (reverse) the outgoing end fades to silence instead.
//
// A head holds one SampleBuffer reference while active (retain at begin, release at end): a buffer swapped out of
// its slot stays alive until its tail has faded, whatever the slot's retire grace period at the device rate.
#pragma once

#include "core/audio/SampleBuffer.h"
#include "engine/voice/InterpKernels.h"
#include "engine/voice/Interpolators.h"
#include "engine/voice/PlaybackParams.h"

namespace engine::voice
{
using kernels::Fixed;
using SampleBuffer = core::audio::SampleBuffer;

constexpr int kSeamFrames = 64; // PLY-011

struct Head
{
    const SampleBuffer* buf = nullptr;
    Fixed d = 0;              // direction-normalised position
    Fixed step = kernels::kOne; // |rate| > 0
    int dir = 1;              // +1 forward, −1 reverse
    Fixed stopD = 0;          // non-looping end (exclusive)
    Fixed loopEndD = 0;       // looping: wrap boundary
    Fixed loopLen = 0;        // loop length (Q32.32)
    Fixed xfade = 0;          // seam crossfade length in source frames (Q32.32), 0 = hard loop
    bool loop = false, xfadeSilent = false;
    bool active = false;
    bool finished = false;    // a non-looping region reached its end
    InterpolatorKind interp = InterpolatorKind::Hermite4;
    // Voice-internal gain: while fadeLeft > 0, frame j of the next ones plays at (fadeLeft − 1 − j)·delta (a linear
    // fade that reaches exactly 0); `gain` is always the current gain (fadeLeft·delta while fading).
    float gain = 1.0f, delta = 0.0f;
    int fadeLeft = 0;

    std::int64_t position() const noexcept { return kernels::indexOf(dir > 0 ? d : -d); }
    bool fading() const noexcept { return fadeLeft > 0; }
};

// Starts `h` on `buf` (retained) at the beginning of `region` (clamped here) with |rate| `step`.
void beginHead(Head& h, const SampleBuffer* buf, const Region& region, Fixed step, InterpolatorKind interp) noexcept;
// Region / loop change of a playing head (position kept; reverse is fixed for the note).
void setHeadRegion(Head& h, const Region& region) noexcept;
// Ends `h` and drops its buffer reference.
void endHead(Head& h) noexcept;
// Starts (or shortens) a linear fade to zero over `frames` frames.
void fadeHead(Head& h, int frames) noexcept;

// Renders n ≤ 64 frames of `h` (pre-gain) into out[0], out[1] (mono buffers are copied to out[1]); frames after the
// region end are zero. `scratch` (2 × 64 floats) holds the seam's second read. Does not apply the gain.
void renderHead(Head& h, float* const out[2], int n, float* const scratch[2]) noexcept;

// out[c][j] *= the fade's gain for the fading frames, zero after the fade; advances the fade (ending the head
// when it reaches zero). Constant-gain heads are scaled by `gain` unless it is 1.
void applyGain(Head& h, float* const out[2], int n) noexcept;
// dst[c][j] += src[c][j] · (gain ramp), advancing the fade like applyGain.
void addWithGain(Head& h, float* const dst[2], const float* const src[2], int n) noexcept;
} // namespace engine::voice
