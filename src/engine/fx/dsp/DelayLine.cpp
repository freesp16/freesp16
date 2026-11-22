#include "engine/fx/dsp/DelayLine.h"

#include "engine/fx/dsp/Interp.h"
#include "engine/fx/dsp/Vec.h"

namespace engine::fx::dsp
{
std::size_t DelayLine::prepare(int maxAge)
{
    cap_ = (maxAge > 1 ? maxAge : 1) + 4;
    storeL_ = std::make_unique<float[]>(static_cast<std::size_t>(cap_)); // rt-ok: message thread, prepare()
    storeR_ = std::make_unique<float[]>(static_cast<std::size_t>(cap_)); // rt-ok: message thread, prepare()
    bufL_ = storeL_.get();
    bufR_ = storeR_.get();
    w_ = filled_ = 0;
    return bytes();
}

void DelayLine::hardClear() noexcept
{
    if (cap_ > 0)
    {
        std::memset(bufL_, 0, sizeof(float) * static_cast<std::size_t>(cap_));
        std::memset(bufR_, 0, sizeof(float) * static_cast<std::size_t>(cap_));
    }
    w_ = filled_ = 0;
}

void DelayLine::writeBlock(const float* l, const float* r, int n) noexcept
{
    int done = 0;
    while (done < n)
    {
        const int room = cap_ - w_;
        const int m = n - done < room ? n - done : room;
        vec::copy(bufL_ + w_, l + done, m);
        vec::copy(bufR_ + w_, r + done, m);
        done += m;
        w_ += m;
        if (w_ == cap_)
            w_ = 0;
    }
    filled_ = filled_ + n < cap_ ? filled_ + n : cap_;
}

void DelayLine::readLinear(float age, float& l, float& r) const noexcept
{
    const int i = static_cast<int>(age);
    const float f = age - static_cast<float>(i);
    // Age i is the newer neighbour, i + 1 the older: the fraction moves towards the older sample.
    l = interp::linear(readL(i), readL(i + 1), f);
    r = interp::linear(readR(i), readR(i + 1), f);
}

void DelayLine::readHermite(float age, float& l, float& r) const noexcept
{
    const int i = static_cast<int>(age);
    const float f = age - static_cast<float>(i);
    l = interp::hermite(readL(i - 1), readL(i), readL(i + 1), readL(i + 2), f);
    r = interp::hermite(readR(i - 1), readR(i), readR(i + 1), readR(i + 2), f);
}

void DelayLine::readBlock(float* l, float* r, int age, int n) const noexcept
{
    int j = 0;
    if (age > filled_) // the oldest part of the block is older than the last clear: silence
    {
        const int z = age - filled_ < n ? age - filled_ : n;
        std::memset(l, 0, sizeof(float) * static_cast<std::size_t>(z));
        std::memset(r, 0, sizeof(float) * static_cast<std::size_t>(z));
        j = z;
    }
    if (j >= n)
        return;
    int i = index(age - j);
    while (j < n)
    {
        const int room = cap_ - i;
        const int m = n - j < room ? n - j : room;
        vec::copy(l + j, bufL_ + i, m);
        vec::copy(r + j, bufR_ + i, m);
        j += m;
        i += m;
        if (i == cap_)
            i = 0;
    }
}
} // namespace engine::fx::dsp
