// Reverb — see Reverb.h. The slot numbers in comments are the reference's (research docs/effects/send-reverb.md "1.60
// code map"): slot 32 parameter update, slot 17 (= SendFxReverb slot 33) comb coefficients, slot 7/6 room glide, slot 8
// chunk loop, slot 9 one chunk, slot 16 init. Float steps one C++ float operation each, never fused (APP_FX_NOFUSE).
#include "engine/fx/effects/Reverb.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"

#include <bit>
#include <cmath>

namespace engine::fx
{
namespace
{
using dsp::delay::f32;
using dsp::delay::truncS32;
using dsp::delay::truncU32;

const float kTwoThirds = std::bit_cast<float>(0x3f2aaaabu); // 0.6666667 (glide-time step, slot 35)
const float kChunk40ms = std::bit_cast<float>(0x3d23d70bu); // 0.04
constexpr float kApGain = 0.7f;

// slot 35 0x111780: the glide time moves ⅔ ms toward 40 − ⅔·|cur − tgt| (floor 20)
float glideTime(int cur, int tgt, float prev) noexcept
{
    APP_FX_NOFUSE
    const int d = cur > tgt ? cur - tgt : tgt - cur;
    float tt = 40.0f - static_cast<float>(d) * kTwoThirds;
    if (tt <= 20.0f)
        tt = 20.0f;
    float p = prev;
    if (p < tt)
        p = p + kTwoThirds;
    else if (p > tt)
        p = p - kTwoThirds;
    return p <= 20.0f ? 20.0f : p;
}

#if APP_TEST_HOOKS
float clampMs(float ms) noexcept // overrideConstants only (test hooks; unused in the device build)
{
    return ms < 0.1f ? 0.1f : (ms > 100.0f ? 100.0f : ms);
}
#endif
} // namespace

// ---------------------------------------------------------------------------------------------------- laws

float Reverb::t60(const ReverbConstants& k, float v) noexcept // FX-822: log T60 linear in v between the anchors
{
    const double x = v < 0.0f ? 0.0 : (v > 1.0f ? 1.0 : static_cast<double>(v));
    int i = 0;
    while (i < 3 && x > static_cast<double>(k.t60v[static_cast<std::size_t>(i + 1)]))
        ++i;
    const double v0 = static_cast<double>(k.t60v[static_cast<std::size_t>(i)]);
    const double v1 = static_cast<double>(k.t60v[static_cast<std::size_t>(i + 1)]);
    const double l0 = std::log(static_cast<double>(k.t60s[static_cast<std::size_t>(i)]));
    const double l1 = std::log(static_cast<double>(k.t60s[static_cast<std::size_t>(i + 1)]));
    const double u = v1 > v0 ? (x - v0) / (v1 - v0) : 0.0;
    return static_cast<float>(std::exp(l0 + u * (l1 - l0)));
}

float Reverb::roomScale(int room) noexcept // s = 0.2 + 0.171·(1.01 + 0.02306·room)²
{
    APP_FX_NOFUSE
    const float f = f32(1.01) + static_cast<float>(room) * f32(0.02306);
    return f32(0.2) + (f * f) * f32(0.171);
}

float Reverb::inputGain(float v) noexcept // 0.2, then 0.2 − 0.1·2.0564518·(v − 0.5137255); 0 outside [0, 1]
{
    APP_FX_NOFUSE
    if (v < 0.0f || v > 1.0f)
        return 0.0f;
    const float k = f32(0.5137255);
    if (v <= k)
        return std::bit_cast<float>(0x3e4ccccdu);
    return f32(0.2) + ((v - k) * f32(2.0564518)) * f32(-0.1);
}

void Reverb::spreadGains(float s, float (&a)[kCombs], float (&b)[kCombs]) noexcept // FX-826 (send_reverb.py)
{
    APP_FX_NOFUSE
    const float one = 1.0f, z = 0.0f, five = 5.0f;
    auto up = [&](double t) { return (s - f32(t)) * five; };
    auto dn = [&](double t) { return one - (s - f32(t)) * five; };
    auto put = [&](float a0, float b0, float a1, float b1, float a2, float b2, float a3, float b3, float a4, float b4)
    {
        a[0] = a0, b[0] = b0, a[1] = a1, b[1] = b1, a[2] = a2, b[2] = b2, a[3] = a3, b[3] = b3, a[4] = a4, b[4] = b4;
    };
    if (s < f32(0.2))
        put(one - s * five, z, one, z, one, z, one, z, z, s * five);
    else if (s < f32(0.4))
        put(z, up(0.2), dn(0.2), z, one, z, one, z, z, one);
    else if (s < f32(0.6))
        put(z, one, z, up(0.4), dn(0.4), z, one, z, z, one);
    else if (s < f32(0.8))
        put(z, one, z, one, z, up(0.6), dn(0.6), z, z, one);
    else
        put(z, one, z, one, z, one, z, up(0.8), z, one);
}

float Reverb::dampingMaxHz(double fs) noexcept // FX-824 (our choice): 18 kHz at 44.1 kHz, min(18 kHz, 0.4·fs) elsewhere
{
    if (fs == 44100.0)
        return 18000.0f;
    const double m = 0.4 * fs;
    return static_cast<float>(m < 18000.0 ? m : 18000.0);
}

// ---------------------------------------------------------------------------------------------------- set-up

void Reverb::prepare(double sampleRate, int)
{
    fs_ = sampleRate;
    const float spm = static_cast<float>(sampleRate) / 1000.0f;
    // Comb rings: the longest comb at room 100 plus 15 % (FX-821), at least the reference's 101 ms (the comb clamp is
    // 100 ms, so every overridden constant fits; the Reference profile's warm-up threshold is this length).
    float longest = 0.0f;
    for (int i = 0; i < kCombs; ++i)
    {
        longest = k_.combL[static_cast<std::size_t>(i)] > longest ? k_.combL[static_cast<std::size_t>(i)] : longest;
        longest = k_.combR[static_cast<std::size_t>(i)] > longest ? k_.combR[static_cast<std::size_t>(i)] : longest;
    }
    const double longestMs = static_cast<double>(longest) * static_cast<double>(roomScale(100)) * 1.15;
    const int sized = static_cast<int>(longestMs * sampleRate / 1000.0) + 2;
    const int ref = static_cast<int>(spm * 101.0f);
    combLen_ = sized > ref ? sized : ref;
    bytes_ = pd_.prepare(sampleRate);
    const float dmax = dampingMaxHz(sampleRate);
    for (auto& c : combs_)
        bytes_ += c.prepare(sampleRate, combLen_, dmax);
    for (auto& a : aps_)
        bytes_ += a.prepare(sampleRate);
    lpf_.prepare(dsp::delay::SteppedIir1::Kind::LowPass, sampleRate, 2, 14000.0, 14000.0);
    hpf_.prepare(dsp::delay::SteppedIir1::Kind::HighPass, sampleRate, 2, 80.0, 80.0);
    init();
}

void Reverb::init() noexcept // the constructor 0x1123d0 / 0x10f644
{
    constexpr float kCtor[6] = {0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 6; ++k)
        p_[k] = kCtor[k];
    roomCur_ = roomSet_ = roomTgt_ = 50;
    glideT_ = 40.0f;
    chunkN_ = truncS32(static_cast<float>(fs_) * kChunk40ms);
    chunkRem_ = 0;
    idle_ = true;
    inGain_ = 0.0f;
    lpfStep_ = 0;
    pd_.init();
    for (auto& c : combs_)
        c.init();
    for (auto& a : aps_)
        a.init();
    lpf_.init();
    hpf_.init();
    initFixed();
    lastT_ = lastSpread_ = -1.0f;
    lastRoom_ = -1;
    lastStep_ = 0xffffffffu;
}

#if APP_TEST_HOOKS
void Reverb::overrideConstants(const ReverbConstants& k) noexcept
{
    k_ = k;
    for (auto& v : k_.apL)
        v = clampMs(v);
    for (auto& v : k_.apR)
        v = clampMs(v);
    lastT_ = -1.0f; // the next slot 17 recomputes every comb
}
#endif

void Reverb::initFixed() noexcept // slot 16 0x10eae0 (the fixed filters are set up in prepare)
{
    for (int i = 0; i < 2; ++i)
    {
        aps_[i].setDelay(k_.apL[static_cast<std::size_t>(i)], 0);
        aps_[i].setDelay(k_.apR[static_cast<std::size_t>(i)], 1);
        aps_[i].setGain(kApGain, 0);
        aps_[i].setGain(kApGain, 1);
    }
}

void Reverb::setParam(int index, float value01) noexcept
{
    if (index >= 0 && index < 6)
        p_[index] = value01;
}

void Reverb::setOption(int which, int value) noexcept
{
    if (which == kClearState && value != 0)
        init();
}

// ---------------------------------------------------------------------------------------------------- control

void Reverb::slot32() noexcept // 0x1120c4 (the mix law is the shell's)
{
    APP_FX_NOFUSE
    roomSet_ = truncS32(1.0f + 99.0f * p_[1]);
    lpfStep_ = truncU32(1.0f + (1.0f - p_[3]) * 99.0f);
}

void Reverb::slot17(bool force) noexcept // SendFxReverb slot 33 0x11185c
{
    APP_FX_NOFUSE
    const bool shared = (active_ & kReverbQuirkSharedRamp) != 0;
    const float T = t60(k_, p_[2]);
    if (force || T != lastT_ || roomCur_ != lastRoom_)
    {
        lastT_ = T;
        lastRoom_ = roomCur_;
        const float s = roomScale(roomCur_);
        for (int i = 0; i < kCombs; ++i)
        {
            const float dl = s * k_.combL[static_cast<std::size_t>(i)];
            const float dr = s * k_.combR[static_cast<std::size_t>(i)];
            const float el = ((dl * -3.0f) * dsp::delay::kMilli) / T; // FX-823: g = 10^(−3·d/(1000·T60))
            const float er = ((dr * -3.0f) * dsp::delay::kMilli) / T;
            const float gl = static_cast<float>(std::pow(10.0, static_cast<double>(el)));
            const float gr = static_cast<float>(std::pow(10.0, static_cast<double>(er)));
            combs_[i].setDelay(dl, 0, shared);
            combs_[i].setDelay(dr, 1, shared);
            combs_[i].setGain(gl, 0, shared);
            combs_[i].setGain(gr, 1, shared);
        }
    }
    if (force || lpfStep_ != lastStep_)
    {
        lastStep_ = lpfStep_;
        for (auto& c : combs_)
            c.setDampStep(lpfStep_);
    }
    if (force || p_[4] != lastSpread_)
    {
        lastSpread_ = p_[4];
        float a[kCombs], b[kCombs];
        spreadGains(p_[4], a, b);
        for (int i = 0; i < kCombs; ++i)
        {
            combs_[i].setSpread(a[i], 0, shared);
            combs_[i].setSpread(b[i], 1, shared);
        }
    }
    inGain_ = inputGain(p_[2]);
}

void Reverb::slot7() noexcept // 0x10e540: new glide time and chunk length, comb coefficients
{
    APP_FX_NOFUSE
    const float prev = ! idle_ ? glideT_ : 40.0f;
    const float t = glideTime(roomCur_, roomTgt_, prev);
    glideT_ = t;
    chunkN_ = chunkRem_ = truncS32((t * dsp::delay::kMilli) * static_cast<float>(fs_));
    for (auto& c : combs_)
        c.setGlide(t);
    slot17(false);
}

void Reverb::slot6() noexcept // 0x10e4d8: one room step toward the target
{
    if (roomCur_ == roomTgt_)
    {
        idle_ = true;
        return;
    }
    roomCur_ += roomCur_ < roomTgt_ ? 1 : -1;
    slot7();
}

void Reverb::reset() noexcept // slot 27 0x11208c: slot 32, core reset 0x10fae0, pre-delay reset 0x1112b8
{
    active_ = quirks_;
    const bool warmUp = (active_ & kReverbQuirkWarmUp) != 0;
    const bool stale = (active_ & kReverbQuirkStaleRing) != 0;
    slot32();
    const bool glideIn = (active_ & kReverbQuirkGlideIn) != 0;
    if (! glideIn) // Sixteen: the stored room and pre-delay at once (FX-056)
    {
        roomTgt_ = roomSet_;
        roomCur_ = roomSet_;
        pd_.set(p_[0]);
    }
    if ((active_ & kReverbQuirkGlide100) == 0) // FX-886 fix: the combs glide over the current t (40 ms here)
        for (auto& c : combs_)
            c.setGlide(40.0f);
    initFixed();
    slot17(true);
    lpf_.zeroState();
    hpf_.zeroState();
    for (auto& c : combs_)
        c.reset(stale, warmUp);
    for (auto& a : aps_)
        a.reset(stale, warmUp);
    roomCur_ = roomTgt_;
    glideT_ = glideTime(roomTgt_, roomTgt_, 40.0f);
    chunkN_ = chunkRem_ = 0;
    idle_ = true;
    pd_.reset(stale, warmUp);
}

// ---------------------------------------------------------------------------------------------------- audio

template <bool Neon> void Reverb::chunk(float* l, float* r, int n) noexcept // slot 9 0x10e418, in place
{
    pd_.set(p_[0]); // slot 34
    pd_.process(l, r, n);
    dsp::delay::mul<Neon>(l, inGain_, n); // slot 11 (always multiplied)
    dsp::delay::mul<Neon>(r, inGain_, n);
    lpf_.process<Neon>(l, r, n);
    hpf_.process<Neon>(l, r, n);
    dsp::delay::zero(accL_, n); // slot 12: five parallel combs, summed in order
    dsp::delay::zero(accR_, n);
    for (auto& c : combs_)
    {
        c.process<Neon>(l, r, oL_, oR_, n);
        dsp::delay::add<Neon>(accL_, oL_, n);
        dsp::delay::add<Neon>(accR_, oR_, n);
    }
    dsp::delay::copy<Neon>(l, accL_, n);
    dsp::delay::copy<Neon>(r, accR_, n);
    for (auto& a : aps_) // slot 13
        a.process<Neon>(l, r, n);
}

template <bool Neon> void Reverb::run(float* l, float* r) noexcept // slot 25 0x111730
{
    slot32();
    if (roomSet_ != roomTgt_) // core slot 18 0x10e2e0
    {
        roomTgt_ = roomSet_;
        idle_ = false;
        slot7();
    }
    slot17(false);
    int n = kBlockFrames, pos = 0; // slot 8 0x10e34c: chunks end at room steps
    while (n > 0)
    {
        if (idle_ || chunkRem_ > n)
        {
            chunk<Neon>(l + pos, r + pos, n);
            if (! idle_)
                chunkRem_ -= n;
            return;
        }
        const int c = chunkRem_;
        chunk<Neon>(l + pos, r + pos, c);
        n -= c;
        pos += c;
        slot6();
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

// Registration: Send and Master (index 1 of both lists), gate mode 1 (FX-860), no wet high-pass
// (F141), the reverb mix law (the delays' piecewise law, wet × 0.70795, FX-828). No options.
APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "reverb",
    .name = "REVERB",
    .slots = static_cast<std::uint8_t>(engine::fx::SlotKind::Send | engine::fx::SlotKind::Master),
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
