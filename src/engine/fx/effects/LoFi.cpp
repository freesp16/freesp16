// LoFi — see LoFi.h. Every float32 step is one C++ float operation (never fused: APP_FX_NOFUSE on the host's clang,
// -ffp-contract=off on GCC hosts, no fused instruction on the A9); the double steps are double; libm only on
// parameter changes and glide steps (pow, tan, log10f), never per sample. Addresses in comments are the reference's
// (research docs/effects/lofi.md "1.60 code map").
#include "engine/fx/effects/LoFi.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Vec.h"

#include <cmath>
#include <cstring>
#include <limits>

#if APP_FX_HAVE_NEON
#include <arm_neon.h>
#endif

namespace engine::fx
{
namespace
{
using dsp::nearlyEqual;
using dsp::lofi::Biquad;
using dsp::lofi::FilterKind;

constexpr double kFltMin = 1.17549435e-38; // the model's FLT_MIN literal (LinearParameter change test)
constexpr int kBits[18] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 100};
constexpr int kGateLimitSteps = 38; // FX-924

// round(x), half to even, for |x| < 2^31 (the reference's double + 1.5·2^52 trick).
int rnd(double x) noexcept
{
    auto t = static_cast<long long>(x);
    const double fr = x - static_cast<double>(t);
    if (x >= 0.0)
    {
        if (fr > 0.5 || (fr == 0.5 && (t & 1) != 0))
            ++t;
    }
    else if (fr < -0.5 || (fr == -0.5 && (t & 1) != 0))
        --t;
    return static_cast<int>(t);
}

float log10f32(float x) noexcept // the reference's libm log10f (glibc 2.13 = fdlibm on the device)
{
    return std::log10(x);
}

float pow10f32(float e) noexcept // f32(pow(10, e)) in double (crmath.pow = glibc dbl-64)
{
    return static_cast<float>(std::pow(10.0, static_cast<double>(e)));
}

// The .init_array initialiser 0x137cc: step multipliers and the reciprocal logs of the step-count formula.
struct Multipliers
{
    float g0, g1, g2, g3, g4, invLogG1, invLogG3;
    Multipliers() noexcept
    {
        APP_FX_NOFUSE
        g0 = 17.0f;
        g3 = pow10f32(log10f32(50.0f) / 100.0f);
        g4 = 1.0f / g3;
        g1 = pow10f32(log10f32(1000.0f) / 250.0f);
        g2 = 1.0f / g1;
        invLogG1 = 1.0f / log10f32(g1);
        invLogG3 = 1.0f / log10f32(g3);
    }
};
const Multipliers kM; // static initialisation, read-only afterwards

int gateSamples(double fs) noexcept // rnd(f32(f32(f32(fs)·0.001)·3))
{
    return rnd(static_cast<double>((static_cast<float>(fs) * 0.001f) * 3.0f));
}
} // namespace

// ---------------------------------------------------------------------------------------------------- Gate

bool LoFi::Gate::fadingOut() const noexcept
{
    return ramp_.ramping() && nearlyEqual(ramp_.target(), 0.0f);
}

bool LoFi::Gate::settledClosed() const noexcept
{
    return ! ramp_.ramping() && nearlyEqual(ramp_.current(), 0.0f);
}

void LoFi::Gate::apply(float* l, float* r, int n) noexcept // 0xf521c → 0xf5a54
{
    const float v = ramp_.ramping() ? ramp_.target() : ramp_.current();
    if (! nearlyEqual(v, 0.0f) && ! nearlyEqual(v, 1.0f))
    {
        if (n > 0)
        {
            std::memset(l, 0, sizeof(float) * static_cast<std::size_t>(n));
            std::memset(r, 0, sizeof(float) * static_cast<std::size_t>(n));
        }
        return;
    }
    ramp_.apply(l, r, n); // the ramp, or the constant-gain shortcuts (1: nothing, |g| < ε: zero)
}

// ---------------------------------------------------------------------------------------------------- params

bool LoFi::FParam::changed() noexcept
{
    APP_FX_NOFUSE
    const float d = last - cur;
    const auto dd = static_cast<double>(d);
    if (dd <= kFltMin && -kFltMin <= dd)
        return false;
    last = cur;
    return true;
}

bool LoFi::CParam::changed() noexcept
{
    if (last == cur)
        return false;
    last = cur;
    return true;
}

