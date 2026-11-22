// Compressor — see Compressor.h. Line for line the MIT model tools/fx/compressor.py (research pinned commit 8901c97);
// F(x) there = a float32 rounding here. Float + − × ÷ of two floats are computed in float (equal to the model's double
// operation rounded once); the model's mixed double steps are written out as double.
#include "engine/fx/effects/Compressor.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/dsp/Vec.h"
#include "engine/fx/dsp/compressor_kernels.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::fx::effects
{
namespace
{
namespace ck = dsp::compressor;

constexpr float kMilli = 0.001f;
constexpr float kTenth = 0.1f;
constexpr float k005 = 0.05f;
constexpr float kM005 = -0.05f;
constexpr float kHyst = 1.4125376f; // 0x3fb4ce08, +3 dB
constexpr float kMeterDecay = 0.89125091f; // 0x3f642905, −1 dB per block
constexpr double kSqrt2 = static_cast<double>(1.4142135f); // the double of the float literal (0x3ff6a09e60000000)

int rintEven(float x) noexcept // (double)x + 1.5·2^52: round half to even
{
    return static_cast<int>(std::nearbyint(static_cast<double>(x)));
}
int truncS32(float x) noexcept // vcvt.s32.f32
{
    if (! (x == x))
        return 0;
    if (x >= 2147483647.0f)
        return 2147483647;
    if (x <= -2147483648.0f)
        return -2147483647 - 1;
    return static_cast<int>(x);
}
int qmap(float v, int lo, int hi) noexcept // rint(F(lo + F(v · (hi − 1 − lo))))
{
    APP_FX_NOFUSE
    const float s = v * static_cast<float>(hi - 1 - lo);
    return rintEven(static_cast<float>(lo) + s);
}
float thrDb(float v) noexcept
{
    return static_cast<float>(qmap(v, -52, 1));
}
float ratioOf(float v) noexcept
{
    return static_cast<float>(qmap(v, 10, 301)) * kTenth;
}
float gainDb(float v) noexcept
{
    return static_cast<float>(qmap(v, 0, 51));
}
float attackMs(float v) noexcept
{
    const int i = qmap(v, 1, 1020);
    return i <= 20 ? static_cast<float>(i) * k005 : static_cast<float>(i - 19);
}
float releaseMs(float v) noexcept
{
    return static_cast<float>(qmap(v, 10, 4001));
}
// Default maths: std:: in double, rounded to float.
float expF(float x) noexcept
{
    return static_cast<float>(std::exp(static_cast<double>(x)));
}
float powF(float x, float y) noexcept
{
    return static_cast<float>(std::pow(static_cast<double>(x), static_cast<double>(y)));
}
} // namespace

// ------------------------------------------------------------------------------------------------ meters (FX-1126)

void Compressor::Meter::update(float peak) noexcept
{
    float p = peak;
    if (p <= ck::kLevelFloor)
        p = ck::kLevelFloor;
    float v = 2.0f;
    if (p <= v)
        v = p;
    if (cur < v)
    {
        cur = v;
        cnt = 0;
    }
    else if (hold < cnt)
    {
        cnt = 0;
        const float d = cur * kMeterDecay;
        if (! (d < ck::kLevelFloor))
            cur = d;
    }
    else
        ++cnt;
}

void Compressor::Meter::reset() noexcept
{
    cur = ck::kLevelFloor;
    cnt = 0;
}

// ------------------------------------------------------------------------------------------------ set-up

void Compressor::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    const float fsf = static_cast<float>(sampleRate);
    win_ = std::max(1, truncS32((fsf * 3.0f) / 1000.0f)); // 132 / 144 / 288
    invWin_ = static_cast<float>(1.0 / static_cast<double>(win_));
    ring_.assign(static_cast<std::size_t>(win_), 0.0f);           // rt-ok: prepare, message thread
    laLen_ = truncS32((fsf + fsf) / 1000.0f) + 1;                  // 89 / 97 / 193
    laL_.assign(static_cast<std::size_t>(laLen_), 0.0f);           // rt-ok: prepare, message thread
    laR_.assign(static_cast<std::size_t>(laLen_), 0.0f);           // rt-ok: prepare, message thread
    laStep_ = static_cast<float>(1000.0 / static_cast<double>(fsf * 3.3333333f)); // 1/147
    e_ = expF(1.0f);
    einv_ = expF(-1.0f);
    const int hold = truncS32(10.0f / ((static_cast<float>(kBlockFrames) / fsf) * 1000.0f)); // 6 at 44.1 kHz
    for (auto& m : mtr_)
        m.hold = hold;
    clear();
}

