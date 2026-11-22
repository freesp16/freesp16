#include "engine/transport/Metronome.h"

#include "core/util/musictime/MusicTime.h"
#include "engine/dsp/simd/Kernels.h"

#include <cmath>
#include <cstring>

namespace engine::transport
{
namespace
{
constexpr double kAccentHz = 1500.0, kBeatHz = 1000.0;
constexpr double kDecaySeconds = 0.003; // e^(−t/3 ms) × a linear fade to 0 at 12 ms: no step at the end
constexpr double kTwoPi = 6.283185307179586476925;
} // namespace

void Metronome::prepare(double sampleRate) noexcept
{
    const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const long n = std::lround(kClickSeconds * rate);
    len_ = n < 1 ? 1 : (n > kMaxClickFrames ? kMaxClickFrames : static_cast<int>(n));
    accent_.fill(0.0f);
    beat_.fill(0.0f);
    const auto level = static_cast<double>(kBeatLevel);
    for (int i = 0; i < len_; ++i)
    {
        const double t = i / rate;
        const double env = std::exp(-t / kDecaySeconds) * (1.0 - static_cast<double>(i) / len_);
        accent_[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(kTwoPi * kAccentHz * t) * env);
        beat_[static_cast<std::size_t>(i)] = static_cast<float>(level * std::sin(kTwoPi * kBeatHz * t) * env);
    }
    pos_ = len_; // silent
    table_ = beat_.data();
}

void Metronome::set(bool on, float gain, bool onlyPreCount) noexcept
{
    on_ = on;
    gain_ = gain > 0.0f ? gain : 0.0f;
    onlyPreCount_ = onlyPreCount;
}

void Metronome::play(int from, int to) noexcept
{
    int n = 0;
    if (pos_ < len_ && to > from)
    {
        n = to - from < len_ - pos_ ? to - from : len_ - pos_;
        simd::scale(block_.data() + from, table_ + pos_, gain_, n);
        pos_ += n;
    }
    if (from + n < to)
        std::memset(block_.data() + from + n, 0, sizeof(float) * static_cast<std::size_t>(to - from - n));
}

bool Metronome::renderMono(const BlockContext& ctx) noexcept
{
    constexpr int kMaxClicks = 4; // ≤ 1 beat per block below 2,000 BPM; a Locate may add one
    int at[kMaxClicks];
    const float* table[kMaxClicks];
    int clicks = 0;
    if (on_ && gain_ > 0.0f && ctx.ppqEnd > ctx.ppqStart)
    {
        const double ppf = (ctx.ppqEnd - ctx.ppqStart) / kBlockFrames;
        core::musictime::forEachGridPoint(ctx.ppqStart, ppf, kBlockFrames, 1.0,
                                          [&](double beat, int frame) noexcept
                                          {
                                              if ((onlyPreCount_ && beat >= 0.0) || clicks == kMaxClicks)
                                                  return;
                                              const double bars = (beat - barOrigin_) / 4.0;
                                              const double nearest = std::floor(bars + 0.5);
                                              const bool accent = std::fabs(bars - nearest) < 1e-6;
                                              at[clicks] = frame;
                                              table[clicks] = accent ? accent_.data() : beat_.data();
                                              ++clicks;
                                          });
    }
    if (clicks == 0 && ! sounding())
        return false;
    int from = 0;
    for (int i = 0; i < clicks; ++i)
    {
        play(from, at[i]);
        table_ = table[i];
        pos_ = 0;
        from = at[i];
    }
    play(from, kBlockFrames);
    return true;
}

void Metronome::addTo(float* l, float* r) const noexcept
{
    simd::add(l, block_.data(), kBlockFrames);
    simd::add(r, block_.data(), kBlockFrames);
}

void Metronome::copyTo(float* l, float* r) const noexcept
{
    std::memcpy(l, block_.data(), sizeof(float) * kBlockFrames);
    std::memcpy(r, block_.data(), sizeof(float) * kBlockFrames);
}

void Metronome::render(const BlockContext& ctx, float* auxL, float* auxR) noexcept
{
    if (renderMono(ctx))
        addTo(auxL, auxR);
}
} // namespace engine::transport