// ---------------------------------------------------------------------------------------------------- filter container

void LoFi::Filter::init(int fs, double fsd) noexcept
{
    sub[0].init(FilterKind::HighPass, fs);
    sub[1].init(FilterKind::LowPass, fs);
    sub[2].init(FilterKind::BandPass, fs);
    sub[3].init(FilterKind::Off, fs);
    gate.prepare(fsd, gateSamples(fsd));
    gate.setNow(0.0f);
    pending = cur = 3;
    cutT = cutC = 1000.0f;
    cutMult = kM.g1;
    cutSteps = 0;
    qT = qC = 0.7f;
    qMult = kM.g3;
    qSteps = 0;
    next = 0.0f;
    maxSteps = 0;
    flag40 = dirty41 = false;
}

void LoFi::Filter::setType(int t) noexcept // inline in 0xfb20c (= 0xfbb50)
{
    if (t < 0 || t > 3)
        return;
    pending = t;
    if (t != cur)
        gate.close();
}

void LoFi::Filter::configure(Biquad& s) noexcept
{
    float hi = s.nyquist();
    if (cutC <= hi)
        hi = cutC;
    float fc = 5.0f;
    if (fc <= hi)
        fc = hi;
    s.setCoefs(fc, static_cast<double>(qC));
}

void LoFi::Filter::rampSetup() noexcept // 0xfb994
{
    APP_FX_NOFUSE
    if (dirty41)
    {
        const int n = rnd(static_cast<double>(dsp::absf(kM.invLogG1 * log10f32(cutT / cutC))));
        cutSteps = n + 1;
        cutMult = cutT > cutC ? kM.g1 : kM.g2;
        const int n2 = rnd(static_cast<double>(dsp::absf(kM.invLogG3 * log10f32(qT / qC))));
        qSteps = n2 + 1;
        qMult = qT > qC ? kM.g3 : kM.g4;
        next = 44.1f;
        dirty41 = false;
    }
    maxSteps = qSteps > cutSteps ? qSteps : cutSteps;
    if (gate.fadingOut())
        return;
    if (static_cast<float>(cutSteps) > static_cast<float>(kGateLimitSteps))
        gate.close();
}

Biquad& LoFi::Filter::swap() noexcept
{
    cur = pending;
    cutC = cutT;
    qC = qT;
    cutSteps = qSteps = maxSteps = 0;
    Biquad& s = sub[cur];
    configure(s);
    s.setPrime(true);
    return s;
}

void LoFi::Filter::snapNow() noexcept
{
    cur = pending;
    cutC = cutT;
    qC = qT;
    cutSteps = qSteps = maxSteps = 0;
    dirty41 = false;
    configure(sub[cur]);
    gate.setNow(1.0f); // a type change has just closed it
}

void LoFi::Filter::process(float* l, float* r, int n, bool scalar) noexcept // 0xfbb70
{
    APP_FX_NOFUSE
    if (cur != 3)
        rampSetup();
    if (gate.fadingOut())
    {
        const int rem = gate.remaining();
        if (n < rem)
        {
            sub[cur].process(l, r, n, scalar);
            gate.apply(l, r, n);
            return;
        }
        sub[cur].process(l, r, rem, scalar);
        gate.apply(l, r, rem);
        Biquad& s = swap();
        s.process(l + rem, r + rem, n - rem, scalar);
        gate.open();
        gate.apply(l + rem, r + rem, n - rem);
        return;
    }
    if (gate.settledClosed())
    {
        Biquad& s = swap();
        s.process(l, r, n, scalar);
        gate.open();
        gate.apply(l, r, n);
        return;
    }
    int rem = n, pos = 0; // open or fading in: the stepped cutoff / Q ramp
    while (rem > 0)
    {
        int chunk;
        if (maxSteps <= 0)
        {
            chunk = rem;
            rem = 0;
        }
        else
        {
            const float s15 = next;
            const int r6 = rnd(static_cast<double>(s15));
            if (rem >= r6)
            {
                const auto s14 = static_cast<float>(r6);
                rem -= r6;
                const int ocs = cutSteps;
                cutSteps = ocs > 1 ? ocs - 1 : 0;
                const int oqs = qSteps;
                next = (s15 - s14) + 44.1f;
                cutC = ocs > 1 ? cutC * cutMult : cutT;
                qSteps = oqs > 1 ? oqs - 1 : 0;
                flag40 = true;
                qC = oqs > 1 ? qC * qMult : qT;
                chunk = r6;
            }
            else
            {
                chunk = rem;
                next = s15 - static_cast<float>(rem);
                rem = 0;
            }
        }
        Biquad& s = sub[cur];
        s.process(l + pos, r + pos, chunk, scalar);
        pos += chunk;
        if (flag40)
        {
            configure(s);
            s.setPrime(false);
            flag40 = false;
        }
    }
    gate.apply(l, r, n);
}

