#include "engine/voice/Interpolators.h"

#include "engine/voice/InterpKernels.h"

namespace engine::voice
{
namespace
{
using kernels::Fixed;

template <typename Kernel>
double renderWith(Kernel kernel, const float* const* src, int channels, std::int64_t srcFrames, double pos,
                  double rate, float* const* dst, int frames) noexcept
{
    if (frames <= 0 || channels <= 0)
        return pos;
    channels = channels > 2 ? 2 : channels;
    const Fixed p = kernels::toFixed(pos), step = kernels::toFixed(rate);
    kernel(src, channels, srcFrames, 0, p, step, dst, frames);
    return kernels::fromFixed(p + step * frames);
}
} // namespace

std::string_view interpolatorName(InterpolatorKind k) noexcept
{
    switch (k)
    {
        case InterpolatorKind::Hermite4: return "hermite4";
        case InterpolatorKind::Sinc16: return "sinc16";
        case InterpolatorKind::Linear: return "linear";
    }
    return "hermite4";
}

double Hermite4::render(const float* const* src, int channels, std::int64_t srcFrames, double pos, double rate,
                        float* const* dst, int frames) noexcept
{
    return renderWith(&kernels::hermite4Checked, src, channels, srcFrames, pos, rate, dst, frames);
}

double Sinc16::render(const float* const* src, int channels, std::int64_t srcFrames, double pos, double rate,
                      float* const* dst, int frames) noexcept
{
    return renderWith(&kernels::sinc16Checked, src, channels, srcFrames, pos, rate, dst, frames);
}

double Linear::render(const float* const* src, int channels, std::int64_t srcFrames, double pos, double rate,
                      float* const* dst, int frames) noexcept
{
    return renderWith(&kernels::linearChecked, src, channels, srcFrames, pos, rate, dst, frames);
}
} // namespace engine::voice
