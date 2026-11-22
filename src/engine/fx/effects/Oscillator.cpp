// ModOscillator — see Oscillator.h. Operation for operation as tools/fx/osc.py (MIT): float32 steps are C++ float
// expressions (one rounding per operation, never fused: APP_FX_NOFUSE, -ffp-contract=off on the host), double steps
// are double expressions; the libm calls are std:: in double (correctly rounded in practice, as the reference's glibc
// 2.13 dbl-64 sin/cos/acos; sinf/cosf/expf are evaluated in double and rounded once — the reference's fdlibm sinf/cosf
// and glibc expf differ by 1 ulp in rare arguments, research oscillator.md "Math library").
#include "engine/fx/effects/Oscillator.h"

#include "engine/fx/dsp/Numerics.h"
#include "engine/fx/effects/OscillatorRandomTable.h"

#include <cmath>

namespace engine::fx
{
namespace
{
constexpr double kTwoPi = 6.283185307179586;  // double literal 0x401921fb54442d18
constexpr float kTwoPiF = 6.2831855f;         // 0x40c90fdb
constexpr float kNegTwoPiF = -6.2831855f;     // 0xc0c90fdb (setSmoothingHz)
constexpr float kSmoothInitK = -314.15927f;   // 0xc39d1463 = −2π·50 (init: 50 Hz smoother)
constexpr float kPeriodFloor = 9999998.0f;    // setFrequency period for f <= kMinFrequency
constexpr std::uint32_t kTableMask = 0x1fffu; // 8192 entries; index 0x2000 → table[0]

float expf32(float x) noexcept
{
    return static_cast<float>(std::exp(static_cast<double>(x)));
}

// Wrap into [0, 1) by whole cycles in float32 (osc.py _wrap01); a non-finite or absurd value becomes 0 (FX-1308).
float wrap01(float ph) noexcept
{
    APP_FX_NOFUSE
    if (! (dsp::absf(ph) < 65536.0f))
        return 0.0f;
    while (ph < 0.0f)
        ph = ph + 1.0f;
    while (ph >= 1.0f)
        ph = ph - 1.0f;
    return ph;
}

float fdiv(float a, float b) noexcept
{
    // VFP vdiv.f32 semantics; a zero old period (never initialised) keeps the counter at 0 instead of a NaN.
    return b == 0.0f ? 0.0f : a / b;
}
} // namespace

void ModOscillator::clear() noexcept
{
    table_ = kOscRandomTable.data();
    amp_ = 1.0f;
    off_ = 0.0f;
    wave_ = -1;
    freq_ = 0.0f;
    invFs_ = slope_ = d28_ = d30_ = d38_ = d40_ = d48_ = 0.0;
    ph_ = ph2_ = 0.0f;
    a_ = b_ = 0.0f;
    sm_[0] = sm_[1] = 0.0f;
    rsCnt_ = rsPer_ = 0;
    hold_ = smoothOff_ = false;
    c_ = P_ = fsf_ = 0.0f;
    rOff_ = rIdx_ = 0;
}

void ModOscillator::setTable(const float* table) noexcept
{
    table_ = table != nullptr ? table : kOscRandomTable.data();
}

bool ModOscillator::init(int wave, float freq, double fsIn, std::uint32_t resyncSeed) noexcept
{
    APP_FX_NOFUSE
    const auto fs = static_cast<std::int64_t>(fsIn);
    std::uint32_t x = resyncSeed * 448u;
    if (rOff_ > 0x2000u)
        rOff_ = 0;
    const auto fsu = static_cast<std::uint32_t>(fs);
    rsCnt_ = x;
    wave_ = -1;
    off_ = 0.0f;
    freq_ = 0.0f;
    invFs_ = slope_ = d28_ = d30_ = d38_ = d40_ = d48_ = 0.0;
    amp_ = 1.0f;
    ph_ = ph2_ = 0.0f;
    rsPer_ = fsu;
    rIdx_ = 0;
    if (fsu != 0 && fsu < x) // the reference's subtract loop: x ends in (0, fs]
        rsCnt_ = (x - 1u) % fsu + 1u;
    hold_ = false;
    if (fs <= 0)
        return false;
    invFs_ = 1.0 / static_cast<double>(fs);
    fsf_ = static_cast<float>(fs);
    const float e = expf32(static_cast<float>(invFs_) * kSmoothInitK);
    smoothOff_ = false;
    sm_[0] = sm_[1] = 0.0f;
    b_ = e;
    a_ = 1.0f - e;
    if (freq == 0.0f)
        return false;
    const auto fsf = static_cast<float>(fs);
    if (freq <= kMinFrequency)
        freq = kMinFrequency;
    float P = fsf / freq - 1.0f;
    if (P < 1.0f)
        P = 1.0f;
    P_ = P;
    if (wave < 0 || wave > 6)
        return false;
    switch (wave)
    {
    case kSine:
    {
        const double w = (static_cast<double>(dsp::absf(freq)) * kTwoPi) * invFs_;
        slope_ = w;
        d28_ = std::cos(w);
        ph_ = 1.0f;
        ph2_ = 0.0f;
        d38_ = std::sin(w);
        d48_ = 0.0;
        d40_ = 1.0;
        d30_ = d28_ + d28_;
        break;
    }
    case kTriangle:
        ph_ = 0.0f;
        d40_ = 1.0;
        slope_ = (invFs_ * 4.0) * static_cast<double>(freq);
        c_ = P * 0.25f;
        break;
    case kSaw:
        c_ = 0.0f;
        ph_ = 0.0f;
        d40_ = 2.0;
        slope_ = (invFs_ * -2.0) * static_cast<double>(freq);
        break;
    case kPulse50:
    case kPulse25:
    case kPulse12:
        d28_ = wave == kPulse50 ? 0.5 : (wave == kPulse25 ? 0.25 : 0.125);
        slope_ = invFs_ * static_cast<double>(freq);
        c_ = 0.0f;
        d40_ = 0.0;
        ph_ = 0.0f;
        break;
    default: // kRandom
    {
        d40_ = 0.0;
        const float fa = dsp::absf(freq);
        c_ = 0.0f;
        std::uint32_t idx = rIdx_ + rOff_;
        if (idx >= 0x2000u)
            idx -= 0x2000u;
        rIdx_ = rIdx_ < 0x2000u ? rIdx_ + 1 : 0;
        const float v = table_[idx & kTableMask];
        ph2_ = v;
        ph_ = 0.0f;
        slope_ = invFs_ * static_cast<double>(fa);
        d48_ = static_cast<double>(v);
        break;
    }
    }
    freq_ = freq;
    wave_ = wave;
    return true;
}

bool ModOscillator::setFrequency(float f) noexcept
{
    APP_FX_NOFUSE
    if (f == 0.0f)
    {
        hold_ = true;
        return true;
    }
    const float Pold = P_;
    float Pn;
    if (f <= kMinFrequency)
    {
        Pn = kPeriodFloor;
        f = kMinFrequency;
        P_ = Pn;
    }
    else
    {
        const float fsP = quirk44k_ ? 44100.0f : fsf_; // FX-1380: the reference's literal 44100
        Pn = fsP / f - 1.0f;
        P_ = Pn;
        if (Pn < 1.0f)
        {
            Pn = 1.0f;
            P_ = Pn;
        }
    }
    c_ = fdiv(Pn * c_, Pold);
    switch (wave_)
    {
    case kSine:
    {
        const double w = (static_cast<double>(dsp::absf(f)) * kTwoPi) * invFs_;
        const double y1 = d40_, sOld = d38_, cwOld = d28_, y2 = d48_;
        const double d10 = y1 * sOld;
        const double d11 = y2 - y1 * cwOld;
        const double s = std::sin(w), c = std::cos(w);
        slope_ = w;
        d28_ = c;
        d38_ = s;
        const double d18 = (d10 * c) / s;
        d40_ = d10 / s;
        d30_ = c + c;
        d48_ = d18 + d11;
        break;
    }
    case kTriangle:
        slope_ = (invFs_ * 4.0) * static_cast<double>(f);
        break;
    case kSaw:
        slope_ = (invFs_ * -2.0) * static_cast<double>(f);
        break;
    case kPulse50:
        d28_ = 0.5;
        slope_ = invFs_ * static_cast<double>(f);
        break;
    case kPulse25:
    case kPulse12:
        d28_ = wave_ == kPulse25 ? 0.25 : 0.125;
        slope_ = invFs_ * static_cast<double>(f);
        if (f < 0.0f)
            d28_ = wave_ == kPulse25 ? 0.75 : 0.875;
        break;
    case kRandom:
        slope_ = invFs_ * static_cast<double>(dsp::absf(f));
        break;
    default:
        return false;
    }
    freq_ = f;
    return true;
}

bool ModOscillator::setPeriodMs(float ms) noexcept
{
    if (ms == 0.0f)
        return false;
    return setFrequency(1000.0f / ms);
}

bool ModOscillator::setPhaseOffset(float phi) noexcept
{
    APP_FX_NOFUSE
    if (freq_ == 0.0f)
        return false;
    phi = wrap01(phi);
    switch (wave_)
    {
    case kSine:
    {
        const float a = phi * kTwoPiF;
        ph_ = static_cast<float>(std::cos(static_cast<double>(a)));
        ph2_ = static_cast<float>(std::sin(static_cast<double>(a)));
        break;
    }
    case kTriangle:
        ph_ = phi * 4.0f;
        break;
    case kSaw:
        ph_ = phi + phi;
        break;
    case kPulse50:
    case kPulse25:
    case kPulse12:
    case kRandom:
        ph_ = phi;
        break;
    default:
        return false;
    }
    return true;
}

bool ModOscillator::resetPhase(float phi) noexcept
{
    APP_FX_NOFUSE
    if (freq_ == 0.0f)
        return false;
    phi = wrap01(phi);
    switch (wave_)
    {
    case kSine:
    {
        const double d8 = static_cast<double>(phi) * kTwoPi;
        const double s = d38_;
        d40_ = std::cos(d8) / s;
        d48_ = std::cos(d8 - slope_) / s;
        c_ = P_ * phi;
        break;
    }
    case kTriangle:
    {
        const double d16 = 1.0 + static_cast<double>(phi) * 4.0;
        d40_ = d16;
        const float c = (phi + 0.25f) * P_;
        c_ = c;
        if (d16 >= 4.0)
            d40_ = d16 - 4.0;
        if (P_ < c)
            c_ = c - P_;
        break;
    }
    case kSaw:
        c_ = P_ * phi;
        d40_ = static_cast<double>(phi) + static_cast<double>(phi);
        break;
    case kPulse50:
    case kPulse25:
    case kPulse12:
    case kRandom:
        c_ = P_ * phi;
        d40_ = static_cast<double>(phi);
        break;
    default:
        return false;
    }
    return true;
}

bool ModOscillator::setSmoothingHz(float hz) noexcept
{
    APP_FX_NOFUSE
    if (! (invFs_ > 0.0))
        return false;
    if (! (hz > 0.0f))
    {
        b_ = 0.0f;
        a_ = 1.0f;
        return true;
    }
    const float e = expf32((hz * kNegTwoPiF) * static_cast<float>(invFs_));
    b_ = e;
    a_ = 1.0f - e;
    return true;
}

void ModOscillator::resync() noexcept // 0x101b30
{
    if (wave_ == kSine)
    {
        const double y1 = d40_, s = d38_, y2 = d48_;
        double v = y1 * s;
        const double dr = (y1 <= y2 || y1 != y1 || y2 != y2) ? 1.0 : -1.0;
        if (v > 1.0)
            v = 1.0;
        else if (v < -1.0)
            v = -1.0;
        d40_ = v / s;
        const double a = std::acos(v);
        d48_ = std::cos(a * dr - slope_) / s;
    }
    rsCnt_ = 0;
}

float ModOscillator::fetch() noexcept
{
    std::uint32_t idx = rIdx_ + rOff_;
    if (idx > 0x1fffu)
        idx -= 0x2000u;
    rIdx_ = rIdx_ > 0x1fffu ? 0 : rIdx_ + 1;
    return table_[idx & kTableMask];
}

void ModOscillator::holdValues(float& lc, float& rc) const noexcept
{
    APP_FX_NOFUSE
    const float ph = ph_;
    switch (wave_)
    {
    case kSine:
    {
        lc = static_cast<float>(d40_ * d38_);
        const double t = d48_ - d40_ * d28_;
        rc = lc * ph - static_cast<float>(t) * ph2_;
        return;
    }
    case kTriangle:
    {
        const float f = static_cast<float>(d40_);
        lc = d40_ > 2.0 ? 3.0f - f : f - 1.0f;
        float t = f + ph;
        if (t >= 4.0f)
            t = t - 4.0f;
        rc = t > 2.0f ? 3.0f - t : t - 1.0f;
        return;
    }
    case kSaw:
    {
        const float f = static_cast<float>(d40_);
        float t = f + ph;
        lc = f - 1.0f;
        if (t >= 2.0f)
            t = t - 2.0f;
        rc = t - 1.0f;
        return;
    }
    case kPulse50:
    case kPulse25:
    case kPulse12:
    {
        const float f = static_cast<float>(d40_);
        float t = f + ph;
        lc = d40_ <= d28_ ? 1.0f : -1.0f;
        if (t >= 1.0f)
            t = t - 1.0f;
        rc = d28_ < static_cast<double>(t) ? -1.0f : 1.0f;
        return;
    }
    default:
        lc = static_cast<float>(d48_);
        rc = ph2_;
        return;
    }
}

void ModOscillator::runSine(float* l, float* r, int n) noexcept // 0x1021c0
{
    APP_FX_NOFUSE
    const double s = d38_;
    const float ph = ph_, ph2 = ph2_;
    const double inv = s != 0.0 ? 1.0 / s : 1.0;
    if (n > 0)
    {
        const double k2 = d30_, cw = d28_, ninv = -inv;
        double y1 = d40_, y2 = d48_;
        for (int i = 0; i < n; ++i)
        {
            const double y = k2 * y1 - y2;
            float x = static_cast<float>(s * y);
            double prev, cur;
            if (x > 1.0f)
            {
                x = 1.0f;
                prev = cur = inv;
            }
            else if (x < -1.0f)
            {
                x = -1.0f;
                prev = cur = ninv;
            }
            else
            {
                prev = y1;
                cur = y;
            }
            l[i] = x;
            const double q = prev - cur * cw;
            r[i] = ph * x - static_cast<float>(q) * ph2;
            y1 = cur;
            y2 = prev;
        }
        d40_ = y1;
        d48_ = y2;
    }
    float c = c_ + static_cast<float>(n);
    const float P = P_;
    if (! (P > c))
    {
        c = c - P;
        while (P <= c)
            c = c - P;
    }
    c_ = c;
}

void ModOscillator::runTriangle(float* l, float* r, int n) noexcept // 0x101e14 / 0x10252c
{
    APP_FX_NOFUSE
    const float P = P_, q = P * 0.25f, k = static_cast<float>(slope_), offR = q * ph_, half = P * 0.5f;
    const bool pos = ! (k < 0.0f);
    auto tri = [&](float t) noexcept
    {
        const float ck = t * k;
        if (pos)
            return t > half ? 3.0f - ck : ck - 1.0f;
        return (t > half ? 3.0f : 1.0f) + ck;
    };
    float c = c_;
    for (int i = 0; i < n; ++i)
    {
        c = c + 1.0f;
        if (c >= P)
            c = c - P;
        l[i] = tri(c);
        float cr = c + offR;
        if (cr >= P)
            cr = cr - P;
        r[i] = tri(cr);
    }
    const double ck = static_cast<double>(c) * static_cast<double>(k);
    d40_ = pos ? ck : -ck;
    c_ = c;
}

void ModOscillator::runSaw(float* l, float* r, int n) noexcept // 0x101f08 / 0x1024bc
{
    APP_FX_NOFUSE
    const float P = P_, offR = (P * ph_) * 0.5f, k = static_cast<float>(slope_);
    const bool pos = ! (k < 0.0f);
    float c = c_;
    for (int i = 0; i < n; ++i)
    {
        c = c + 1.0f;
        if (c >= P)
            c = c - P;
        float cr = c + offR;
        if (cr >= P)
            cr = cr - P;
        if (pos)
        {
            l[i] = c * k - 1.0f;
            r[i] = cr * k - 1.0f;
        }
        else
        {
            l[i] = 1.0f + c * k;
            r[i] = 1.0f + cr * k;
        }
    }
    const double ck = static_cast<double>(c) * static_cast<double>(k);
    d40_ = pos ? ck : -ck;
    c_ = c;
}

void ModOscillator::runPulse(float* l, float* r, int n) noexcept // 0x101fa0 / 0x102430
{
    APP_FX_NOFUSE
    const float P = P_, offR = P * ph_, k = static_cast<float>(slope_), thr = static_cast<float>(d28_) * P;
    const bool pos = ! (k < 0.0f);
    float c = c_;
    for (int i = 0; i < n; ++i)
    {
        c = c + 1.0f;
        if (c >= P)
            c = c - P;
        float cr = c + offR;
        if (pos)
            l[i] = ! (c > thr) ? 1.0f : -1.0f;
        else
            l[i] = c < thr ? -1.0f : 1.0f;
        if (cr >= P)
            cr = cr - P;
        r[i] = ! (cr > thr) ? 1.0f : -1.0f;
    }
    const double ck = static_cast<double>(c) * static_cast<double>(k);
    d40_ = pos ? ck : -ck;
    c_ = c;
}

void ModOscillator::runRandom(float* l, float* r, int n) noexcept // 0x102058
{
    APP_FX_NOFUSE
    const float P = P_, ph = ph_;
    float cL = c_;
    float cR = cL + ph * P;
    if (cR >= P)
        cR = cR - P;
    const float k = static_cast<float>(slope_);
    float vL = static_cast<float>(d48_), vR = ph2_;
    if (n > 0)
    {
        if (! (ph >= 0.5f) && ph == ph) // R leads (φ < 0.5)
        {
            float r8 = vR;
            for (int i = 0; i < n; ++i)
            {
                cR = cR + 1.0f;
                if (! (P > cR))
                {
                    r8 = vR;
                    for (;;)
                    {
                        cR = cR - P;
                        vR = fetch();
                        if (P > cR)
                            break;
                        r8 = vR;
                    }
                }
                cL = cL + 1.0f;
                r[i] = vR;
                if (! (P > cL))
                {
                    do
                        cL = cL - P;
                    while (! (P > cL));
                    vL = r8;
                }
                l[i] = vL;
            }
        }
        else // L leads
        {
            float sl = vL;
            for (int i = 0; i < n; ++i)
            {
                cL = cL + 1.0f;
                if (! (P > cL))
                {
                    sl = vL;
                    for (;;)
                    {
                        cL = cL - P;
                        vL = fetch();
                        if (P > cL)
                            break;
                        sl = vL;
                    }
                }
                cR = cR + 1.0f;
                l[i] = vL;
                if (! (P > cR))
                {
                    do
                        cR = cR - P;
                    while (! (P > cR));
                    vR = sl;
                }
                r[i] = vR;
            }
        }
    }
    ph2_ = vR;
    c_ = cL;
    d40_ = static_cast<double>(cL) * static_cast<double>(k);
    d48_ = static_cast<double>(vL);
}

void ModOscillator::process(float* l, float* r, int n) noexcept // 0x101bdc
{
    APP_FX_NOFUSE
    if (n <= 0)
        return;
    if (! valid())
    {
        for (int i = 0; i < n; ++i) // FX-1308 (our choice): silent at the offset (the reference leaves the buffers)
            l[i] = r[i] = off_;
        return;
    }
    if (hold_)
    {
        float lc, rc;
        holdValues(lc, rc);
        for (int i = 0; i < n; ++i)
        {
            l[i] = lc;
            r[i] = rc;
        }
    }
    else
    {
        switch (wave_)
        {
        case kSine:
            runSine(l, r, n);
            break;
        case kTriangle:
            runTriangle(l, r, n);
            break;
        case kSaw:
            runSaw(l, r, n);
            break;
        case kRandom:
            runRandom(l, r, n);
            break;
        default:
            runPulse(l, r, n);
            break;
        }
        rsCnt_ += static_cast<std::uint32_t>(n);
        if (rsCnt_ > rsPer_)
            resync();
    }
    const float amp = amp_, off = off_;
    if (! smoothOff_) // out = off + amp·x, then the smoother: one pass, the L and R chains interleaved
    {
        const float a = a_, b = b_;
        float y0 = sm_[0], y1 = sm_[1];
        for (int i = 0; i < n; ++i)
        {
            const float xl = off + amp * l[i], xr = off + amp * r[i];
            y0 = a * xl + b * y0;
            y1 = a * xr + b * y1;
            l[i] = y0;
            r[i] = y1;
        }
        sm_[0] = y0;
        sm_[1] = y1;
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = off + amp * l[i];
            r[i] = off + amp * r[i];
        }
        sm_[0] = sm_[1] = 0.0f;
    }
}
} // namespace engine::fx
