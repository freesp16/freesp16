#include "engine/sampling/InputStage.h"

#include "engine/dsp/simd/Kernels.h"

#include <cstring>

namespace engine::sampling
{
void InputStage::process(stages::BlockBuffers& b) noexcept
{
    if (b.inputL == nullptr)
    {
        peak_[0] = peak_[1] = 0.0f;
        clip_ = false;
        return;
    }
    const float* inR = b.inputR != nullptr ? b.inputR : b.inputL;
    const float pl = simd::peakAbs(b.inputL, kBlockFrames);
    const float pr = inR == b.inputL ? pl : simd::peakAbs(inR, kBlockFrames);
    clip_ = pl >= kClipLevel || pr >= kClipLevel;
    if (! limiter_on_ || (pl <= limiter_.threshold() && pr <= limiter_.threshold()))
    {
        peak_[0] = pl;
        peak_[1] = pr;
        b.inputR = inR;
        return; // the shaper is the identity here: the raw input is the post-limiter input
    }
    std::memcpy(l_, b.inputL, sizeof l_);
    std::memcpy(r_, inR, sizeof r_);
    limiter_.process(l_, r_, kBlockFrames);
    // The shaper is monotonic in |x|, so the post-limiter peak is the shaped raw peak (no second pass).
    peak_[0] = limiter_.shape(pl);
    peak_[1] = limiter_.shape(pr);
    b.inputL = l_;
    b.inputR = r_;
}
} // namespace engine::sampling
