// Reverb — see Reverb.h.
#include "engine/fx/effects/Reverb.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"

#include <cmath>

namespace engine::fx
{
namespace
{
constexpr int kN = kBlockFrames;
static_assert(kN <= dsp::reverb::kMaxN);

// Line lengths at room 100 (ms). Lines 0–3 feed the left output, 4–7 the right; the two groups have about the same
// total length so neither side answers first. Lines 1–3 take the left input, 5–7 the right.
constexpr float kBaseMs[Reverb::kLines] = {48.7f, 29.7f, 65.9f, 43.9f, 54.1f, 34.3f, 59.3f, 39.1f};
constexpr float kDiffMsL[Reverb::kDiffusers] = {3.41f, 4.57f, 8.83f, 11.69f};
constexpr float kDiffMsR[Reverb::kDiffusers] = {3.73f, 4.21f, 9.47f, 12.31f};
constexpr float kDiffG[Reverb::kDiffusers] = {0.75f, 0.75f, 0.625f, 0.625f};
constexpr float kModMs = 0.25f;                                         // ± line modulation
constexpr double kModHz[Reverb::kLines / 2] = {0.53, 0.71, 0.89, 1.07}; // lines 1, 3, 5, 7
constexpr float kRoomGlide = 0.03f; // samples per sample on the line that moves furthest (≈ 3 % pitch while gliding)
constexpr float kPreGlide = 0.15f;  // pre-delay glide, samples per sample
constexpr float kCoefSmooth = 0.3f; // per block toward the coefficient targets (≈ 5 ms)
constexpr float kInvSqrt8 = 0.35355339f;
// Levels (wet at the reference send defaults about the level of the earlier reverb).
constexpr float kInCal = 1.0f;
constexpr float kOut = 1.45f;
constexpr float kEarly = 0.35f;
constexpr float kT60v[5] = {0.0f, 0.09f, 0.3f, 49.0f / 99.0f, 1.0f};
constexpr float kT60s[5] = {0.10f, 0.87f, 3.55f, 8.70f, 100.6f};

float clamp01(float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
float oddRound(float x) noexcept // the nearest odd integer
{
    return 2.0f * std::floor(x * 0.5f) + 1.0f;
}
float peak(const float* l, const float* r, int n) noexcept
{
    const float a = dsp::vec::peakAbs(l, n), b = dsp::vec::peakAbs(r, n);
    return a > b ? a : b;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- laws

float Reverb::t60(float v) noexcept // log T60 linear in v between the anchors
{
    const double x = v < 0.0f ? 0.0 : (v > 1.0f ? 1.0 : static_cast<double>(v));
    int i = 0;
    while (i < 3 && x > static_cast<double>(kT60v[i + 1]))
        ++i;
    const double v0 = static_cast<double>(kT60v[i]), v1 = static_cast<double>(kT60v[i + 1]);
    const double l0 = std::log(static_cast<double>(kT60s[i])), l1 = std::log(static_cast<double>(kT60s[i + 1]));
    const double u = v1 > v0 ? (x - v0) / (v1 - v0) : 0.0;
    return static_cast<float>(std::exp(l0 + u * (l1 - l0)));
}

float Reverb::roomScale(int room) noexcept // 0.2·5^((room − 1)/99)
{
    const int r = room < 1 ? 1 : (room > 100 ? 100 : room);
    return static_cast<float>(0.2 * std::pow(5.0, static_cast<double>(r - 1) / 99.0));
}

float Reverb::inputGain(float v) noexcept // 0.2, then 0.2 − 0.1·2.0564518·(v − 0.5137255): long decays build up less
{
    APP_FX_NOFUSE
    const float x = clamp01(v);
    const float k = 0.5137255f;
    if (x <= k)
        return 0.2f;
    return 0.2f + ((x - k) * 2.0564518f) * -0.1f;
}

float Reverb::dampingMaxHz(double fs) noexcept
{
    if (fs == 44100.0)
        return 18000.0f;
    const double m = 0.4 * fs;
    return static_cast<float>(m < 18000.0 ? m : 18000.0);
}

float Reverb::dampingHz(float v, double fs) noexcept // step = trunc(1 + 99(1 − v)) → 2000·9^((step − 1)/99)
{
    const float x = 1.0f + (1.0f - clamp01(v)) * 99.0f;
    int step = static_cast<int>(x);
    step = step < 1 ? 1 : (step > 100 ? 100 : step);
    const float hz = static_cast<float>(2000.0 * std::pow(9.0, static_cast<double>(step - 1) / 99.0));
    const float mx = dampingMaxHz(fs);
    return hz < mx ? hz : mx;
}

float Reverb::baseDelayMs(int line) noexcept
{
    return line >= 0 && line < kLines ? kBaseMs[line] : 0.0f;
}

// ---------------------------------------------------------------------------------------------------- set-up

void Reverb::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    spm_ = static_cast<float>(sampleRate / 1000.0);
    depth_ = kModMs * spm_;
    bytes_ = 0;
    const int pdLen = static_cast<int>(100.0f * spm_) + kN + 4;
    bytes_ += pdL_.prepare(pdLen) + pdR_.prepare(pdLen);
    for (int j = 0; j < kDiffusers; ++j)
    {
        bytes_ += apL_[j].prepare(static_cast<int>(kDiffMsL[j] * spm_ + 0.5f), kDiffG[j]);
        bytes_ += apR_[j].prepare(static_cast<int>(kDiffMsR[j] * spm_ + 0.5f), kDiffG[j]);
    }
    for (int i = 0; i < kLines; ++i)
        dMax_[i] = oddRound(kBaseMs[i] * spm_) + depth_ + 2.0f;
    bytes_ += fdn_.prepare(dMax_);
    const float in[kLines] = {0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f}; // 1–3 left, 5–7 right
    fdn_.setInputGains(in);
    hpA_ = static_cast<float>(1.0 / (1.0 + 2.0 * dsp::delay::kPi * 80.0 / sampleRate));
    lpC_ = static_cast<float>(1.0 - std::exp(-2.0 * dsp::delay::kPi * 12000.0 / sampleRate));
    for (int i = 0; i < kLines / 2; ++i)
        lfo_[i].setRate(kModHz[i], sampleRate, kN);
    for (auto& v : lastP_)
        v = -1.0f;
    reset();
}

void Reverb::clearState() noexcept
{
    pdL_.clear();
    pdR_.clear();
    for (int j = 0; j < kDiffusers; ++j)
    {
        apL_[j].clear();
        apR_[j].clear();
    }
    fdn_.clear();
    for (int c = 0; c < 2; ++c)
        hpX_[c] = hpY_[c] = lpY_[c] = 0.0f;
}

void Reverb::reset() noexcept // FX-862: the stored values at once; rings cleared; the LFOs restart
{
    for (auto& v : lastP_)
        v = -1.0f;
    targets(true);
    clearState();
    for (int i = 0; i < kLines / 2; ++i)
    {
        lfo_[i].setPhase(0.25 * static_cast<double>(i));
        mod_[2 * i + 1] = depth_ * lfo_[i].s;
    }
    quiet_ = 0;
    sleeping_ = false;
}

void Reverb::setParam(int index, float value01) noexcept
{
    if (index >= 0 && index < 6)
        p_[index] = value01;
}

void Reverb::setOption(int which, int value) noexcept
{
    if (which == kClearState && value != 0)
        clearState();
}

// ---------------------------------------------------------------------------------------------------- control

void Reverb::targets(bool snap) noexcept
{
    const bool roomChanged = p_[1] != lastP_[1];
    const bool decayChanged = roomChanged || p_[2] != lastP_[2] || p_[3] != lastP_[3];
    const bool preChanged = p_[0] != lastP_[0];
    if (! preChanged && ! decayChanged && p_[4] == lastP_[4])
        return;
    if (preChanged) // pre-delay
    {
        lastP_[0] = p_[0];
        const float ms = 100.0f * clamp01(p_[0]);
        const float t = std::round((ms < 0.1f ? 0.1f : ms) * spm_);
        if (t != pdTgt_ || snap)
        {
            pdTgt_ = t;
            pdStart_ = pdCur_;
            const float dist = std::fabs(pdTgt_ - pdCur_);
            pdProg_ = snap || dist == 0.0f ? 1.0f : 0.0f;
            pdStep_ = dist > 0.0f ? kPreGlide * static_cast<float>(kN) / dist : 1.0f;
            if (snap)
                pdCur_ = pdStart_ = pdTgt_;
        }
    }
    if (roomChanged)
    {
        lastP_[1] = p_[1];
        const float x = 1.0f + 99.0f * clamp01(p_[1]);
        room_ = static_cast<int>(x);
        room_ = room_ < 1 ? 1 : (room_ > 100 ? 100 : room_);
        const float s = roomScale(room_);
        const float minD = static_cast<float>(kN) + depth_ + 4.0f;
        float far = 0.0f;
        for (int i = 0; i < kLines; ++i)
        {
            float t = oddRound(kBaseMs[i] * s * spm_);
            t = t < minD ? oddRound(minD) + 2.0f : t;
            t = t > dMax_[i] - depth_ - 2.0f ? dMax_[i] - depth_ - 2.0f : t;
            dTgt_[i] = t;
            dStart_[i] = dCur_[i];
            const float d = std::fabs(t - dCur_[i]);
            far = d > far ? d : far;
        }
        glide_ = snap || far == 0.0f ? 1.0f : 0.0f;
        glideStep_ = far > 0.0f ? kRoomGlide * static_cast<float>(kN) / far : 1.0f;
        if (snap)
            for (int i = 0; i < kLines; ++i)
                dCur_[i] = dStart_[i] = dTgt_[i];
    }
    if (decayChanged)
    {
        lastP_[2] = p_[2];
        lastP_[3] = p_[3];
        const double T = static_cast<double>(t60(p_[2]));
        const double fd = static_cast<double>(dampingHz(p_[3], fs_));
        for (int i = 0; i < kLines; ++i)
        {
            float a = 0.0f, b = 0.0f;
            dsp::reverb::absorbent(static_cast<double>(dTgt_[i]), fs_, T, fd, static_cast<double>(kDampRatio), a, b);
            aTgt_[i] = a;
            bTgt_[i] = b * kInvSqrt8;
        }
        inGain_ = inputGain(p_[2]) * kInCal;
        coefMoving_ = true;
    }
    if (p_[4] != lastP_[4])
    {
        lastP_[4] = p_[4];
        width_ = 1.0f + clamp01(p_[4]);
    }
    outGain_ = kOut;
    earlyGain_ = kEarly;
    // blocks of silence before the network may have received everything: pre-delay + diffusion + longest line
    float span = pdTgt_ + depth_;
    for (int j = 0; j < kDiffusers; ++j)
        span += static_cast<float>(apL_[j].delay() > apR_[j].delay() ? apL_[j].delay() : apR_[j].delay());
    float longest = 0.0f;
    for (int i = 0; i < kLines; ++i)
        longest = dTgt_[i] > longest ? dTgt_[i] : longest;
    quietNeeded_ = static_cast<int>((span + longest) / static_cast<float>(kN)) + 2;
    if (snap)
        coefficients(true);
}

void Reverb::coefficients(bool snap) noexcept
{
    if (! coefMoving_ && ! snap)
        return;
    bool moving = false;
    for (int i = 0; i < kLines; ++i)
    {
        float a = aTgt_[i], b = bTgt_[i];
        if (! snap)
        {
            const float da = aTgt_[i] - aCur_[i], db = bTgt_[i] - bCur_[i];
            if (std::fabs(da) > 1.0e-6f || std::fabs(db) > 1.0e-7f)
            {
                a = aCur_[i] + da * kCoefSmooth;
                b = bCur_[i] + db * kCoefSmooth;
                moving = true;
            }
        }
        aCur_[i] = a;
        bCur_[i] = b;
        fdn_.setFilter(i, a, b);
    }
    coefMoving_ = moving;
}

// ---------------------------------------------------------------------------------------------------- audio

template <bool Neon> void Reverb::run(float* l, float* r) noexcept
{
    APP_FX_NOFUSE
    const bool silent = peak(l, r, kN) <= kWakeLevel;
    if (silent)
    {
        if (quiet_ < (1 << 30))
            ++quiet_;
        if (sleeping_)
        {
            dsp::delay::zero(l, kN);
            dsp::delay::zero(r, kN);
            return;
        }
    }
    else
    {
        quiet_ = 0;
        if (sleeping_) // wake from cleared rings with the current knobs, no glides
        {
            sleeping_ = false;
            for (auto& v : lastP_)
                v = -1.0f;
            targets(true);
        }
    }
    targets(false);
    coefficients(false);

    // pre-delay (write first: any delay >= 0 reads ages n + D − k >= 1)
    dsp::delay::mul<Neon>(l, inGain_, kN);
    dsp::delay::mul<Neon>(r, inGain_, kN);
    pdL_.write(l, kN);
    pdR_.write(r, kN);
    const float pd0 = pdCur_;
    if (pdProg_ < 1.0f)
    {
        pdProg_ = pdProg_ + pdStep_ >= 1.0f ? 1.0f : pdProg_ + pdStep_;
        pdCur_ = pdProg_ >= 1.0f ? pdTgt_ : pdStart_ + (pdTgt_ - pdStart_) * pdProg_;
    }
    alignas(16) float xl[kN], xr[kN];
    if (pd0 == pdCur_)
    {
        const int a = kN + static_cast<int>(pdCur_);
        pdL_.readInt(a, xl, kN);
        pdR_.readInt(a, xr, kN);
    }
    else
    {
        const float a0 = static_cast<float>(kN) + pd0, da = (pdCur_ - pd0) / static_cast<float>(kN);
        pdL_.readLerp<Neon>(a0, da, xl, kN);
        pdR_.readLerp<Neon>(a0, da, xr, kN);
    }

    // input band: one-pole HPF 80 Hz y = a·y1 + a·(x − x1), one-pole LPF 12 kHz z = (1 − c)·z1 + c·y (both channels
    // in one loop: four short independent recursions)
    {
        float hxl = hpX_[0], hyl = hpY_[0], lyl = lpY_[0], hxr = hpX_[1], hyr = hpY_[1], lyr = lpY_[1];
        const float a = hpA_, c = lpC_, c1 = 1.0f - lpC_;
        for (int k = 0; k < kN; ++k)
        {
            const float il = xl[k], ir = xr[k];
            hyl = hyl * a + (il - hxl) * a;
            hyr = hyr * a + (ir - hxr) * a;
            hxl = il;
            hxr = ir;
            lyl = lyl * c1 + hyl * c;
            lyr = lyr * c1 + hyr * c;
            xl[k] = lyl;
            xr[k] = lyr;
        }
        hpX_[0] = hxl;
        hpY_[0] = hyl;
        lpY_[0] = lyl;
        hpX_[1] = hxr;
        hpY_[1] = hyr;
        lpY_[1] = lyr;
    }

    // input diffusion
    for (int j = 0; j < kDiffusers; ++j)
    {
        apL_[j].process<Neon>(xl, kN);
        apR_[j].process<Neon>(xr, kN);
    }

    // late field: room glide + line modulation
    float d0[kLines], d1[kLines];
    if (glide_ < 1.0f)
        glide_ = glide_ + glideStep_ >= 1.0f ? 1.0f : glide_ + glideStep_;
    for (int i = 0; i < kLines; ++i)
    {
        d0[i] = dCur_[i] + mod_[i];
        dCur_[i] = glide_ >= 1.0f ? dTgt_[i] : dStart_[i] + (dTgt_[i] - dStart_[i]) * glide_;
        if ((i & 1) != 0)
            mod_[i] = depth_ * lfo_[i >> 1].advance();
        d1[i] = dCur_[i] + mod_[i];
    }
    alignas(16) float fl[kN], fr[kN];
    fdn_.process<Neon>(xl, xr, fl, fr, d0, d1, kN);

    // wet = late + early (the diffused input), then the width
    dsp::delay::mul<Neon>(fl, outGain_, kN);
    dsp::delay::mul<Neon>(fr, outGain_, kN);
    dsp::delay::mul<Neon>(xl, earlyGain_, kN);
    dsp::delay::mul<Neon>(xr, earlyGain_, kN);
    dsp::delay::add<Neon>(fl, xl, kN);
    dsp::delay::add<Neon>(fr, xr, kN);
    if (width_ != 1.0f)
    {
        const float w = 0.5f * width_;
        for (int k = 0; k < kN; ++k)
        {
            const float m = (fl[k] + fr[k]) * 0.5f, s = (fl[k] - fr[k]) * w;
            l[k] = m + s;
            r[k] = m - s;
        }
    }
    else
    {
        dsp::delay::copy<Neon>(l, fl, kN);
        dsp::delay::copy<Neon>(r, fr, kN);
    }

    // sleep once the tail is gone
    if (silent && quiet_ >= quietNeeded_ && peak(l, r, kN) < kSleepLevel)
    {
        sleeping_ = true;
        clearState();
        dsp::delay::zero(l, kN);
        dsp::delay::zero(r, kN);
    }
}

void Reverb::processScalar(float* l, float* r) noexcept
{
    run<false>(l, r);
}

#if APP_FX_HAVE_NEON
void Reverb::processNeon(float* l, float* r) noexcept
{
    run<true>(l, r);
}
#endif

void Reverb::process(float* l, float* r, const ProcessContext&) noexcept
{
#if APP_FX_HAVE_NEON
    run<true>(l, r);
#else
    run<false>(l, r);
#endif
}
} // namespace engine::fx

// Registration: the track inserts (FX1/FX2), Send and Master; gate mode 1, no wet high-pass, the reverb mix law
// (the delays' piecewise law, wet × 0.70795). No options.
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "reverb",
    .name = "REVERB",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"fx.reverb.preDelay", "fx.reverb.room", "fx.reverb.time", "fx.reverb.damping", "fx.reverb.spread",
               "fx.reverb.mix"},
    .options = {},
    .visual = "decayCurve",
    .shell = {.gateMode = static_cast<std::uint8_t>(engine::fx::GateMode::InputFade),
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::ReverbPiecewise),
              .wetHighPass = false,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::Reverb>,
    .featureFlag = {}}))