void Compressor::clear() noexcept
{
    p_[0] = 1.0f; // compressor.py constructor values (the shell sends the full set on activation, FX-056)
    p_[1] = 0.0f;
    p_[2] = 1.0f;
    p_[3] = 1.0f;
    p_[4] = 0.5f;
    p_[5] = 0.0f;
    knee_ = 0.0f;
    detect_ = 0;
    lookAheadParam_ = lookAhead_ = false;
    dirty_ = true;
    slope_ = 1.0f;
    thrPt_ = 0.0f;
    gain_ = makeup_ = 1.0f;
    peak_ = 0.0f;
    ramp_ = 0;
    minGain_ = 1.0f;
    levelDb_ = 0.0f;
    hold_ = 0;
    target_ = 1.0f;
    lastKnee_ = 0.0f;
    lastThr_ = 1.0f;
    lastRatio_ = 0.0f;
    dry_ = 1.0f;
    wet_ = 0.0f;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    pos_ = cnt_ = 0;
    sum_ = 0.0;
    std::fill(laL_.begin(), laL_.end(), 0.0f);
    std::fill(laR_.begin(), laR_.end(), 0.0f);
    laPos_ = 0;
    laFade_ = 1.0f;
    for (auto& m : mtr_)
        m.reset();
    updateConstants();
    // FX-1183: the reference constructor computes the hold length before the release time is set (0); Sixteen
    // computes it from the actual release. Unobservable on the output (the hold only counts after an attack).
    holdLim_ = quirkHoldInit_ ? 0 : std::min(r2_, r1_);
}

void Compressor::reset() noexcept // slot 27 (0xf7fb8 → 0xf7e38), then slot 26 (EffectBase activation)
{
    gain_ = makeup_ = minGain_ = 1.0f;
    laPos_ = 0;
    std::fill(laL_.begin(), laL_.end(), 0.0f);
    std::fill(laR_.begin(), laR_.end(), 0.0f);
    // FX-1161: the crossfade restarts fully on the side of the current setting (the reference reads the copy of the
    // last block; the shell sends the options before reset(), so the current setting is the one the next block runs).
    lookAhead_ = lookAheadParam_;
    laFade_ = lookAhead_ ? 0.0f : 1.0f;
    cnt_ = 0;
    hold_ = 0;
    sum_ = 0.0;
    target_ = 1.0f;
    for (auto& m : mtr_)
        m.reset();
    // slot 26: the mix uses make-up 1 until the next block
    dry_ = 1.0f - p_[5];
    wet_ = p_[5] * makeup_;
}

void Compressor::setParam(int index, float value01) noexcept
{
    if (index < 0 || index >= 6)
        return;
    p_[index] = value01;
    dirty_ = true;
}

void Compressor::setOption(int which, int value) noexcept
{
    switch (which)
    {
    case kOptLookAhead:
        lookAheadParam_ = value != 0; // copied at the next block start (process_send)
        break;
    case kOptDetect:
        detect_ = value;
        break;
    case kOptKnee:
        knee_ = value != 0 ? 1.0f : 0.0f;
        dirty_ = true;
        break;
    case kClearState:
        if (value != 0)
            clear();
        break;
    default:
        break;
    }
}

int Compressor::latencySamples() const noexcept
{
    return lookAheadParam_ ? laLen_ - 1 : 0;
}

