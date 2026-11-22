// The 2-band EQ's shelf and peak filter family (spec 02-features/effects/parametric-eq.md FX-421…423,
// FX-427; research docs/effects/parametric-eq.md "Shelf and peak (IirFilter2, F040)", CC BY 4.0). Operation for operation
// equal to IirFilter2 of the MIT research model tools/fx/peq.py (pinned commit 8901c97): Zölzer's boost forms with 1/Q
// in place of √2, used unchanged for cuts, coefficients in double stored as float32; K = (tan(π·fc/fs)·(1/2π))·2π
// (the reference's extra steps matter for bit-exactness). configure() keeps fc / Q / gain only inside the limits
// (fc 1 Hz…Nyquist, Q 0.01…20, gain −80…+40 dB, V − 1 in −1…100); out-of-range or NaN values keep the previous one
// (FX-423). The filter itself is the shared DF-I biquad filt::Iir (filter_blocks.h) with its FX-427 re-seed.
#pragma once

#include "engine/fx/dsp/filter_blocks.h"

#include <cmath>

namespace engine::fx::dsp::eq2
{
struct Shelf
{
    bool peaking = false; // false: LowShelvingFilter, true: PeakingFilter
    double fs = 44100.0;
    float fcMin = 1.0f, fcMax = 22050.0f, fc = 11025.5f;
    double Q = 0.7071067811865476, p = 0.0, gdb = 0.0;
    filt::Iir iir{};

    void init(bool isPeaking, double sampleRate) noexcept
    {
        APP_FX_NOFUSE
        peaking = isPeaking;
        fs = sampleRate;
        const float nyq = static_cast<float>(static_cast<int>(sampleRate / 2.0));
        fcMin = 1.0f;
        fcMax = nyq;
        fc = (nyq + 1.0f) * 0.5f;
        Q = 1.0 / std::sqrt(2.0);
        p = 0.0;
        gdb = 0.0;
        iir = {};
        iir.c = coefs(fc, Q, p);
    }

    // slot 6 (0xe2ab4)
    void configure(float fcIn, double qIn, double gdbIn, bool flag) noexcept
    {
        APP_FX_NOFUSE
        if (! (fcIn < fcMin) && fcIn <= fcMax)
            fc = fcIn;
        if (! (qIn < 0.01) && qIn <= 20.0)
            Q = qIn;
        if (! (gdbIn < -80.0) && gdbIn <= 40.0)
            gdb = gdbIn;
        double pp = std::pow(10.0, gdbIn / 20.0) - 1.0;
        if (! (pp < -1.0) && pp <= 100.0)
            p = pp;
        else
            pp = p;
        iir.c = coefs(fc, Q, pp);
        iir.reseed = flag;
    }

    // slot 9 (0xe2540 low shelf / 0xe2440 peak)
    filt::Coefs coefs(float fc32, double q, double pm1) const noexcept
    {
        APP_FX_NOFUSE
        constexpr double kPi = 3.141592653589793;
        constexpr double kInvTwoPi = 0.15915494309189535;
        constexpr double kTwoPi = 6.283185307179586;
        const double fsf = static_cast<double>(static_cast<float>(fs));
        const double t = std::tan(static_cast<double>(fc32) * (kPi / fsf));
        const double K = (t * kInvTwoPi) * kTwoPi;
        const double iq = 1.0 / q;
        filt::Coefs c;
        if (! peaking)
        {
            const double V = pm1 + 1.0;
            const double sv = std::sqrt(V);
            const double K2 = K * K;
            const double K2x2 = K2 + K2;
            const double kq = K * iq;
            const double nrm = 1.0 / ((kq + 1.0) + K2);
            const double s = (sv * K) * iq;
            const double VK2 = K2 * V;
            const double b1n = K2x2 * V - 2.0;
            const double b0n = (s + 1.0) + VK2;
            const double b2n = VK2 + (1.0 - s);
            const double a1n = K2x2 - 2.0;
            const double a2n = (1.0 - kq) + K2;
            c.b0 = static_cast<float>(b0n * nrm);
            c.b1 = static_cast<float>(b1n * nrm);
            c.b2 = static_cast<float>(b2n * nrm);
            c.a1 = static_cast<float>(a1n * nrm);
            c.a2 = static_cast<float>(a2n * nrm);
            return c;
        }
        const double kq = K * iq;
        const double K2 = K * K;
        const double nrm = 1.0 / ((kq + 1.0) + K2);
        const double V = pm1 + 1.0;
        const double vk = (V * K) * iq;
        const double K2x2 = K2 + K2;
        const double a2n = (1.0 - kq) + K2;
        const double a1n = K2x2 - 2.0;
        const double b0n = (vk + 1.0) + K2;
        const double b2n = (1.0 - vk) + K2;
        c.b0 = static_cast<float>(b0n * nrm);
        c.a1 = static_cast<float>(a1n * nrm);
        c.b1 = c.a1;
        c.b2 = static_cast<float>(b2n * nrm);
        c.a2 = static_cast<float>(a2n * nrm);
        return c;
    }
};
} // namespace engine::fx::dsp::eq2