// ---------------------------------------------------------------------------------------------------- LoFi

void LoFi::init() noexcept // constructor 0xfd640 (fs, block)
{
    APP_FX_NOFUSE
    for (auto& p : p_)
        p = FParam{};
    type_ = CParam{0, 0};
    pos_ = CParam{2, 2};
    preN_ = rnd(static_cast<double>(static_cast<float>(fs_) * 0.001f));
    preT_ = preC_ = 1.0f;
    preInc_ = 0.0f;
    preCnt_ = 0;
    bits_ = 32;
    scale_ = 256.0f;
    inv_ = 1.0f / 256.0f;
    shPhase_ = 1.0f;
    shStep_ = 1.0f;
    shHold_[0] = shHold_[1] = 0.0f;
    filt_.init(fsInt_, fs_);
    posGate_.prepare(fs_, gateSamples(fs_));
    posGate_.setNow(0.0f);
    posPending_ = posCur_ = 1;
    pristine_ = true;
    typeSet_ = posSet_ = false;
    applyStartUpProfile();
}

void LoFi::applyStartUpProfile() noexcept
{
    // FX-980: the reference builds both gates closed; Sixteen builds them open.
    const float g = (quirks_ & kLoFiQuirkClosedGates) != 0 ? 0.0f : 1.0f;
    filt_.gate.setNow(g);
    posGate_.setNow(g);
}

void LoFi::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    fsInt_ = static_cast<int>(sampleRate);
    init();
}

void LoFi::setQuirkProfile(std::uint32_t mask) noexcept
{
    quirks_ = mask;
    if (pristine_)
        applyStartUpProfile();
}

void LoFi::setParam(int index, float value01) noexcept // slot 14 → LinearParameter::set
{
    if (index >= 0 && index < 6)
        p_[index].cur = value01;
}

void LoFi::setOption(int which, int value) noexcept
{
    if (which == kOptFilterType)
    {
        type_.cur = value;
        typeSet_ = true;
    }
    else if (which == kOptPosition)
    {
        pos_.cur = value;
        posSet_ = true;
    }
    else if (which == kClearState && value != 0)
        init();
}