float Compressor::telemetry(int which) const noexcept
{
    switch (which)
    {
    case kTelemetryDry:
        return dry_;
    case kTelemetryWet:
        return wet_;
    case kTelGainReduction:
    {
        const float g = minGain_ > ck::kLevelFloor ? minGain_ : ck::kLevelFloor;
        const float db = static_cast<float>(-20.0 * std::log10(static_cast<double>(g)));
        const float v = db / kGrFullScaleDb;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    case kTelOutput:
    {
        const float a = mtr_[2].cur > mtr_[3].cur ? mtr_[2].cur : mtr_[3].cur;
        const float v = a * makeup_;
        return v > 2.0f ? 2.0f : v;
    }
    case kTelInputL:
        return mtr_[0].cur;
    case kTelInputR:
        return mtr_[1].cur;
    case kTelMinGain:
        return minGain_;
    default:
        return 0.0f;
    }
}

// ------------------------------------------------------------------------------------------------ per block

void Compressor::updateConstants() noexcept // the parameter reads of gain_stage, recomputed only after a change
{
    APP_FX_NOFUSE
    const float fsf = static_cast<float>(fs_);
    thr_ = thrDb(p_[0]);
    invRatio_ = 1.0f / ratioOf(p_[1]);
    kneeDb_ = knee_ * 18.0f;
    makeupTarget_ = powF(10.0f, gainDb(p_[4]) * k005);
    const float A = attackMs(p_[2]);
    a_ = A == 0.0f ? einv_ : expF(-(1.0f / ((A * fsf) * kMilli)));
    const float Rl = releaseMs(p_[3]);
    r_ = Rl == 0.0f ? e_ : expF(1.0f / ((Rl * fsf) * kMilli));
    r1_ = truncS32((fsf * kMilli) * 10.0f);              // 441 (10 ms)
    r2_ = truncS32(((fsf * Rl) * kMilli) * 0.25f);       // release / 4 in samples
    const int m = truncS32(fsf * kMilli);                 // 44
    ms44_ = kBlockFrames >= m ? kBlockFrames : m;
    dirty_ = false;
}

void Compressor::detectRms(const float* l, const float* r) noexcept // slot 25 inline, LevelDetect == 1
{
    ramp_ = 1;
    float mx = 0.0f;
    double s = sum_;
    int pos = pos_;
    float* ring = ring_.data();
    for (int i = 0; i < kBlockFrames; ++i)
    {
        float w = 0.0f;
        if (cnt_ < win_)
            ++cnt_;
        else
            w = 1.0f;
        const float old = w * ring[pos];
        const float a = l[i] * l[i];
        const float b = r[i] * r[i];
        const float x = a > b ? a : b;
        s = s - static_cast<double>(old);
        ring[pos] = x;
        s = s + static_cast<double>(x);
        const float ms = static_cast<float>(s * static_cast<double>(invWin_));
        if (ms > mx)
        {
            ramp_ = i + 1;
            mx = ms;
        }
        if (++pos >= win_)
            pos = 0;
    }
    pos_ = pos;
    sum_ = s;
    const float lv = static_cast<float>(std::sqrt(static_cast<double>(mx)) * kSqrt2);
    peak_ = lv < ck::kLevelFloor ? ck::kLevelFloor : lv;
}

void Compressor::gainComputer() noexcept // gain_stage 0xf8c40 up to the per-sample ramp
{
    APP_FX_NOFUSE
    constexpr int n = kBlockFrames;
    if (p_[0] != lastThr_)
    {
        lastThr_ = p_[0];
        ramp_ = ms44_;
    }
    if (p_[1] != lastRatio_)
    {
        lastRatio_ = p_[1];
        ramp_ = ms44_;
    }
    const float T = thr_, invR = invRatio_, K = kneeDb_;
    const float L = static_cast<float>(20.0 * std::log10(static_cast<double>(peak_)));
    levelDb_ = L;
    if (K > 0.0f)
    {
        if (lastKnee_ <= 0.0f)
            ramp_ = ms44_;
        const float lo = T - K, twoK = K + K, hi = T + K, invW = 1.0f / twoK;
        slope_ = 1.0f;
        if (lo < L)
        {
            if (hi < L)
                slope_ = invR;
            if (hi >= L)
            {
                const float s10 = (L - lo) * invW;
                slope_ = 1.0f + (invR - 1.0f) * s10;
            }
            thrPt_ = -160.0f;
            if (hi < L)
                thrPt_ = T;
            else if (hi >= L)
            {
                const float s14 = (L - lo) * invW;
                thrPt_ = lo + (T - lo) * s14;
            }
        }
        else
            thrPt_ = -160.0f;
    }
    else
    {
        if (lastKnee_ > 0.0f)
            ramp_ = ms44_;
        slope_ = 1.0f;
        if (T < L)
        {
            slope_ = invR;
            thrPt_ = T;
        }
        else
            thrPt_ = -160.0f;
    }
    lastKnee_ = K;
    const int r1 = r1_;
    int r2 = r2_;
    int r3 = r2 < r1 ? r2 : r1;
    bool attack = false;
    float gt = 1.0f;
    if (L > -160.0f)
    {
        const float y = thrPt_ + (L - thrPt_) * slope_;
        gt = powF(10.0f, (L - y) * kM005);
        if (gt < 1.0f && ! (gt > target_ * kHyst))
        {
            attack = true;
            hold_ = r2;
            r3 = ramp_;
            if (gt < target_)
                target_ = gt;
            if (r2 > r1)
            {
                hold_ = r1;
                r2 = r1;
            }
            holdLim_ = r2;
        }
    }
    if (! attack)
    {
        bool expired = true;
        int h = hold_;
        if (h > 0)
        {
            h -= n;
            hold_ = h;
            if (r3 < holdLim_ && r3 < h)
            {
                hold_ = r3;
                h = r3;
            }
            if (h > 0)
                expired = false;
        }
        if (expired)
        {
            float tg = target_ * e_;
            target_ = tg;
            if (gt < tg)
            {
                target_ = gt;
                tg = gt;
            }
            if (tg > 1.0f)
                target_ = 1.0f;
        }
        ramp_ = ms44_;
        r3 = ramp_;
    }
    if (r3 <= 0)
    {
        r3 = 1;
        ramp_ = 1;
    }
    // FX-1123 step 1: one pow per block (none while the gain sits on its target: pow(1, y) = 1 exactly)
    const float ratio = target_ / gain_;
    const float k = ratio == 1.0f ? 1.0f : powF(ratio, static_cast<float>(1.0 / static_cast<double>(r3)));
    float g = gain_;
    const float tgt = target_;
    if (k == 1.0f && g == tgt && a_ * g < g)
    {
        // Settled: the ramp keeps g and the slew limit passes it (g > a·g); the block's gain is constant.
        for (int i = 0; i < n; ++i)
            gbuf_[i] = g;
        minGain_ = g < 1.0f ? g : 1.0f;
        makeup_ = makeupTarget_;
        return;
    }
    if (k > 1.0f)
        for (int i = 0; i < n; ++i)
        {
            g = g * k;
            if (g > tgt)
                g = tgt;
            gbuf_[i] = g;
        }
    else
        for (int i = 0; i < n; ++i)
        {
            g = g * k;
            if (g < tgt)
                g = tgt;
            gbuf_[i] = g;
        }
    // FX-1123 step 2: the slew limits (the reference's compare order, unordered compares take neither branch)
    float mn = 1.0f, s15 = 1.0f, cur = gain_;
    const float a = a_, r = r_;
    for (int i = 0; i < n; ++i)
    {
        const float b = gbuf_[i];
        if (cur >= b)
        {
            const float s12 = a * cur;
            if (b > s12)
                s15 = b;
            else if (b <= s12)
                s15 = s12;
        }
        if (cur < b)
        {
            const float s14 = r * cur;
            if (b < s14)
                s15 = b;
            else if (b >= s14)
                s15 = s14;
        }
        cur = s15;
        gbuf_[i] = cur;
        if (cur < mn)
            mn = cur;
    }
    gain_ = cur;
    minGain_ = mn;
    makeup_ = makeupTarget_;
}

template <bool Neon> bool Compressor::lookAhead(const float* l, const float* r) noexcept
{
    // process_send: per sample, read the delayed sample (laLen_ − 1 behind) and then write the input. The ring is at
    // least one block + 1 long, so this is one block read from w + 1 followed by one block write at w (each read
    // happens before the write of the same slot, as in the per-sample loop), in at most two segments each.
    const int nl = laLen_, w = laPos_;
    const bool settledOff = ! lookAhead_ && laFade_ == 1.0f;
    if (! settledOff)
    {
        const int rd = w + 1 < nl ? w + 1 : 0;
        const int a = nl - rd < kBlockFrames ? nl - rd : kBlockFrames;
        std::memcpy(aL_, laL_.data() + rd, sizeof(float) * static_cast<std::size_t>(a));
        std::memcpy(aR_, laR_.data() + rd, sizeof(float) * static_cast<std::size_t>(a));
        std::memcpy(aL_ + a, laL_.data(), sizeof(float) * static_cast<std::size_t>(kBlockFrames - a));
        std::memcpy(aR_ + a, laR_.data(), sizeof(float) * static_cast<std::size_t>(kBlockFrames - a));
    }
    const int b = nl - w < kBlockFrames ? nl - w : kBlockFrames;
    std::memcpy(laL_.data() + w, l, sizeof(float) * static_cast<std::size_t>(b));
    std::memcpy(laR_.data() + w, r, sizeof(float) * static_cast<std::size_t>(b));
    std::memcpy(laL_.data(), l + b, sizeof(float) * static_cast<std::size_t>(kBlockFrames - b));
    std::memcpy(laR_.data(), r + b, sizeof(float) * static_cast<std::size_t>(kBlockFrames - b));
    laPos_ = w + kBlockFrames < nl ? w + kBlockFrames : w + kBlockFrames - nl;
    // Settled on one side: the mix is the identity on that side (up to the sign of a zero).
    if (settledOff)
        return false; // the gain applies to the input itself
    if (lookAhead_ && laFade_ == 0.0f)
        return true; // aL_/aR_ hold the delayed signal
    const float step = lookAhead_ ? -laStep_ : laStep_;
    for (int i = 0; i < kBlockFrames; ++i)
    {
        float f = step + laFade_;
        float c;
        laFade_ = f;
        if (f > 1.0f)
        {
            laFade_ = 1.0f;
            f = 1.0f;
            c = 0.0f;
        }
        else if (f >= 0.0f)
            c = 1.0f - f;
        else
        {
            laFade_ = 0.0f;
            f = 0.0f;
            c = 1.0f;
        }
        fbuf_[i] = f;
        cbuf_[i] = c;
    }
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        ck::neon::crossfade(aL_, aL_, l, cbuf_, fbuf_, kBlockFrames);
        ck::neon::crossfade(aR_, aR_, r, cbuf_, fbuf_, kBlockFrames);
        return true;
    }
#endif
    ck::scalar::crossfade(aL_, aL_, l, cbuf_, fbuf_, kBlockFrames);
    ck::scalar::crossfade(aR_, aR_, r, cbuf_, fbuf_, kBlockFrames);
    return true;
}

