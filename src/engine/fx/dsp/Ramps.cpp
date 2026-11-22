#include "engine/fx/dsp/Ramps.h"

#include "engine/fx/dsp/Vec.h"

namespace engine::fx::dsp
{
namespace
{
float inverseOf(int n) noexcept
{
    return static_cast<float>(1.0 / static_cast<double>(n));
}
void zero(float* l, float* r, int n) noexcept
{
    std::memset(l, 0, sizeof(float) * static_cast<std::size_t>(n));
    std::memset(r, 0, sizeof(float) * static_cast<std::size_t>(n));
}
} // namespace

// ---------------------------------------------------------------------------------------------------- SmoothFloat

void SmoothFloat::prepare(double fs) noexcept
{
    fs_ = fs;
    state_ = 0;
    cur_ = target_ = step_ = 0.0f;
    rem_ = 0;
    const int n = static_cast<int>(static_cast<float>(fs) * 0.0033333332f);
    n_ = n > 1 ? n : 1;
    inv_ = inverseOf(n_);
}

void SmoothFloat::prepareSamples(double fs, int n) noexcept
{
    fs_ = fs;
    state_ = 0;
    cur_ = target_ = step_ = 0.0f;
    rem_ = 0;
    n_ = n > 1 ? n : 1;
    inv_ = inverseOf(n_);
}

void SmoothFloat::setTime(float ms) noexcept
{
    if (! (ms > 0.0f))
        return;
    const int n = static_cast<int>((ms * 0.001f) * static_cast<float>(fs_));
    if (n < 1)
        return;
    if (state_ == 0)
    {
        n_ = n;
        inv_ = inverseOf(n);
        return;
    }
    // A running ramp restarts from the current value with the new length (reference setSmoothingSamples).
    const float old = target_;
    target_ = cur_;
    state_ = 0;
    n_ = n;
    inv_ = inverseOf(n);
    if (! nearlyEqual(old, cur_))
    {
        target_ = old;
        state_ = 1;
        rem_ = n_;
        step_ = (old - cur_) * inv_;
    }
}

void SmoothFloat::snap(float v) noexcept
{
    state_ = 0;
    cur_ = target_ = v;
    step_ = 0.0f;
    rem_ = 0;
}

void SmoothFloat::setTarget(float v) noexcept
{
    if (nearlyEqual(v, target_))
        return;
    target_ = v;
    state_ = 1;
    rem_ = n_;
    step_ = (v - cur_) * inv_;
}

void SmoothFloat::block(float* g, int n) noexcept
{
    if (n < 1)
        return;
    float c = cur_;
    int rem = rem_;
    int k = 0;
    if (state_ != 0)
    {
        if (n < rem)
        {
            for (; k < n; ++k)
            {
                g[k] = c;
                c += step_;
            }
            cur_ = c;
            rem_ = rem - n;
            return;
        }
        for (int j = 0; j < rem; ++j)
        {
            g[k++] = c;
            c += step_;
        }
        state_ = 0;
        c = target_;
        rem = 0;
    }
    for (; k < n; ++k)
        g[k] = c;
    cur_ = c;
    rem_ = rem;
}

float SmoothFloat::steady() const noexcept
{
    if (nearlyEqual(cur_, 1.0f))
        return 1.0f;
    if (absf(cur_) < kEps)
        return 0.0f;
    return cur_;
}

void SmoothFloat::apply(float* l, float* r, int n) noexcept
{
    if (state_ == 0)
    {
        const float v = cur_;
        if (nearlyEqual(v, 1.0f))
            return;
        if (absf(v) < kEps)
        {
            zero(l, r, n);
            return;
        }
        vec::mul(l, v, n);
        vec::mul(r, v, n);
        return;
    }
    alignas(16) float g[kMaxBlock];
    block(g, n);
    vec::mulv(l, g, n);
    vec::mulv(r, g, n);
}

// ---------------------------------------------------------------------------------------------------- SignalGate

void SignalGate::prepare(double fs, int n) noexcept
{
    ramp_.prepareSamples(fs, n);
    ramp_.snap(0.0f);
}

void SignalGate::apply(float* l, float* r, int n) noexcept
{
    const float v = ramp_.ramping() ? ramp_.target() : ramp_.current();
    if (! nearlyEqual(v, 0.0f) && ! nearlyEqual(v, 1.0f))
    {
        zero(l, r, n);
        return;
    }
    if (ramp_.ramping())
    {
        alignas(16) float g[kMaxBlock];
        ramp_.block(g, n);
        vec::mulv(l, g, n);
        vec::mulv(r, g, n);
        return;
    }
    const float c = ramp_.current();
    if (nearlyEqual(c, 1.0f))
        return;
    if (absf(c) < kEps)
    {
        zero(l, r, n);
        return;
    }
    vec::mul(l, c, n);
    vec::mul(r, c, n);
}

// ---------------------------------------------------------------------------------------------------- MuteSmoother

void MuteSmoother::prepare(int n, bool muted) noexcept
{
    if (n < 1)
    {
        n_ = 1;
        inv_ = 1.0f;
    }
    else
    {
        n_ = n;
        inv_ = inverseOf(n);
    }
    muted_ = muted;
    rem_ = 0;
    if (! muted)
    {
        state_ = Open;
        g_ = 1.0f;
        step_ = -inv_;
    }
    else
    {
        state_ = Closed;
        g_ = 0.0f;
        step_ = inv_;
    }
}

void MuteSmoother::setMuted(bool m) noexcept
{
    if (muted_ == m)
        return;
    step_ = m ? -inv_ : inv_;
    muted_ = m;
    rem_ = n_ - rem_;
    state_ = Ramp;
}

void MuteSmoother::process(float* l, float* r, int n) noexcept
{
    if (state_ == Open)
        return;
    int i = 0;
    for (;;)
    {
        if (n == 0)
            return;
        if (state_ != Ramp)
        {
            if (state_ == Closed)
                zero(l + i, r + i, n);
            return;
        }
        const int rem = rem_;
        float g = g_;
        const float st = step_;
        if (n <= rem)
        {
            for (int k = 0; k < n; ++k)
            {
                l[i + k] *= g;
                r[i + k] *= g;
                g += st;
            }
            g_ = g;
            rem_ = rem - n;
            return;
        }
        if (rem > 0)
        {
            for (int k = 0; k < rem; ++k)
            {
                l[i + k] *= g;
                r[i + k] *= g;
                g += st;
            }
            i += rem;
        }
        if (! muted_)
            break;
        n -= rem;
        state_ = Closed;
        rem_ = 0;
        g_ = 0.0f;
    }
    state_ = Open;
    rem_ = 0;
    g_ = 1.0f;
}

void MuteSmoother::crossfade(float* l, float* r, const float* dl, const float* dr, int n) noexcept
{
    APP_FX_NOFUSE
    if (state_ == Open)
        return;
    int i = 0;
    for (;;)
    {
        if (n == 0)
            return;
        if (state_ != Ramp)
        {
            if (state_ == Closed)
            {
                vec::copy(l + i, dl + i, n);
                vec::copy(r + i, dr + i, n);
            }
            return;
        }
        const int rem = rem_;
        float g = g_;
        const float st = step_;
        const int m = n <= rem ? n : (rem > 0 ? rem : 0);
        for (int k = i; k < i + m; ++k)
        {
            const float a = 1.0f - g;
            const float pl = dl[k] * a;
            const float ql = g * l[k];
            const float pr = a * dr[k];
            const float qr = g * r[k];
            l[k] = pl + ql;
            r[k] = pr + qr;
            g += st;
        }
        if (n <= rem)
        {
            g_ = g;
            rem_ = rem - n;
            return;
        }
        i += m;
        if (! muted_)
            break;
        n -= rem;
        state_ = Closed;
        rem_ = 0;
        g_ = 0.0f;
    }
    state_ = Open;
    rem_ = 0;
    g_ = 1.0f;
}
} // namespace engine::fx::dsp
