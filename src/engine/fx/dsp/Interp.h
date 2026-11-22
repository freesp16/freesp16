// Interpolation kernels of the effects DSP kit (fractional delay reads, table oscillators). Scalar, inline, non-fused
// (Numerics.h). `f` is the fractional position in [0, 1) between x0 and x1.
#pragma once

#include "engine/fx/dsp/Numerics.h"

namespace engine::fx::dsp::interp
{
// x0 + f·(x1 − x0)
inline float linear(float x0, float x1, float f) noexcept
{
    APP_FX_NOFUSE
    const float d = x1 - x0;
    const float p = f * d;
    return x0 + p;
}

// 4-point, 3rd-order Hermite (Catmull-Rom) through xm1, x0, x1, x2 at x0 + f.
inline float hermite(float xm1, float x0, float x1, float x2, float f) noexcept
{
    APP_FX_NOFUSE
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = (xm1 - 2.5f * x0) + (2.0f * x1 - 0.5f * x2);
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}
} // namespace engine::fx::dsp::interp