template <bool Neon> void Compressor::run(float* left, float* right) noexcept
{
    if (dirty_)
        updateConstants();
    lookAhead_ = lookAheadParam_;
    const bool delayed = lookAhead<Neon>(left, right);
    const float* pl = delayed ? aL_ : left;
    const float* pr = delayed ? aR_ : right;
    float inL, inR;
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        inL = dsp::vec::neon::peakAbs(left, kBlockFrames);
        inR = dsp::vec::neon::peakAbs(right, kBlockFrames);
    }
    else
#endif
    {
        inL = dsp::vec::scalar::peakAbs(left, kBlockFrames);
        inR = dsp::vec::scalar::peakAbs(right, kBlockFrames);
    }
    mtr_[0].update(inL);
    mtr_[1].update(inR);
    if (detect_ == 1)
        detectRms(left, right);
    else
    {
        // detect_peak: also clears the RMS sum and fill count (the reference shares the fields)
        sum_ = 0.0;
        cnt_ = 0;
        int pos = 1;
#if APP_FX_HAVE_NEON
        if constexpr (Neon)
            peak_ = ck::neon::peakDetect(left, right, kBlockFrames, pos);
        else
#endif
            peak_ = ck::scalar::peakDetect(left, right, kBlockFrames, pos);
        ramp_ = pos;
    }
    gainComputer();
    float outL, outR;
