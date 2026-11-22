#include "engine/voice/stretch/VoiceStretch.h"

#include "engine/EngineApi.h"
#include "engine/voice/stretch/StretcherBank.h"

#include <algorithm>
#include <cstring>

namespace engine::voice::stretch
{
using engine::kBlockFrames;
namespace
{
void zero(float* const out[2], int n) noexcept
{
    std::memset(out[0], 0, static_cast<std::size_t>(n) * sizeof(float));
    std::memset(out[1], 0, static_cast<std::size_t>(n) * sizeof(float));
}
} // namespace

void VoiceStretch::prepare() noexcept
{
    active_ = draining_ = false;
    tailLen_ = tailPos_ = 0;
    estimatedFor_ = nullptr;
}

TimeStretcher* VoiceStretch::stretcher() const noexcept
{
    return bank_ != nullptr ? bank_->lane(lane_) : nullptr;
}

double VoiceStretch::sampleBpm(const SampleBuffer* b) noexcept
{
    if (sampleBpm_ > 0.0f)
        return sampleBpm_;
    if (b == nullptr || b->frames <= 0 || b->sampleRate <= 0.0)
        return 0.0;
    if (b != estimatedFor_) // PLY-031 estimate, once per buffer (it then follows tempo changes like a known BPM)
    {
        estimatedFor_ = b;
        estimatedBpm_ = sampleTempo(0.0, 0.0, static_cast<double>(b->frames) / b->sampleRate, projectBpm_).bpm;
    }
    return estimatedBpm_;
}

StretchPlan VoiceStretch::plan(StretchMode mode, float cents, const SampleBuffer* b) noexcept
{
    const double bpm = mode == StretchMode::Off ? 0.0 : sampleBpm(b);
    return planFor(mode, projectBpm_, bpm, cents);
}

VoiceStretch::Start VoiceStretch::begin(const StretchPlan& p) noexcept
{
    active_ = false;
    TimeStretcher* s = stretcher();
    if (s == nullptr || bank_->scratchFrames() == 0 || bank_->holdNew())
        return Start::Resample;
    if (! bank_->tryPrime())
        return Start::Deferred;
    s->reset();
    timeRatio_ = p.timeRatio;
    pitch_ = p.pitchScale;
    s->setRatios(timeRatio_, pitch_);
    generation_ = bank_->generation();
    active_ = true;
    draining_ = false;
    inValid_ = inMapped_ = 0.0;
    fadeIn_ = 0;
    return Start::Stretched;
}

void VoiceStretch::blockStart(const StretchPlan& p) noexcept
{
    if (! active_)
        return;
    TimeStretcher* s = stretcher();
    if (s == nullptr)
    {
        active_ = false; // the backend became "none": the caller continues the note without a stretcher
        return;
    }
    if (bank_->generation() != generation_)
    {
        s->reset();
        generation_ = bank_->generation();
        timeRatio_ = 0.0; // force setRatios
    }
    if (p.timeRatio != timeRatio_ || p.pitchScale != pitch_)
    {
        timeRatio_ = p.timeRatio;
        pitch_ = p.pitchScale;
        s->setRatios(timeRatio_, pitch_);
    }
    bank_->countPlayer();
}

bool VoiceStretch::render(Head& h, float* const out[2], int n, float* const scratch[2]) noexcept
{
    TimeStretcher* s = stretcher();
    if (s == nullptr || n <= 0)
    {
        zero(out, n > 0 ? n : 0);
        return true;
    }
    int need = s->framesNeeded(n);
    need = std::clamp(need, 0, bank_->scratchFrames());
    float* in[2] = {bank_->scratch(0), bank_->scratch(1)};
    for (int done = 0; done < need;)
    {
        const int k = std::min(kBlockFrames, need - done);
        float* dst[2] = {in[0] + done, in[1] + done};
        if (! draining_ && h.active)
        {
            if (h.loop)
                inValid_ += k;
            else
            {
                const Fixed left = h.stopD - h.d;
                const double frames = left <= 0 ? 0.0 : static_cast<double>((left + h.step - 1) / h.step);
                inValid_ += frames < k ? frames : static_cast<double>(k);
            }
            renderHead(h, dst, k, scratch);
            if (h.finished)
                draining_ = true;
        }
        else
            zero(dst, k);
        done += k;
    }
    const float* src[2] = {in[0], in[1]};
    s->process(src, need, out, n);
    for (int j = 0; fadeIn_ < kFadeIn && j < n; ++j, ++fadeIn_)
    {
        const float g = static_cast<float>(fadeIn_ + 1) * (1.0f / static_cast<float>(kFadeIn + 1));
        out[0][j] *= g;
        out[1][j] *= g;
    }
    inMapped_ += static_cast<double>(n) / (timeRatio_ > 0.0 ? timeRatio_ : 1.0);
    return draining_ && inMapped_ >= inValid_;
}

void VoiceStretch::handOff(Head& h, float gain, int fadeFrames, float* const scratch[2]) noexcept
{
    // Keep what is left of an earlier tail at the front.
    const int rem = tailLen_ - tailPos_;
    for (int c = 0; c < 2; ++c)
        if (rem > 0 && tailPos_ > 0)
            std::memmove(tail_[c], tail_[c] + tailPos_, static_cast<std::size_t>(rem) * sizeof(float));
    tailPos_ = 0;
    tailLen_ = rem > 0 ? rem : 0;
    const int fade = std::clamp(fadeFrames, 1, kMaxTail);
    if (active_ && gain > 0.0f)
    {
        const int len = std::max(tailLen_, fade);
        for (int c = 0; c < 2; ++c)
            std::memset(tail_[c] + tailLen_, 0, static_cast<std::size_t>(len - tailLen_) * sizeof(float));
        alignas(16) float tmp[2][kBlockFrames];
        float* out[2] = {tmp[0], tmp[1]};
        const float delta = gain / static_cast<float>(fade);
        bool done = false;
        for (int at = 0; at < fade && ! done; at += kBlockFrames)
        {
            const int k = std::min(kBlockFrames, fade - at);
            done = render(h, out, k, scratch);
            for (int j = 0; j < k; ++j)
            {
                const float g = static_cast<float>(fade - 1 - (at + j)) * delta;
                tail_[0][at + j] += tmp[0][j] * g;
                tail_[1][at + j] += tmp[1][j] * g;
            }
        }
        tailLen_ = len;
    }
    active_ = false;
}

void VoiceStretch::addTail(float* const tail[2], int n) noexcept
{
    const int m = std::min(n, tailLen_ - tailPos_);
    for (int j = 0; j < m; ++j)
    {
        tail[0][j] += tail_[0][tailPos_ + j];
        tail[1][j] += tail_[1][tailPos_ + j];
    }
    tailPos_ += m;
    if (tailPos_ >= tailLen_)
        tailLen_ = tailPos_ = 0;
}
} // namespace engine::voice::stretch
