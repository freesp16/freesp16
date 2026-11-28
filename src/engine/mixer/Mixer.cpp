#include "engine/mixer/Mixer.h"

#include "core/util/GainLaw.h"
#include "engine/dsp/simd/Kernels.h"

#include <cmath>
#include <cstring>

#if APP_MIXER_HAVE_NEON
 #include <arm_neon.h>
#endif

// Bit-exact parity with the float32 research model (MixerKernels.h): no FMA contraction in the mixer.
#if defined(__clang__)
 #pragma clang fp contract(off)
#elif defined(__GNUC__)
 #pragma GCC optimize("fp-contract=off")
#endif

namespace engine::mixer
{
namespace
{
constexpr int kN = kBlockFrames;

int roundIndex(float v) noexcept
{
    if (! (v == v)) // NaN
        return 0;
    return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
}

int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// Linear balance (track_mix.pan_gains, F097): 0 dB in the centre, no cross-feed.
void panGains(int p, float& l, float& r) noexcept
{
    l = r = 1.0f;
    if (p < 0)
        r = static_cast<float>(p + 64) * 0.015625f;
    else if (p > 0)
        l = static_cast<float>(64 - p) * 0.015625f;
}

float faderGain(int index) noexcept { return core::GainLaw::gain(index); }
float sendGain(int index) noexcept { return core::GainLaw::gain(index) * 0.5f; } // 127 → 1.0, 100 → 0.5

StripParams sanitise(StripParams p) noexcept
{
    p.volume = static_cast<std::uint8_t>(p.volume > 127 ? 127 : p.volume);
    p.send = static_cast<std::uint8_t>(p.send > 127 ? 127 : p.send);
    p.pan = static_cast<std::int8_t>(clampi(p.pan, -64, 64));
    p.sendChannel = static_cast<std::uint8_t>(p.sendChannel > 1 ? 1 : p.sendChannel);
    return p;
}

// dst = src × ramp (in place when dst == src); skips the multiply when the ramp is idle at exactly 1.
void applyGain(GainRamp& g, float* gainBuf, float* dstL, float* dstR, const float* srcL, const float* srcR) noexcept
{
    if (g.idle())
    {
        const float v = g.current();
        if (v == 1.0f)
        {
            if (dstL != srcL)
            {
                std::memcpy(dstL, srcL, sizeof(float) * kN);
                std::memcpy(dstR, srcR, sizeof(float) * kN);
            }
            return;
        }
        simd::scale(dstL, srcL, v, kN);
        simd::scale(dstR, srcR, v, kN);
        return;
    }
    g.fill(gainBuf, kN);
    if (dstL != srcL)
    {
        std::memcpy(dstL, srcL, sizeof(float) * kN);
        std::memcpy(dstR, srcR, sizeof(float) * kN);
    }
    simd::multiply(dstL, gainBuf, kN);
    simd::multiply(dstR, gainBuf, kN);
}

void storeMeter(MeterBlock& m, int index, const kernels::StereoMeter& v) noexcept
{
    m.peak[index][0] = v.peakL;
    m.peak[index][1] = v.peakR;
    m.sumSq[index][0] = v.sumL;
    m.sumSq[index][1] = v.sumR;
    if (v.peakL >= 1.0f || v.peakR >= 1.0f)
        m.clip |= 1u << index;
}
} // namespace

// ---- SoftLimiter ----

SoftLimiter::SoftLimiter() noexcept
{
    const double t = std::pow(10.0, -3.0 / 20.0);
    const double c = std::pow(10.0, -0.3 / 20.0);
    threshold_ = static_cast<float>(t);
    ceiling_ = static_cast<float>(c);
    if (static_cast<double>(ceiling_) > c)
        ceiling_ = std::nextafter(ceiling_, 0.0f); // the ceiling is never above −0.3 dBFS
    const double d = c - static_cast<double>(threshold_);
    scale_ = static_cast<float>(kTable / static_cast<double>(kRange) / d);
    for (int k = 0; k <= kTable; ++k)
    {
        const double u = k * static_cast<double>(kRange) / kTable;
        const float y = static_cast<float>(static_cast<double>(threshold_) + d * std::tanh(u));
        table_[k] = y < ceiling_ ? y : ceiling_;
    }
}

float SoftLimiter::shape(float x) const noexcept
{
    const float a = x < 0.0f ? -x : x;
    if (! (a > threshold_))
        return x;
    float u = (a - threshold_) * scale_;
    u = u < static_cast<float>(kTable) ? u : static_cast<float>(kTable);
    int i = static_cast<int>(u);
    i = i < kTable - 1 ? i : kTable - 1;
    const float f = u - static_cast<float>(i);
    const float t0 = table_[i], t1 = table_[i + 1];
    float y = t0 + f * (t1 - t0);
    y = y < ceiling_ ? y : ceiling_;
    return x < 0.0f ? -y : y;
}

void SoftLimiter::processChannel(float* x, int n) const noexcept
{
    if (simd::peakAbs(x, n) <= threshold_)
        return; // transparent below −3 dBFS: nothing to do (the usual block)
#if APP_MIXER_HAVE_NEON
    const float32x4_t th = vdupq_n_f32(threshold_), sc = vdupq_n_f32(scale_), ceil = vdupq_n_f32(ceiling_);
    const float32x4_t top = vdupq_n_f32(static_cast<float>(kTable)), zero = vdupq_n_f32(0.0f);
    const uint32x4_t last = vdupq_n_u32(kTable - 1), sign = vdupq_n_u32(0x80000000u);
    const int body = n & ~3;
    for (int i = 0; i < body; i += 4)
    {
        const float32x4_t v = vld1q_f32(x + i);
        const float32x4_t a = vabsq_f32(v);
        const uint32x4_t over = vcgtq_f32(a, th);
        const uint32x2_t any = vpmax_u32(vget_low_u32(over), vget_high_u32(over));
        if (vget_lane_u32(vpmax_u32(any, any), 0) == 0)
            continue;
        float32x4_t u = vmaxq_f32(vmulq_f32(vsubq_f32(a, th), sc), zero);
        u = vminq_f32(u, top);
        const uint32x4_t iu = vminq_u32(vcvtq_u32_f32(u), last);
        const float32x4_t f = vsubq_f32(u, vcvtq_f32_u32(iu));
        alignas(16) std::uint32_t idx[4];
        vst1q_u32(idx, iu);
        alignas(16) float t0[4], t1[4];
        for (int k = 0; k < 4; ++k)
        {
            t0[k] = table_[idx[k]];
            t1[k] = table_[idx[k] + 1];
        }
        const float32x4_t a0 = vld1q_f32(t0), a1 = vld1q_f32(t1);
        float32x4_t y = vaddq_f32(a0, vmulq_f32(f, vsubq_f32(a1, a0)));
        y = vminq_f32(y, ceil);
        const uint32x4_t yb = vorrq_u32(vreinterpretq_u32_f32(y), vandq_u32(vreinterpretq_u32_f32(v), sign));
        vst1q_f32(x + i, vbslq_f32(over, vreinterpretq_f32_u32(yb), v));
    }
    for (int i = body; i < n; ++i)
        x[i] = shape(x[i]);
#else
    for (int i = 0; i < n; ++i)
        x[i] = shape(x[i]);
#endif
}

void SoftLimiter::process(float* l, float* r, int n) noexcept
{
    processChannel(l, n);
    processChannel(r, n);
}

float outputTrimGain(int index) noexcept
{
    static const std::array<float, 1024> table = [] {
        std::array<float, 1024> t {};
        for (int i = 1; i < 1024; ++i)
            t[static_cast<std::size_t>(i)] =
                static_cast<float>(std::pow(10.0, (-60.0 + (i - 1) * (60.0 / 1022.0)) / 20.0));
        return t;
    }();
    return table[static_cast<std::size_t>(clampi(index, 0, 1023))];
}

// ---- Mixer ----

Mixer::Mixer() noexcept
{
    (void) core::GainLaw::gain(0); // build the lazy tables here (message thread), never first on the audio thread
    (void) outputTrimGain(0);
    for (int t = 0; t < kTracks; ++t)
        retarget(t, 0);
    updateGates(0);
    prepare(44100.0, kN);
}

void Mixer::prepare(double, int)
{
    // Every ramp lands on its target: the mixer starts settled at its parameters (our choice: no start-up ramps —
    // the research model's smoothers start at 1.0 and its mute fades in over the first 128 samples).
    for (int t = 0; t < kTracks; ++t)
    {
        for (auto& r : ramps_[static_cast<std::size_t>(t)])
            r.reset(r.target());
        mute_[static_cast<std::size_t>(t)].reset(((audible_ >> t) & 1u) == 0);
    }
    busyRamps_ = 0;
    hardCut_ = 0;
    returnRamp_.reset(faderGain(global(GlobalParam::SendReturnVolume)));
    masterRamp_.reset(faderGain(global(GlobalParam::MasterVolume)));
    trimRamp_.reset(outputTrimGain(global(GlobalParam::OutputTrim)));
    return_.clear();
    returnActive_ = false;
    meters_ = {};
}

const StripParams& Mixer::strip(int scene, int track) const noexcept
{
    return params_[static_cast<std::size_t>(clampi(scene, 0, kScenes - 1))]
                  [static_cast<std::size_t>(clampi(track, 0, kTracks - 1))];
}

StripParams Mixer::effective(int track) const noexcept
{
    StripParams p = params_[static_cast<std::size_t>(scene_)][static_cast<std::size_t>(track)];
    const LockState& l = locks_[static_cast<std::size_t>(track)];
    if (l.mask & 1u)
        p.volume = static_cast<std::uint8_t>(clampi(roundIndex(l.volume), 0, 127));
    if (l.mask & 2u)
        p.pan = static_cast<std::int8_t>(clampi(roundIndex(l.pan), -64, 64));
    if (l.mask & 4u)
        p.send = static_cast<std::uint8_t>(clampi(roundIndex(l.send), 0, 127));
    if (l.mask & 8u)
        p.mute = l.mute;
    // Modulation on top of the stored or locked value: an override replaces it, the offsets add, one clamp.
    const auto ti = static_cast<std::size_t>(track);
    const auto& m = mod_[ti];
    const auto& o = override_[ti];
    const std::uint8_t ov = overridden_[ti];
    const auto modded = [&](int w, int base, int lo, int hi) noexcept {
        const auto wi = static_cast<std::size_t>(w);
        if ((ov >> w) & 1u)
            return clampi(roundIndex(o[wi] + m[wi]), lo, hi);
        return m[wi] != 0.0f ? clampi(base + roundIndex(m[wi]), lo, hi) : base;
    };
    p.volume = static_cast<std::uint8_t>(modded(0, p.volume, 0, 127));
    p.pan = static_cast<std::int8_t>(modded(1, p.pan, -64, 64));
    p.send = static_cast<std::uint8_t>(modded(2, p.send, 0, 127));
    return p;
}

void Mixer::setModulation(int track, StripMod which, float offset, bool hasOverride, float override) noexcept
{
    const auto w = static_cast<std::size_t>(which);
    if (track < 0 || track >= kTracks || w >= 3)
        return;
    const auto ti = static_cast<std::size_t>(track);
    const auto bit = static_cast<std::uint8_t>(1u << w);
    const bool had = (overridden_[ti] & bit) != 0;
    float& m = mod_[ti][w];
    float& o = override_[ti][w];
    if (m == offset && had == hasOverride && (! hasOverride || o == override))
        return;
    m = offset;
    o = hasOverride ? override : 0.0f;
    overridden_[ti] = static_cast<std::uint8_t>(hasOverride ? overridden_[ti] | bit : overridden_[ti] & ~bit);
    retarget(track, 0);
}

void Mixer::retarget(int track, int frame) noexcept
{
    const StripParams p = effective(track);
    float gl = 1.0f, gr = 1.0f;
    panGains(p.pan, gl, gr);
    auto& r = ramps_[static_cast<std::size_t>(track)];
    r[kernels::PanL].setTarget(gl, frame);
    r[kernels::PanR].setTarget(gr, frame);
    r[kernels::Send].setTarget(sendGain(p.send), frame);
    r[kernels::Fader].setTarget(faderGain(p.volume), frame);
    for (const auto& lane : r)
        if (! lane.idle())
            busyRamps_ |= 1u << track;
}

void Mixer::updateGates(int frame) noexcept
{
    // MIX-003 / SCN-012 (OQ-11): a track feeds the buses only if it is muted at neither the track nor the pattern level
    // and, when any solo is set at either level, it is soloed at either level. Changes go through the mute ramp.
    std::uint16_t mute = patternMute_, solo = patternSolo_, baseMute = patternMute_, lockMute = 0, steps = 0;
    for (int t = 0; t < kTracks; ++t)
    {
        const StripParams p = effective(t);
        const auto& own = params_[static_cast<std::size_t>(scene_)][static_cast<std::size_t>(t)];
        mute = static_cast<std::uint16_t>(mute | (p.mute ? 1u << t : 0u));
        solo = static_cast<std::uint16_t>(solo | (p.solo ? 1u << t : 0u));
        baseMute = static_cast<std::uint16_t>(baseMute | (own.mute ? 1u << t : 0u));
        steps = static_cast<std::uint16_t>(steps | (own.stopSteps ? 1u << t : 0u));
        if (p.mute && ! own.mute)
            lockMute = static_cast<std::uint16_t>(lockMute | 1u << t); // a step's MUTE lock (it silences its step)
    }
    const std::uint16_t soloMask = solo != 0 ? solo : 0xFFFFu;
    // A track whose MUTE MODE is STOP STEPS keeps sounding when muted / unsoloed (its ringing notes, live hits) and the
    // router skips its sequencer steps instead (the track's own and the pattern's mutes, solo — not a step's MUTE lock,
    // which still silences its own step). The other tracks (STOP SOUND) are gated here.
    stepMuted_ = static_cast<std::uint16_t>(steps & ~(~baseMute & soloMask));
    const auto audible = static_cast<std::uint16_t>((~mute & soloMask & ~steps) | (~lockMute & steps));
    std::uint32_t changed = static_cast<std::uint16_t>(audible ^ audible_);
    for (std::uint32_t t = 0; changed != 0; ++t, changed >>= 1)
        if (changed & 1u)
            mute_[t].set(((audible >> t) & 1u) == 0, frame);
    audible_ = audible;
}

void Mixer::swapStrips(int scene, int a, int b, bool withSequences) noexcept
{
    std::swap(params_[scene][a], params_[scene][b]);
    if (scene != scene_)
        return;
    std::swap(locks_[a], locks_[b]);
    std::swap(mod_[a], mod_[b]);
    std::swap(override_[a], override_[b]);
    std::swap(overridden_[a], overridden_[b]);
    std::swap(ramps_[a], ramps_[b]);
    std::swap(mute_[a], mute_[b]);
    const auto swapBits = [a, b](auto& mask)
    {
        const auto x = ((static_cast<unsigned>(mask) >> a) ^ (static_cast<unsigned>(mask) >> b)) & 1u;
        mask ^= static_cast<std::remove_reference_t<decltype(mask)>>((x << a) | (x << b));
    };
    swapBits(busyRamps_);
    swapBits(audible_);
    swapBits(hardCut_);
    if (withSequences)
    {
        swapBits(patternMute_);
        swapBits(patternSolo_);
    }
    updateGates(0);
}

void Mixer::setStrip(int scene, int track, const StripParams& p, int frame) noexcept
{
    if (scene < 0 || scene >= kScenes || track < 0 || track >= kTracks)
        return;
    params_[static_cast<std::size_t>(scene)][static_cast<std::size_t>(track)] = sanitise(p);
    if (scene != scene_)
        return;
    retarget(track, frame);
    updateGates(frame);
}

void Mixer::setGlobal(GlobalParam p, float value, int frame) noexcept
{
    const auto i = static_cast<std::size_t>(p);
    if (i >= globals_.size())
        return;
    const int v = roundIndex(value);
    switch (p)
    {
        case GlobalParam::MasterVolume:
            globals_[i] = clampi(v, 0, 127);
            masterRamp_.setTarget(faderGain(globals_[i]), frame);
            break;
        case GlobalParam::SendReturnVolume:
            globals_[i] = clampi(v, 0, 127);
            returnRamp_.setTarget(faderGain(globals_[i]), frame);
            break;
        case GlobalParam::HeadphoneVolume: globals_[i] = clampi(v, 0, 127); break;
        case GlobalParam::SoftLimiter:
        case GlobalParam::MasterLimiter: globals_[i] = value >= 0.5f ? 1 : 0; break;
        case GlobalParam::OutputTrim:
            globals_[i] = clampi(v, 0, 1023);
            trimRamp_.setTarget(outputTrimGain(globals_[i]), frame);
            break;
    }
}

void Mixer::setPatternMuteSolo(std::uint16_t mute, std::uint16_t solo, int frame) noexcept
{
    patternMute_ = mute;
    patternSolo_ = solo;
    updateGates(frame);
}

void Mixer::setScene(int scene, int frame) noexcept
{
    if (scene < 0 || scene >= kScenes || scene == scene_)
        return;
    scene_ = scene;
    for (int t = 0; t < kTracks; ++t)
        retarget(t, frame);
    updateGates(frame);
}

void Mixer::applyLockValues(int track, std::uint8_t mask, std::uint8_t hold, const float* values, int frame) noexcept
{
    LockState& l = locks_[static_cast<std::size_t>(track)];
    for (int k = 0; k < 4; ++k)
    {
        const auto bit = static_cast<std::uint8_t>(1u << k);
        if (mask & bit)
        {
            switch (k)
            {
                case 0: l.volume = values[0]; break;
                case 1: l.pan = values[1]; break;
                case 2: l.send = values[2]; break;
                default: l.mute = values[3] >= 0.5f; break;
            }
            l.mask = static_cast<std::uint8_t>(l.mask | bit);
            l.hold = static_cast<std::uint8_t>((l.hold & ~bit) | (hold & bit));
        }
        else if ((l.hold & bit) == 0)
            l.mask = static_cast<std::uint8_t>(l.mask & ~bit); // Revert: back to the strip's own value
    }
    retarget(track, frame);
    updateGates(frame);
}

void Mixer::hardMute(int track) noexcept
{
    if (track < 0 || track >= kTracks)
        return;
    mute_[static_cast<std::size_t>(track)].cut();
    hardCut_ = static_cast<std::uint16_t>(hardCut_ | 1u << track);
}

void Mixer::finishLanes(int track, const float* acc) noexcept
{
    auto& r = ramps_[static_cast<std::size_t>(track)];
    bool busy = false;
    for (int k = 0; k < kernels::kLanes; ++k)
    {
        r[static_cast<std::size_t>(k)].finishBlock(acc[k], kN);
        busy = busy || ! r[static_cast<std::size_t>(k)].idle();
    }
    if (! busy)
        busyRamps_ &= ~(1u << track);
}

void Mixer::processTracks(stages::BlockBuffers& b, const Hooks& h) noexcept
{
    b.mainBus.clear();
    sendUsed_ = false; // the send bus is cleared by its first tap (or by processSend): most blocks have no send
    stripOut_ = 0;
    std::memset(&meters_, 0, sizeof meters_);
    for (int t = 0; t < kTracks; ++t)
    {
        const auto ti = static_cast<std::size_t>(t);
        auto& blk = b.track[ti];
        auto& r = ramps_[ti];
        const bool active = ((b.activeTracks >> t) & 1u) != 0;
        const bool busy = ((busyRamps_ >> t) & 1u) != 0;
        const MuteRamp::Kind mk = mute_[ti].fill(active ? muteGain_ : nullptr, kN);

        kernels::Ramp4 lanes;
        alignas(16) float acc[kernels::kLanes];
        if (busy)
            for (int k = 0; k < kernels::kLanes; ++k)
            {
                const auto& g = r[static_cast<std::size_t>(k)];
                lanes.cur[k] = g.current();
                lanes.step[k] = g.step();
                lanes.target[k] = g.target();
                lanes.start[k] = g.startIn(kN);
                lanes.end[k] = g.endIn(kN);
            }

        if (! active || mk == MuteRamp::Kind::Closed)
        {
            // Silent or fully muted: nothing reaches the buses, the ramps advance exactly as if they had run.
            if (busy)
            {
                kernels::rampFill(lanes, gains_, acc, kN);
                finishLanes(t, acc);
            }
            continue;
        }
        if (h.insert[ti] != nullptr)
            h.insert[ti]->process(blk.l, blk.r);

        kernels::StripArgs a;
        a.l = blk.l;
        a.r = blk.r;
        a.mainL = b.mainBus.l;
        a.mainR = b.mainBus.r;
        a.mute = mk == MuteRamp::Kind::Buffer ? muteGain_ : nullptr;
        alignas(16) float constant[kernels::kLanes];
        const auto& sendLane = r[kernels::Send];
        const bool sendOn = sendLane.current() != 0.0f || sendLane.target() != 0.0f;
        if (sendOn)
        {
            if (! sendUsed_)
                b.sendBus.clear();
            sendUsed_ = true;
            a.sendL = b.sendBus.l;
            a.sendR = b.sendBus.r;
        }
        kernels::StereoMeter m;
        if (busy)
        {
            kernels::rampFill(lanes, gains_, acc, kN);
            finishLanes(t, acc);
            a.gains = gains_;
            a.rampGains = true;
            m = kernels::strip(a, kN);
        }
        else
        {
            for (int k = 0; k < kernels::kLanes; ++k)
                constant[k] = r[static_cast<std::size_t>(k)].current();
            a.gains = constant;
            if (a.mute == nullptr && ! sendOn && constant[kernels::PanL] == 1.0f && constant[kernels::PanR] == 1.0f &&
                constant[kernels::Fader] == 1.0f)
                m = kernels::unity(blk.l, blk.r, b.mainBus.l, b.mainBus.r, kN); // NFR-030: bit-transparent
            else
                m = kernels::strip(a, kN);
        }
        storeMeter(meters_, t, m);
        stripOut_ = static_cast<std::uint16_t>(stripOut_ | 1u << t); // the assign feed is valid
    }
    // A hard-muted track that is still audible fades back in from the next block.
    for (std::uint32_t cut = hardCut_, t = 0; cut != 0; ++t, cut >>= 1)
        if ((cut & 1u) != 0 && ((audible_ >> t) & 1u) != 0)
            mute_[t].set(false, 0);
    hardCut_ = 0;
}

void Mixer::processSend(stages::BlockBuffers& b, const Hooks& h) noexcept
{
    if (h.sendFx == nullptr)
    {
        // Send FX Off: the return is silence (audio-engine.md "Buses"; there is no dry path around the send FX).
        returnRamp_.fill(nullptr, kN);
        returnActive_ = false;
        b.sendActive = false;
        return;
    }
    if (! sendUsed_)
        b.sendBus.clear();
    sendUsed_ = true;
    h.sendFx->process(b.sendBus.l, b.sendBus.r);
    applyGain(returnRamp_, busGain_, return_.l, return_.r, b.sendBus.l, b.sendBus.r);
    returnActive_ = true;
    b.sendActive = true;
    storeMeter(meters_, kMeterSend, kernels::meter(return_.l, return_.r, kN));
}

void Mixer::processMaster(stages::BlockBuffers& b, const Hooks& h) noexcept
{
    if (h.masterFx != nullptr)
        h.masterFx->process(b.mainBus.l, b.mainBus.r);
    auto& m = b.master;
    std::memcpy(&m, &b.mainBus, sizeof(stages::StereoBlock));
    if (returnActive_) // the send return is added after the master FX (F106)
    {
        simd::add(m.l, return_.l, kN);
        simd::add(m.r, return_.r, kN);
    }
    if (monitorL_ != nullptr && monitorR_ != nullptr) // + input monitor, before the master fader
    {
        simd::add(m.l, monitorL_, kN);
        simd::add(m.r, monitorR_, kN);
    }
    applyGain(masterRamp_, busGain_, m.l, m.r, m.l, m.r);
    if (b.auxActive) // post-master additions (preview, metronome)
    {
        simd::add(m.l, b.aux.l, kN);
        simd::add(m.r, b.aux.r, kN);
    }
    if (globals_[static_cast<std::size_t>(GlobalParam::MasterLimiter)] != 0)
        limiter_.process(m.l, m.r, kN);
    storeMeter(meters_, kMeterMaster, kernels::meter(m.l, m.r, kN));
    if (trimInMaster_) // MIX-060 output trim (main); otherwise the output stage trims every output
        applyGain(trimRamp_, busGain2_, m.l, m.r, m.l, m.r);
    else
        trimRamp_.fill(nullptr, kN); // keep the ramp's state (global() / a later setTrimInMaster(true))
}
} // namespace engine::mixer