#if APP_FX_HAVE_NEON
    if constexpr (Neon)
    {
        ck::neon::applyGain(gbuf_, pl, pr, left, right, kBlockFrames);
        outL = dsp::vec::neon::peakAbs(left, kBlockFrames);
        outR = dsp::vec::neon::peakAbs(right, kBlockFrames);
    }
    else
#endif
    {
        ck::scalar::applyGain(gbuf_, pl, pr, left, right, kBlockFrames);
        outL = dsp::vec::scalar::peakAbs(left, kBlockFrames);
        outR = dsp::vec::scalar::peakAbs(right, kBlockFrames);
    }
    mtr_[2].update(outL);
    mtr_[3].update(outR);
    // slot 26 (update_mix): dry = F(1 − v), wet = F(v · makeup)
    dry_ = 1.0f - p_[5];
    wet_ = p_[5] * makeup_;
}

void Compressor::processScalar(float* left, float* right) noexcept
{
    run<false>(left, right);
}

#if APP_FX_HAVE_NEON
void Compressor::processNeon(float* left, float* right) noexcept
{
    run<true>(left, right);
}
#endif

void Compressor::process(float* left, float* right, const ProcessContext&) noexcept
{
#if APP_FX_HAVE_NEON
    run<true>(left, right);
#else
    run<false>(left, right);
#endif
}
} // namespace engine::fx::effects

// Registration: Insert, Send and Master; shell gate mode 0 (FX-1162), no wet high-pass (FX-028),
// the effect-defined mix dry 1 − v / wet v·makeup (FX-1124), and the reference Insert skip/clamp (FX-033/1180, honoured
// by the shell only in an Insert slot with the Reference quirk profile).
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "compressor",
    .name = "COMPRESSOR",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.compressor.ratio", "fx.compressor.attack", "fx.compressor.release", "fx.compressor.threshold",
               "fx.compressor.gain", "fx.compressor.mix"},
    .options = engine::fx::effects::kCompressorOptions,
    .visual = "meters",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::Open),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::EffectDefined),
              .wetHighPass = false,
              .skipWhenSilent = true,
              .clampOutput = true},
    .create = &engine::fx::make<engine::fx::effects::Compressor>}))
