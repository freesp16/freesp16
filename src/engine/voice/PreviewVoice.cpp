#include "engine/voice/PreviewVoice.h"

#include <cmath>

namespace engine::voice
{
PreviewVoice::~PreviewVoice()
{
    endHead(head_);
    endHead(fading_);
}

void PreviewVoice::prepare(double sampleRate)
{
    engineRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    const long n = std::lround(0.002 * engineRate_);
    fadeFrames_ = n < 1 ? 1 : static_cast<int>(n);
    endHead(head_);
    endHead(fading_);
    installed_ = nullptr;
    gain_ = target_;
}

void PreviewVoice::install(const SampleBuffer* buffer) noexcept
{
    if (buffer == installed_)
        return;
    installed_ = buffer;
    if (head_.active)
    {
        endHead(fading_);
        fading_ = head_;
        head_ = Head {};
        fadeHead(fading_, fadeFrames_);
    }
    if (buffer == nullptr || buffer->frames <= 0)
        return;
    const double rate = buffer->sampleRate > 0.0 ? buffer->sampleRate / engineRate_ : 1.0; // 1 after LIB resampled
    Fixed step = kernels::toFixed(rate);
    step = step < 1 ? 1 : step;
    beginHead(head_, buffer, Region {0, 0, buffer->frames, false, false}, step, InterpolatorKind::Hermite4);
}

void PreviewVoice::setGain(float g) noexcept { target_ = g < 0.0f ? 0.0f : g; }

void PreviewVoice::render(float* auxL, float* auxR) noexcept
{
    float* aux[2] = {auxL, auxR};
    float* s[2] = {scratch_[0], scratch_[1]};
    float* seam[2] = {seam_[0], seam_[1]};
    const float g0 = gain_, dg = (target_ - gain_) / static_cast<float>(kBlockFrames);
    gain_ = target_;
    for (Head* h : {&head_, &fading_})
    {
        if (! h->active)
            continue;
        const int m = h->fadeLeft > 0 && h->fadeLeft < kBlockFrames ? h->fadeLeft : kBlockFrames;
        renderHead(*h, s, m, seam);
        if (dg != 0.0f || g0 != 1.0f) // the preview volume (block ramp) on top of the head's own fade
            for (int c = 0; c < 2; ++c)
                for (int j = 0; j < m; ++j)
                    s[c][j] *= g0 + static_cast<float>(j + 1) * dg;
        const bool finished = h->finished;
        addWithGain(*h, aux, s, m);
        if (h->active && finished)
            endHead(*h);
    }
}
} // namespace engine::voice
