// Block kernels of the track mixer (spec 01-architecture/audio-engine.md "Track strip", "Buses").
// Every kernel has a scalar reference (`scalar::`) and a NEON version (`neon::`, ARMv7 intrinsics subset, only where
// __ARM_NEON is defined); tests/unit/mixer compares them bit for bit on the host (Apple silicon runs the same
// intrinsics). The unqualified functions dispatch at compile time, like engine/dsp/simd/Kernels.h.
//
// Arithmetic follows the research model operation for operation (tools/fx/track_mix.py, F101: bit-exact for normal
// audio): every gain is its own float multiply with its own rounding — pan, then mute, then the send tap (copy × send
// gain), then the fader — and the buses are float additions in track order. MixerKernels.cpp is compiled without FMA
// contraction for that reason (the engine's REALTIME flags allow it elsewhere; the A9 has no fused NEON multiply-add).
//
// Layout: one strip's four gains are interleaved per frame, g[4 * i + lane] (lane order PanL, PanR, Send, Fader), so
// the NEON kernel reads them with one vld4q per 4 frames and the ramp generator advances the four ramps of a strip in
// one 4-lane vector (structure of arrays over the lanes, no scalar recurrence per gain).
#pragma once

#include <cstdint>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
 #define APP_MIXER_HAVE_NEON 1
#else
 #define APP_MIXER_HAVE_NEON 0
#endif

namespace engine::mixer::kernels
{
enum Lane : int
{
    PanL = 0,
    PanR = 1,
    Send = 2,
    Fader = 3,
    kLanes = 4
};

// Per-block meter values of one stereo signal: peak |x| and sum of squares per channel.
struct StereoMeter
{
    float peakL = 0.0f, peakR = 0.0f, sumL = 0.0f, sumR = 0.0f;
};

// One strip (pan → mute → send tap → fader → buses). n is a multiple of 4 (the engine block is 64).
struct StripArgs
{
    float* l = nullptr;            // the track block, in place: holds the post-fader signal afterwards
    float* r = nullptr;
    float* mainL = nullptr;        // main bus (+=)
    float* mainR = nullptr;
    float* sendL = nullptr;        // send bus (+=), tapped post-mute and pre-fader; null = no send tap this block
    float* sendR = nullptr;
    const float* gains = nullptr;  // rampGains: 4 × n interleaved per frame; else the 4 constant gains
    const float* mute = nullptr;   // n per-frame mute gains; null = open (gain 1, no multiply)
    bool rampGains = false;
};

// The four linear ramps of one strip over one block (track_mix.Smoother, state 2 → 1 → 0): lane k holds `cur` before
// frame start[k], steps by `step` from start[k] to end[k] − 1 (the value of a frame is the one before its step) and is
// `target` from end[k] on. Idle lanes have start = end = 0.
struct alignas(16) Ramp4
{
    float cur[kLanes];
    float step[kLanes];
    float target[kLanes];
    std::int32_t start[kLanes];
    std::int32_t end[kLanes];
};

namespace scalar
{
StereoMeter strip(const StripArgs& a, int n) noexcept;
// main += x (a strip at unity with no ramp, no mute, no send: bit-transparent, NFR-030), with the meters.
StereoMeter unity(const float* l, const float* r, float* mainL, float* mainR, int n) noexcept;
// out[4 i + k] = lane k at frame i; accOut[k] = the lane's running value after the block (the next block's `cur`).
void rampFill(const Ramp4& s, float* out, float* accOut, int n) noexcept;
StereoMeter meter(const float* l, const float* r, int n) noexcept;
} // namespace scalar

#if APP_MIXER_HAVE_NEON
namespace neon
{
StereoMeter strip(const StripArgs& a, int n) noexcept;
StereoMeter unity(const float* l, const float* r, float* mainL, float* mainR, int n) noexcept;
void rampFill(const Ramp4& s, float* out, float* accOut, int n) noexcept;
StereoMeter meter(const float* l, const float* r, int n) noexcept;
} // namespace neon
namespace active = neon;
#else
namespace active = scalar;
#endif

inline StereoMeter strip(const StripArgs& a, int n) noexcept { return active::strip(a, n); }
inline StereoMeter unity(const float* l, const float* r, float* mainL, float* mainR, int n) noexcept
{
    return active::unity(l, r, mainL, mainR, n);
}
inline void rampFill(const Ramp4& s, float* out, float* accOut, int n) noexcept { active::rampFill(s, out, accOut, n); }
inline StereoMeter meter(const float* l, const float* r, int n) noexcept { return active::meter(l, r, n); }

constexpr bool hasNeon() noexcept { return APP_MIXER_HAVE_NEON != 0; }
} // namespace engine::mixer::kernels
