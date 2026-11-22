#include "engine/voice/stretch/GrainStretcher.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::voice::stretch
{
namespace
{
std::int64_t nextPow2(std::int64_t v) noexcept
{
    std::int64_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

void copyFloats(float* dst, const float* src, std::int64_t n) noexcept
{
    if (n > 0)
        std::memcpy(dst, src, static_cast<std::size_t>(n) * sizeof(float));
}
} // namespace

void GrainStretcher::prepare(double sampleRate, int maxBlock, int channels)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlock_ = std::max(1, maxBlock);
    channels_ = std::clamp(channels, 1, 2);

    int maxW = 0, maxHop = 0, minHop = 1 << 30, maxSearch = 0, maxWarm = 0;
    for (int t = 0; t < kTiers; ++t)
    {
        const Geometry g = geometryFor(t, sampleRate_);
        maxW = std::max(maxW, g.window);
        maxHop = std::max(maxHop, g.hop);
        minHop = std::min(minHop, g.hop);
        maxSearch = std::max(maxSearch, g.search);
        maxWarm = std::max(maxWarm, g.window / g.hop - 1);
    }
    // Grains one call can compute, and the farthest core frame they read (Ha ≤ hop / (0.25 · 0.5)).
    const std::int64_t grainsPerCall = maxWarm + (maxBlock_ + minHop - 1) / minHop + 2; // + 1 for the compute-ahead
    const double maxHa = maxHop / (kMinTimeRatio * kMinPitch);
    const auto coreSpan = static_cast<std::int64_t>(grainsPerCall * maxHa) + maxW + 2 * maxSearch + 64;
    const std::int64_t coreSize = nextPow2(coreSpan);
    coreMask_ = coreSize - 1;
    // Input frames per call: Ha · pitch = hop / timeRatio ≤ 4 · hop per grain, plus one grain's span at pitch 2.
    maxIn_ = static_cast<int>(grainsPerCall * maxHop / kMinTimeRatio + kMaxPitch * (maxW + maxSearch) + 8);
    const std::int64_t fifoSize = nextPow2(maxBlock_ + 2 * maxHop + 1); // + the compute-ahead (< one hop)
    fifoMask_ = fifoSize - 1;
    for (int c = 0; c < 2; ++c)
    {
        const bool used = c < channels_;
        stage_[c].assign(used ? static_cast<std::size_t>(maxIn_ + 16) : 0u, 0.0f);
        core_[c].assign(used ? static_cast<std::size_t>(coreSize) : 0u, 0.0f);
        acc_[c].assign(used ? static_cast<std::size_t>(maxW) : 0u, 0.0f);
        fifo_[c].assign(used ? static_cast<std::size_t>(fifoSize) : 0u, 0.0f);
    }
    tier_ = pendingTier_;
    geo_ = geometryFor(tier_, sampleRate_);
    allocateCore(channels_);
    grains_ = 0;
    reset();
}

void GrainStretcher::reset() noexcept
{
    tier_ = pendingTier_;
    geo_ = geometryFor(tier_, sampleRate_);
    anaHop_ = toFixed(geo_.hop / ratio_);
    // Two zero frames before input frame 0 (the Hermite resampler reads x[i − 1]).
    stageBase_ = -2;
    inWritten_ = 0;
    for (int c = 0; c < channels_; ++c)
    {
        stage_[c][0] = stage_[c][1] = 0.0f;
        std::fill(acc_[c].begin(), acc_[c].end(), 0.0f);
    }
    rpos_ = 0;
    coreWritten_ = 0;
    // Grain 0's synthesis span is [hop − W, hop); its centre hop − W/2 maps to the analysis centre (hop − W/2) / R.
    const double s0 = geo_.hop - geo_.window;
    const double a0 = (s0 + geo_.window * 0.5) / ratio_ - geo_.window * 0.5;
    anaPos_ = toFixed(a0);
    prevAna_ = floorOf(anaPos_);
    warm_ = geo_.window / geo_.hop - 1;
    fifoRead_ = fifoWrite_ = 0;
    firstGrainIndex_ = grains_;
    resetCore();
}

void GrainStretcher::setRatios(double timeRatio, double pitchScale) noexcept
{
    timeRatio_ = std::isfinite(timeRatio) ? std::clamp(timeRatio, kMinTimeRatio, kMaxTimeRatio) : 1.0;
    pitch_ = std::isfinite(pitchScale) ? std::clamp(pitchScale, kMinPitch, kMaxPitch) : 1.0;
    ratio_ = timeRatio_ * pitch_;
    anaHop_ = toFixed(geo_.hop / ratio_);
    step_ = toFixed(pitch_);
}

int GrainStretcher::framesNeeded(int outFrames) const noexcept
{
    const int need = (outFrames > 0 ? outFrames + ahead() : 0) - fifoLevel();
    if (need <= 0)
        return 0;
    const std::int64_t grains = warm_ + (need + geo_.hop - 1) / geo_.hop;
    const std::int64_t lastStart = floorOf(anaPos_ + (grains - 1) * anaHop_);
    const std::int64_t coreNeed = lastStart + geo_.window + geo_.search - coreWritten_;
    if (coreNeed <= 0)
        return 0;
    const std::int64_t last = floorOf(rpos_ + (coreNeed - 1) * step_);
    const std::int64_t inNeed = last + 3 - inWritten_;
    return inNeed <= 0 ? 0 : static_cast<int>(std::min<std::int64_t>(inNeed, maxIn_));
}

int GrainStretcher::process(const float* const* in, int inFrames, float* const* out, int outFrames) noexcept
{
    // 1  append the input to the staging buffer
    const std::int64_t room = static_cast<std::int64_t>(stage_[0].size()) - (inWritten_ - stageBase_);
    const std::int64_t n = std::clamp<std::int64_t>(inFrames, 0, room);
    for (int c = 0; c < channels_; ++c)
    {
        float* dst = stage_[c].data() + (inWritten_ - stageBase_);
        if (in != nullptr && in[c] != nullptr)
            copyFloats(dst, in[c], n);
        else
            std::fill(dst, dst + n, 0.0f);
    }
    inWritten_ += n;
    // 2  resample into the core ring, 3  grains until the FIFO can serve the request
    resample();
    const int target = outFrames > 0 ? outFrames + ahead() : 0;
    while (fifoLevel() < target)
    {
        if (floorOf(anaPos_) + geo_.window + geo_.search > coreWritten_)
            break;
        emitGrain();
    }
    // 4  pop (an underrun — the caller fed less than framesNeeded — plays zeros)
    const int m = std::min(outFrames, fifoLevel());
    for (int c = 0; c < channels_; ++c)
    {
        const std::int64_t r = fifoRead_ & fifoMask_;
        const std::int64_t first = std::min<std::int64_t>(m, fifoMask_ + 1 - r);
        copyFloats(out[c], fifo_[c].data() + r, first);
        copyFloats(out[c] + first, fifo_[c].data(), m - first);
        std::fill(out[c] + m, out[c] + outFrames, 0.0f);
    }
    fifoRead_ += m;
    return outFrames;
}

void GrainStretcher::resample() noexcept
{
    // Oldest core frame a grain may still read; the ring must not overwrite it.
    const std::int64_t oldest = std::min(prevAna_, floorOf(anaPos_)) - geo_.search;
    const std::int64_t limit = std::max<std::int64_t>(oldest, 0) + coreMask_ + 1;
    if (step_ == kOne && (rpos_ & (kOne - 1)) == 0)
    {
        // Pitch 1 on an integer position: a copy (the same 2-frame look-ahead as the Hermite path).
        const std::int64_t i0 = floorOf(rpos_);
        const std::int64_t count = std::min(inWritten_ - 2 - i0, limit - coreWritten_);
        if (count > 0)
        {
            for (int c = 0; c < channels_; ++c)
            {
                const float* src = stage_[c].data() + (i0 - stageBase_);
                float* ring = core_[c].data();
                const std::int64_t w = coreWritten_ & coreMask_;
                const std::int64_t first = std::min(count, coreMask_ + 1 - w);
                copyFloats(ring + w, src, first);
                copyFloats(ring, src + first, count - first);
            }
            coreWritten_ += count;
            rpos_ += count * kOne;
        }
    }
    else
    {
        while (coreWritten_ < limit)
        {
            const std::int64_t i = floorOf(rpos_);
            if (i + 2 >= inWritten_)
                break;
            const float f = static_cast<float>(static_cast<double>(rpos_ & (kOne - 1)) * (1.0 / 4294967296.0));
            const std::int64_t k = i - stageBase_;
            const std::int64_t w = coreWritten_ & coreMask_;
            for (int c = 0; c < channels_; ++c)
            {
                const float* x = stage_[c].data() + k;
                const float xm1 = x[-1], x0 = x[0], x1 = x[1], x2 = x[2];
                const float c1 = 0.5f * (x1 - xm1);
                const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                core_[c][static_cast<std::size_t>(w)] = ((c3 * f + c2) * f + c1) * f + x0;
            }
            ++coreWritten_;
            rpos_ += step_;
        }
    }
    // Drop the staged input no future core frame reads (keep x[i − 1] of the next one).
    const std::int64_t keep = floorOf(rpos_) - 1;
    if (keep > stageBase_)
    {
        const std::int64_t shift = std::min(keep, inWritten_) - stageBase_;
        const std::int64_t left = inWritten_ - stageBase_ - shift;
        const auto bytes = static_cast<std::size_t>(left > 0 ? left : 0) * sizeof(float);
        for (int c = 0; c < channels_; ++c)
            if (bytes > 0)
                std::memmove(stage_[c].data(), stage_[c].data() + shift, bytes);
        stageBase_ += shift;
    }
}

void GrainStretcher::emitGrain() noexcept
{
    const std::int64_t a = floorOf(anaPos_);
    const int hop = firstGrain() ? 0 : static_cast<int>(a - prevAna_);
    synthesise(a, hop);
    prevAna_ = a;
    anaPos_ += anaHop_;
    ++grains_;
    const int hs = geo_.hop, w = geo_.window;
    for (int c = 0; c < channels_; ++c)
    {
        float* x = acc_[c].data();
        if (warm_ == 0)
        {
            const std::int64_t at = fifoWrite_ & fifoMask_;
            const std::int64_t first = std::min<std::int64_t>(hs, fifoMask_ + 1 - at);
            copyFloats(fifo_[c].data() + at, x, first);
            copyFloats(fifo_[c].data(), x + first, hs - first);
        }
        std::memmove(x, x + hs, static_cast<std::size_t>(w - hs) * sizeof(float));
        std::fill(x + (w - hs), x + w, 0.0f);
    }
    if (warm_ > 0)
        --warm_;
    else
        fifoWrite_ += hs;
}

void GrainStretcher::readCore(int c, std::int64_t start, int n, float* dst) const noexcept
{
    int i = 0;
    for (; i < n && start + i < 0; ++i) // before the first input frame: silence
        dst[i] = 0.0f;
    const std::int64_t end = std::min<std::int64_t>(start + n, coreWritten_);
    std::int64_t p = start + i;
    const float* ring = core_[c].data();
    while (p < end)
    {
        const std::int64_t at = p & coreMask_;
        const std::int64_t run = std::min(end - p, coreMask_ + 1 - at);
        copyFloats(dst + (p - start), ring + at, run);
        p += run;
    }
    for (std::int64_t q = std::max(p, start + i); q < start + n; ++q) // not written yet (never in normal use)
        dst[q - start] = 0.0f;
}
} // namespace engine::voice::stretch
