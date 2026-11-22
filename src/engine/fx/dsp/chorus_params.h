// Parameter plumbing of the reference's LFO effects (Chorus, Flanger, Phaser), operation for operation as
// the MIT models tools/fx/chorus.py (LinParam, Toggle, rint_idx; imported by flanger.py) and tools/fx/phaser.py /
// lofi.py (Param "float", Toggle) of the research (pinned commit 8901c97; research docs/effects/chorus.md "Parameters",
// parameters.md F026, CC BY 4.0).
//
//   LinParam   the reference's LinearParameter (float kind): set() stores the value; changed() is true (and consumes
//              the change) when |f32(last − cur)| > FLT_MIN — the per-block change detection of F026.
//   Toggle     ToggleParameter: v >= 1 → on, v <= 0 → off, anything else (and NaN) leaves it unchanged.
//   rintIdx    BasicParamSpec's index: round-half-even of f32(lo + f32(v·(hi − 1 − lo))) (the double + 1.5·2^52 trick).
//   syncHz     Chorus/Flanger SYNC rate: Hz = f32(f32(f32(b · bpm) / a) · f32(1/60)) for a cycle of a/b beats.
//
// Message/block-rate helpers (never per sample). Float32 steps are single operations, never fused (APP_FX_NOFUSE).
#pragma once

#include "engine/fx/dsp/Numerics.h"

#include <cmath>
#include <cstdint>

namespace engine::fx::dsp::mod
{
constexpr float kFltMin = 1.17549435e-38f;

struct LinParam
{
    float last = 0.0f, cur = 0.0f;
    void init(float v) noexcept { last = cur = v; }
    void set(float v) noexcept { cur = v; }
    bool changed() noexcept
    {
        APP_FX_NOFUSE
        const float d = last - cur;
        if (d <= kFltMin && -kFltMin <= d)
            return false;
        last = cur;
        return true;
    }
    float get() const noexcept { return cur; }
};

struct Toggle
{
    std::uint8_t last = 0, cur = 0;
    void init(int v) noexcept { last = cur = static_cast<std::uint8_t>(v != 0); }
    void set(float v) noexcept
    {
        if (v >= 1.0f)
            cur = 1;
        else if (v <= 0.0f)
            cur = 0;
    }
    bool changed() noexcept
    {
        const bool c = cur != last;
        if (c)
            last = cur;
        return c;
    }
    int get() const noexcept { return cur; }
};

inline int rintIdx(int lo, int hi, float v) noexcept
{
    APP_FX_NOFUSE
    const float x = static_cast<float>(lo) + v * static_cast<float>(hi - 1 - lo);
    return static_cast<int>(std::nearbyint(static_cast<double>(x))); // default rounding mode: half to even
}

inline float syncHz(int a, int b, float bpm) noexcept
{
    APP_FX_NOFUSE
    constexpr float kSixtieth = 1.0f / 60.0f; // 0x3c888889
    return ((static_cast<float>(b) * bpm) / static_cast<float>(a)) * kSixtieth;
}

// Kernel slot 3 of c_chorus / c_flanger: |hz| clamped to [lo, hi] (NaN passes), the sign kept. Returns the argument
// for the oscillator; `mag` gets the stored rate.
inline float clampRate(float hz, float lo, float hi, float& mag) noexcept
{
    APP_FX_NOFUSE
    float sign;
    if (! (hz < 0.0f))
    {
        sign = 1.0f;
        mag = hz;
    }
    else
    {
        sign = -1.0f;
        mag = -hz;
    }
    if (mag < lo)
    {
        mag = lo;
        return sign * lo;
    }
    if (mag <= hi || mag != mag)
        return mag * sign;
    mag = hi;
    return sign * hi;
}
} // namespace engine::fx::dsp::mod