void LoFi::applyParams() noexcept // 0xfb20c (order: PREAMP, BITDEPTH, REDUCTION, CUTOFF, RESONANCE, TYPE, POSITION)
{
    APP_FX_NOFUSE
    const bool sixteenStart = pristine_ && (quirks_ & kLoFiQuirkChangeTrap) == 0;
    if (sixteenStart) // Sixteen: no change-detection trap before the first block (F026)
    {
        for (auto& p : p_)
            p.last = std::numeric_limits<float>::quiet_NaN();
        type_.last = pos_.last = -1;
        if (! typeSet_)
            type_.cur = kFilterOff;
        if (! posSet_)
            pos_.cur = 1;
    }
    if (p_[0].changed())
    {
        const float v = p_[0].cur;
        const float e = (v * 42.0f) * 0.05f;
        const float t = pow10f32(e);
        const float inv = 1.0f / static_cast<float>(preN_);
        preT_ = t;
        preCnt_ = preN_;
        preInc_ = (t - preC_) * inv;
    }
    if (p_[2].changed())
    {
        int i = rnd(static_cast<double>(p_[2].cur * kM.g0));
        i = i < 0 ? 0 : (i > 17 ? 17 : i); // FX-982
        bits_ = kBits[i];
        if (bits_ < 31)
        {
            scale_ = static_cast<float>(1 << bits_);
            inv_ = 1.0f / scale_;
        }
        else
        {
            scale_ = 0.0f; // crusher off (bits >= 32 skips it)
            inv_ = 0.0f;
        }
    }
    if (p_[1].changed())
    {
        const float v = p_[1].cur;
        const auto r = static_cast<float>(std::pow(1000.0000610351562, static_cast<double>(v)) * 44.099998474121094);
        const int rate = rnd(static_cast<double>(r));
        shStep_ = (rate > fsInt_ || rate <= 0) ? 1.0f : static_cast<float>(fsInt_) / static_cast<float>(rate);
    }
    if (p_[3].changed())
    {
        filt_.cutT = static_cast<float>(std::pow(1000.0, static_cast<double>(p_[3].cur)) * 20.0);
        filt_.dirty41 = true;
    }
    if (p_[4].changed())
    {
        const float q = 0.2f + p_[4].cur * 9.8f;
        if (! nearlyEqual(filt_.qT, q))
        {
            filt_.qT = q;
            filt_.dirty41 = true;
        }
    }
    if (type_.changed())
        filt_.setType(type_.cur);
    if (pos_.changed())
    {
        posPending_ = pos_.cur;
        if (filt_.cur == 3)
            posCur_ = posPending_;
        else
            posGate_.close();
    }
    if (pristine_ && (quirks_ & kLoFiQuirkClosedGates) == 0)
    {
        // Sixteen (FX-980 fix): before the first block the first parameter set applies at once — no type fade, no
        // glide from the constructor's 1 kHz, no position fade.
        filt_.snapNow();
        posCur_ = posPending_;
        posGate_.setNow(1.0f);
        preC_ = preT_; // no 44-sample preamp ramp from the constructor's 0 dB either
        preCnt_ = 0;
    }
}

void LoFi::reset() noexcept // slot 27 0xfb5a4 (FX-960)
{
    applyParams();
    filt_.sub[filt_.cur].clearTaps();
    shHold_[0] = shHold_[1] = 0.0f;
    shPhase_ = (quirks_ & kLoFiQuirkHoldPhase) != 0 ? shStep_ : 0.0f; // FX-981 (Sixteen: phase 0)
}

void LoFi::preamp(float* l, float* r, int n) noexcept // 0xfb6e4
{
    APP_FX_NOFUSE
    float g = preC_;
    int cnt = preCnt_;
    int j = 0;
    for (; j < n && (cnt > 0 || g != preT_); ++j) // the ramp (and its last, accumulated sample)
    {
        const float hi = 1.0f / g, lo = -hi;
        const float s = (1.0f + g * 0.2f) * 0.8333333f;
        float x = l[j];
        x = x < lo ? lo : (x > hi ? hi : x);
        l[j] = s * x;
        x = r[j];
        x = x < lo ? lo : (x > hi ? hi : x);
        r[j] = s * x;
        if (cnt > 0)
        {
            --cnt;
            g = g + preInc_;
        }
        else
            g = preT_;
    }
    preC_ = g;
    preCnt_ = cnt;
    if (j >= n)
        return;
    const float hi = 1.0f / g, lo = -hi; // steady: one clip level and scale for the rest of the block
    const float s = (1.0f + g * 0.2f) * 0.8333333f;
    float* ch[2] = {l + j, r + j};
    const int m = n - j;
#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        const float32x4_t vlo = vdupq_n_f32(lo), vhi = vdupq_n_f32(hi);
        for (float* x : ch)
        {
            int i = 0;
            for (; i + 4 <= m; i += 4)
            {
                float32x4_t v = vld1q_f32(x + i);
                v = vminq_f32(vmaxq_f32(v, vlo), vhi);
                vst1q_f32(x + i, vmulq_n_f32(v, s));
            }
            for (; i < m; ++i)
            {
                const float v = x[i];
                x[i] = s * (v < lo ? lo : (v > hi ? hi : v));
            }
        }
        return;
    }
#endif
    for (float* x : ch)
        for (int i = 0; i < m; ++i)
        {
            const float v = x[i];
            x[i] = s * (v < lo ? lo : (v > hi ? hi : v));
        }
}

void LoFi::crush(float* l, float* r, int n) noexcept // 0xfaf28: s·(trunc(2^b·√|x|)·2^−b)²
{
    APP_FX_NOFUSE
    const float sc = scale_, inv = inv_;
    float* ch[2] = {l, r};
#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        alignas(16) float a[dsp::kMaxBlock];
        const float32x4_t one = vdupq_n_f32(1.0f), mone = vdupq_n_f32(-1.0f), zero = vdupq_n_f32(0.0f);
        for (float* x : ch)
        {
            int i = 0;
            for (; i + 4 <= n; i += 4)
                vst1q_f32(a + i, vabsq_f32(vld1q_f32(x + i)));
            for (; i < n; ++i)
                a[i] = dsp::absf(x[i]);
            for (i = 0; i < n; ++i) // VFP vsqrt.f32: exact (ARMv7 NEON has no square root)
                a[i] = std::sqrt(a[i]);
            for (i = 0; i + 4 <= n; i += 4)
            {
                const float32x4_t v = vld1q_f32(x + i);
                const float32x4_t t = vcvtq_f32_s32(vcvtq_s32_f32(vmulq_n_f32(vld1q_f32(a + i), sc)));
                const float32x4_t q = vmulq_n_f32(t, inv);
                const float32x4_t sg = vbslq_f32(vcgtq_f32(v, zero), one, mone);
                vst1q_f32(x + i, vmulq_f32(sg, vmulq_f32(q, q)));
            }
            for (; i < n; ++i)
            {
                const float sg = x[i] > 0.0f ? 1.0f : -1.0f;
                const float q = static_cast<float>(static_cast<std::int32_t>(sc * a[i])) * inv;
                x[i] = sg * (q * q);
            }
        }
        return;
    }
#endif
    for (float* x : ch)
        for (int i = 0; i < n; ++i)
        {
            const float v = x[i];
            const float sg = v > 0.0f ? 1.0f : -1.0f;
            const float a = std::sqrt(v * sg);
            const float q = static_cast<float>(static_cast<std::int32_t>(sc * a)) * inv;
            x[i] = sg * (q * q);
        }
}

void LoFi::sampleHold(float* l, float* r, int n) noexcept // inline in 0xfbf64
{
    APP_FX_NOFUSE
    float ph = shPhase_;
    const float st = shStep_;
    float hl = shHold_[0], hr = shHold_[1];
    for (int j = 0; j < n; ++j)
    {
        if (ph < 1.0f)
        {
            ph = ph + st;
            hl = l[j];
            hr = r[j];
        }
        else
        {
            l[j] = hl;
            r[j] = hr;
        }
        ph = ph - 1.0f;
    }
    shPhase_ = ph;
    shHold_[0] = hl;
    shHold_[1] = hr;
}

void LoFi::chain(float* l, float* r, int n) noexcept // 0xfbf64
{
    posGate_.apply(l, r, n);
    if (posCur_ == 0)
    {
        filt_.process(l, r, n, scalar_);
        preamp(l, r, n);
        if (bits_ < 32)
            crush(l, r, n);
        sampleHold(l, r, n);
    }
    else
    {
        preamp(l, r, n);
        if (bits_ < 32)
            crush(l, r, n);
        sampleHold(l, r, n);
        filt_.process(l, r, n, scalar_);
    }
}

void LoFi::process(float* left, float* right, const ProcessContext&) noexcept // slot 25 0xfc0f4
{
    constexpr int n = kBlockFrames;
    applyParams();
    pristine_ = false;
    bool done = false;
    if (posGate_.ramping())
    {
        if (posGate_.fadingOut() && posGate_.remaining() <= n)
        {
            const int rem = posGate_.remaining();
            chain(left, right, rem);
            posCur_ = posPending_;
            posGate_.open();
            chain(left + rem, right + rem, n - rem);
            done = true;
        }
    }
    else if (nearlyEqual(posGate_.current(), 0.0f))
    {
        posCur_ = posPending_;
        posGate_.open();
    }
    if (! done)
        chain(left, right, n);
}
} // namespace engine::fx

namespace
{
constexpr std::string_view kLoFiOptions[] = {"fx.lofi.type1", "fx.lofi.type2"};
} // namespace

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "lofi",
    .name = "LO-FI",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.lofi.preamp", "fx.lofi.reduction", "fx.lofi.bits", "fx.lofi.cutoff", "fx.lofi.resonance",
               "fx.lofi.mix"},
    .options = kLoFiOptions,
    .visual = "none",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5),
              .wetHighPass = true,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::LoFi>,
    .featureFlag = {}}))
