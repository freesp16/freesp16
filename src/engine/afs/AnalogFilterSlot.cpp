#include "engine/afs/AnalogFilterSlot.h"

#include "engine/fx/dsp/Vec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::afs
{
namespace dsp = fx::dsp::afs;

double envDecayMs(int index127) noexcept
{
    const int i = std::clamp(index127, 0, 127);
    return 10.0 * std::pow(400.0, static_cast<double>(i) / 127.0);
}

// ================================================================================================== Model ====

double ModelProcessor::cutoffHz(double pos1023) noexcept
{
    return 20.0 * std::pow(1000.0, pos1023 / 1023.0);
}

double ModelProcessor::hpCutoffHz(int index1023) noexcept
{
    if (index1023 <= 0)
        return 0.0;
    return 20.0 * std::pow(100.0, static_cast<double>(std::min(index1023, 1023) - 1) / 1022.0);
}

float ModelProcessor::feedback(float r) noexcept
{
    r = std::clamp(r, 0.0f, 1.0f);
    return r <= 0.95f ? 4.0f * r / 0.95f : 4.0f + (r - 0.95f) * (0.6f / 0.05f);
}

void ModelProcessor::prepare(double sampleRate, int)
{
    fs_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    const double fs2 = 2.0 * fs_;
    for (int j = 0; j <= kTable; ++j)
    {
        const double fc = std::min(cutoffHz(static_cast<double>(j)), 0.45 * fs2);
        const double g = std::tan(3.14159265358979323846 * fc / fs2);
        table_[j] = static_cast<float>(g / (1.0 + g));
    }
    smooth_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.001 * fs2))); // 1 ms one-poles on cutoff and feedback
    envAttack_ = static_cast<float>(1.0 / (0.002 * fs2));                // 2 ms linear attack
    // −120 dBFS seed (xorshift32, fixed seed): starts self-oscillation from silence like the circuit's noise, and
    // keeps the ladder's states out of the denormal range.
    std::uint32_t x = 0x5EED0AF5u;
    for (float& n : noise_)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        n = (static_cast<float>(x >> 8) * (1.0f / 16777216.0f) - 0.5f) * 2.0e-6f;
    }
    reset();
}

void ModelProcessor::reset() noexcept
{
    hb_.clear();
    ladder_.clear();
    hp_.clear();
    hpPos_ = -1.0f;
    hpSet_ = -2.0f;
    env_ = 0.0f;
    attacking_ = false;
    primed_ = false;
    noisePos_ = 0;
}

void ModelProcessor::coefficients(const AfsControl& c) noexcept
{
    constexpr float kPerOctave = 102.65133f; // 1023 / log2(1000): cutoff positions per octave
    const float knob = std::clamp(c.lpCutoff01, 0.0f, 1.0f) * 1023.0f;
    const float kT = feedback(c.resonance01);
    const auto target = [&](float env) {
        const float p = knob + (c.envAmount * env + c.lfo) * kPerOctave;
        return p < 0.0f ? 0.0f : (p > 1023.0f ? 1023.0f : p);
    };
    if (! primed_)
    {
        pos_ = target(0.0f);
        kSm_ = kT;
        primed_ = true;
    }
    const float decay = static_cast<float>(std::exp(-6.907755278982137 / (static_cast<double>(c.envDecayMs) * 0.002 * fs_))); // −60 dB
    const int trigAt = c.trig != 0 ? 2 * (c.trig - 1) : -1;

    // Fast path: no envelope, the smoothers settled — one coefficient set for the whole block.
    if (trigAt < 0 && ! attacking_ && env_ == 0.0f)
    {
        const float p = target(0.0f);
        if (std::fabs(p - pos_) < 1.0e-3f && std::fabs(kT - kSm_) < 1.0e-6f)
        {
            pos_ = p;
            kSm_ = kT;
            const int idx = static_cast<int>(pos_);
            const float fr = pos_ - static_cast<float>(idx);
            const float G = table_[idx] + fr * (table_[idx + 1] - table_[idx]);
            const float G2 = G * G, G3 = G2 * G, g4 = G2 * G2;
            const float inv = 1.0f / (1.0f + kSm_ * g4);
            const float kio = (kSm_ * inv) * (1.0f - G);
            for (int i = 0; i < dsp::kOver; ++i)
            {
                coefs_.g[i] = G;
                coefs_.g2[i] = G2;
                coefs_.g3[i] = G3;
                coefs_.inv[i] = inv;
                coefs_.kio[i] = kio;
            }
            return;
        }
    }
    for (int i = 0; i < dsp::kOver; ++i)
    {
        if (i == trigAt)
            attacking_ = true;
        if (attacking_)
        {
            env_ += envAttack_;
            if (env_ >= 1.0f)
            {
                env_ = 1.0f;
                attacking_ = false;
            }
        }
        else if (env_ != 0.0f)
        {
            env_ *= decay;
            if (env_ < 1.0e-6f)
                env_ = 0.0f;
        }
        pos_ += (target(env_) - pos_) * smooth_;
        kSm_ += (kT - kSm_) * smooth_;
        const float p = pos_ < 0.0f ? 0.0f : (pos_ > 1023.0f ? 1023.0f : pos_);
        const int idx = static_cast<int>(p);
        const float fr = p - static_cast<float>(idx);
        const float G = table_[idx] + fr * (table_[idx + 1] - table_[idx]);
        const float G2 = G * G, G3 = G2 * G, g4 = G2 * G2;
        const float inv = 1.0f / (1.0f + kSm_ * g4);
        coefs_.g[i] = G;
        coefs_.g2[i] = G2;
        coefs_.g3[i] = G3;
        coefs_.inv[i] = inv;
        coefs_.kio[i] = (kSm_ * inv) * (1.0f - G);
    }
}

void ModelProcessor::process(float* l, float* r, int frames, const AfsControl& c) noexcept
{
    if (frames != dsp::kBlock)
        return;
    coefficients(c);
    const float* noise = noise_ + static_cast<std::size_t>(noisePos_) * 2 * dsp::kOver;
    noisePos_ = (noisePos_ + 1) & 7;

    // High-pass target (block rate): index 0 = off; otherwise a block-rate one-pole on the index (≈ 3.5 ms).
    const float hpIndex = std::round(std::clamp(c.hpCutoff01, 0.0f, 1.0f) * 1023.0f);
    bool hpOn = hpIndex >= 1.0f;
    if (! hpOn)
    {
        if (hpPos_ >= 0.0f)
            hp_.clear();
        hpPos_ = -1.0f;
        hpSet_ = -2.0f;
    }
    else
    {
        hpPos_ = hpPos_ < 0.0f ? hpIndex : hpPos_ + (hpIndex - hpPos_) * 0.25f;
        if (std::fabs(hpPos_ - hpSet_) > 0.01f)
        {
            hp_.setCutoff(20.0 * std::pow(100.0, (static_cast<double>(hpPos_) - 1.0) / 1022.0), fs_);
            hpSet_ = hpPos_;
        }
    }

#if APP_FX_HAVE_NEON
    if (! scalar_)
    {
        dsp::neon::upsample(hb_, l, r, over_);
        dsp::neon::ladder(ladder_, coefs_, over_, noise, ladderOut_, c.twoPole);
        dsp::neon::downsample(hb_, ladderOut_, l, r);
        if (hpOn)
            dsp::neon::highPass(hp_, l, r, dsp::kBlock);
        return;
    }
#endif
    dsp::scalar::upsample(hb_, l, r, over_);
    dsp::scalar::ladder(ladder_, coefs_, over_, noise, ladderOut_, c.twoPole);
    dsp::scalar::downsample(hb_, ladderOut_, l, r);
    if (hpOn)
        dsp::scalar::highPass(hp_, l, r, dsp::kBlock);
}

// =============================================================================================== External ====

namespace
{
struct Click
{
    float v[ExternalProcessor::kClickLength];
    Click() noexcept // −20 dBFS Hann burst (peak 0.1)
    {
        for (int j = 0; j < ExternalProcessor::kClickLength; ++j)
            v[j] = static_cast<float>(0.1 * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (j + 1) /
                                                                 (ExternalProcessor::kClickLength + 1))));
    }
};
const Click kClick;
} // namespace

const float* ExternalProcessor::click() noexcept
{
    return kClick.v;
}

void ExternalProcessor::prepare(double, int)
{
    reset();
    sendRamp_ = kSendFade; // prepare() settles: no fade-in
}

void ExternalProcessor::reset() noexcept
{
    send_.clear();
    sendRamp_ = 0; // the send fades in over 128 samples: the return then starts from silence, whatever its latency
    if (ping_ != Ping::Idle)
        failPing(); // a route / processor change interrupted the measurement
}

void ExternalProcessor::startPing() noexcept
{
    ping_ = Ping::Flush; // the loop first empties of program (≤ 2048 frames of latency), then the click
    flushed_ = 0;
    captured_ = 0;
    clickSent_ = false;
}

void ExternalProcessor::failPing() noexcept
{
    ping_ = Ping::Idle;
    latency_.store(-1, std::memory_order_release);
    pings_.store(pings_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void ExternalProcessor::process(float* l, float* r, int frames, const AfsControl&) noexcept
{
    if (frames != kBlockFrames)
        return;
    const auto bytes = sizeof(float) * kBlockFrames;
    if (ping_ == Ping::Flush || ping_ == Ping::Capture)
    {
        send_.clear(); // the program is muted on the send while the loop empties and the click travels
        if (ping_ == Ping::Capture && ! clickSent_)
        {
            std::memcpy(send_.l, kClick.v, sizeof kClick.v);
            std::memcpy(send_.r, kClick.v, sizeof kClick.v);
            clickSent_ = true;
        }
    }
    else if (sendRamp_ < kSendFade)
    {
        for (int i = 0; i < kBlockFrames; ++i)
        {
            if (sendRamp_ < kSendFade)
                ++sendRamp_;
            const float g = static_cast<float>(sendRamp_) * (1.0f / kSendFade);
            send_.l[i] = l[i] * g;
            send_.r[i] = r[i] * g;
        }
    }
    else
    {
        std::memcpy(send_.l, l, bytes);
        std::memcpy(send_.r, r, bytes);
    }
    if (retL_ != nullptr)
    {
        std::memcpy(l, retL_, bytes);
        std::memcpy(r, retR_, bytes);
    }
    else
    {
        std::memset(l, 0, bytes);
        std::memset(r, 0, bytes);
    }
    advanceIdle();
}

void ExternalProcessor::advanceIdle() noexcept
{
    // Once per block: called by process() and, for a block without it, by the slot's endBlock(); setReturn() re-arms
    // it at every block start.
    if (ticked_)
        return;
    ticked_ = true;
    switch (ping_)
    {
        case Ping::Idle: break;
        case Ping::Flush:
            flushed_ += kBlockFrames;
            if (flushed_ >= kFlushFrames)
                ping_ = Ping::Capture; // the click goes out with the next processed block
            break;
        case Ping::Capture:
            if (! clickSent_)
                break;
            if (captured_ + kBlockFrames <= kPingFrames)
            {
                if (retL_ != nullptr)
                    std::memcpy(capture_ + captured_, retL_, sizeof(float) * kBlockFrames);
                else
                    std::memset(capture_ + captured_, 0, sizeof(float) * kBlockFrames);
                captured_ += kBlockFrames;
            }
            if (captured_ >= kPingFrames)
            {
                ping_ = Ping::Analyse;
                nextLag_ = 0;
                best_ = 0.0f;
                bestLag_ = -1;
            }
            break;
        case Ping::Analyse:
        {
            const int last = kPingFrames - kClickLength; // inclusive
            const int to = std::min(nextLag_ + kLagsPerBlock, last + 1);
            correlate(capture_, kPingFrames, kClick.v, kClickLength, nextLag_, to, best_, bestLag_);
            nextLag_ = to;
            if (nextLag_ > last)
                finishAnalysis();
            break;
        }
    }
}

int ExternalProcessor::correlate(const float* captured, int n, const float* clk, int clickLen, int fromLag, int toLag,
                                 float& bestValue, int& bestLag) noexcept
{
    for (int lag = fromLag; lag < toLag && lag + clickLen <= n; ++lag)
    {
        float acc = 0.0f;
        for (int j = 0; j < clickLen; ++j)
            acc += captured[lag + j] * clk[j];
        const float a = std::fabs(acc);
        if (a > bestValue)
        {
            bestValue = a;
            bestLag = lag;
        }
    }
    return bestLag;
}

void ExternalProcessor::finishAnalysis() noexcept
{
    float energy = 0.0f;
    for (float v : kClick.v)
        energy += v * v;
    const int result = best_ >= 1.0e-3f * energy && bestLag_ >= 0 ? bestLag_ : -1; // nothing above −60 dB: failed
    ping_ = Ping::Idle;
    latency_.store(result, std::memory_order_release);
    pings_.store(pings_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

// =================================================================================================== Slot ====

AnalogFilterSlot::AnalogFilterSlot() noexcept
{
    for (int t = 0; t < kTracks; ++t)
    {
        hooks_[static_cast<std::size_t>(t)].slot = this;
        hooks_[static_cast<std::size_t>(t)].track = t;
    }
    std::fill(std::begin(dipGain_), std::end(dipGain_), 1.0f);
}

void AnalogFilterSlot::prepare(double sampleRate, int maxBlock)
{
    fs_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    for (auto& m : models_)
        m.prepare(fs_, maxBlock);
    live_ = 0;
    warmBlock_ = warmed_ = false;
    external_.prepare(fs_, maxBlock);
    overHoldSamples_ = static_cast<int>(0.5 * fs_ + 0.5);
    // The configuration settles at once (no dip across a prepare), the drive at its target.
    cur_ = target_ = wanted();
    phase_ = Dip::None;
    count_ = kDipSamples;
    dipPaths_ = 0;
    dipBlock_ = false;
    std::fill(std::begin(dipGain_), std::end(dipGain_), 1.0f);
    driveCur_ = driveTarget_ = driveGain(drive_);
    driveLeft_ = 0;
    overPeak_ = 0.0f;
    overHold_ = 0;
    overLevel_.store(0.0f, std::memory_order_relaxed);
    for (auto& b : ret_)
        b.clear();
    retSeen_ = retValid_ = false;
    retBlockL_ = retBlockR_ = nullptr;
    sent_ = visited_ = false;
}

void AnalogFilterSlot::bindParams(const std::function<core::ParamId(std::string_view)>& resolve) noexcept // rt-ok: set-up only
{
    for (int f = 0; f < kParams; ++f)
        ids_[static_cast<std::size_t>(f)] = resolve ? resolve(kAfsParamIds[f]) : core::ParamId {};
}

bool AnalogFilterSlot::onParam(const ParamChange& c) noexcept
{
    if (! c.param.valid())
        return false;
    for (int f = 0; f < kParams; ++f)
        if (ids_[static_cast<std::size_t>(f)].valid() && ids_[static_cast<std::size_t>(f)] == c.param)
        {
            setField(static_cast<AfsField>(f), c.value);
            return true;
        }
    return false;
}

void AnalogFilterSlot::setField(AfsField f, float v) noexcept
{
    const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
    switch (f)
    {
        case AfsField::Active:
            active_ = v >= 0.5f;
            request();
            break;
        case AfsField::InsertTrack:
            insertTrack_ = std::clamp(i, 0, kTracks);
            request();
            break;
        case AfsField::InsertOrder:
            insertOrder_ = std::clamp(i, 0, 1);
            request();
            break;
        case AfsField::Processor:
            processor_ = std::clamp(i, 0, 1);
            request();
            break;
        case AfsField::Drive:
        {
            drive_ = std::clamp(i, 0, 1023);
            control_.drive01 = static_cast<float>(drive_) / 1023.0f;
            driveTarget_ = driveGain(drive_);
            driveStep_ = (driveTarget_ - driveCur_) / 100.0f; // F107: the 100-sample linear ramp
            driveLeft_ = 100;
            break;
        }
        case AfsField::LpCutoff: control_.lpCutoff01 = static_cast<float>(std::clamp(i, 0, 1023)) / 1023.0f; break;
        case AfsField::Resonance: control_.resonance01 = static_cast<float>(std::clamp(i, 0, 1023)) / 1023.0f; break;
        case AfsField::HpCutoff: control_.hpCutoff01 = static_cast<float>(std::clamp(i, 0, 1023)) / 1023.0f; break;
        case AfsField::Slope: control_.twoPole = i != 0; break;
        case AfsField::EnvAmount: envAmount_ = static_cast<float>(std::clamp(i, -100, 100)) / 100.0f; break;
        case AfsField::EnvDecay: envDecay_ = std::clamp(i, 0, 127); break;
        case AfsField::LfoAmount: lfoAmount_ = static_cast<float>(std::clamp(i, -100, 100)) / 100.0f; break;
        case AfsField::Ping:
            if (cur_.active && cur_.proc == Processor::External && phase_ == Dip::None)
                external_.startPing();
            else
                external_.failPing();
            break;
        case AfsField::Count: break;
    }
}

void AnalogFilterSlot::setRoute(Route route, int track) noexcept
{
    if (route == Route::Master)
        insertTrack_ = 0;
    else
    {
        insertTrack_ = std::clamp(track, 0, kTracks - 1) + 1;
        insertOrder_ = route == Route::TrackPostFx ? 1 : 0;
    }
    request();
}

void AnalogFilterSlot::setActive(bool on) noexcept
{
    active_ = on;
    request();
}

void AnalogFilterSlot::setProcessor(Processor p) noexcept
{
    processor_ = p == Processor::External ? 1 : 0;
    request();
}

AnalogFilterSlot::Config AnalogFilterSlot::wanted() const noexcept
{
    Config c;
    if (insertTrack_ == 0)
    {
        c.route = Route::Master;
        c.track = -1;
    }
    else
    {
        c.route = insertOrder_ != 0 ? Route::TrackPostFx : Route::TrackPreFx;
        c.track = static_cast<std::int8_t>(insertTrack_ - 1);
    }
    c.active = active_;
    c.proc = processor_ == 1 && externalAvailable() ? Processor::External : Processor::Model; // our fallback
    return c;
}

void AnalogFilterSlot::request() noexcept
{
    const Config w = wanted();
    if (w == target_)
        return;
    target_ = w;
    switch (phase_)
    {
        case Dip::None:
            if (w == cur_)
                return;
            phase_ = Dip::Down;
            dipPaths_ = pathBit(cur_) | pathBit(w);
            break;
        case Dip::Down: dipPaths_ |= pathBit(w); break;
        case Dip::Up:
            phase_ = Dip::Down; // from the gain reached so far
            dipPaths_ |= pathBit(cur_) | pathBit(w);
            break;
    }
}

void AnalogFilterSlot::switchConfig() noexcept
{
    const bool toModel = target_.active && target_.proc == Processor::Model;
    const bool sameModel = cur_.active && cur_.proc == Processor::Model && cur_.samePoint(target_);
    const bool toExternal = target_.active && target_.proc == Processor::External;
    const bool sameExternal = cur_.active && cur_.proc == Processor::External && cur_.samePoint(target_);
    cur_ = target_;
    if (toModel)
    {
        if (warmed_)
            live_ ^= 1; // the instance that ran the target point's signal during the last block down
        else if (! sameModel)
            models_[static_cast<std::size_t>(live_)].reset(); // its point was silent: start from silence
    }
    if (toExternal && ! sameExternal)
        external_.reset(); // the send fades in (its return then starts from silence)
    warmed_ = false;
}

void AnalogFilterSlot::beginPeriod(const AudioIO& io) noexcept
{
    constexpr int fsPort = static_cast<int>(OutPort::FilterSend);
    const bool send = io.out != nullptr && io.numOutPorts > fsPort && io.out[2 * fsPort] != nullptr &&
                      io.out[2 * fsPort + 1] != nullptr;
    const bool ret = io.in != nullptr && io.numIn >= 4 && io.in[2] != nullptr && io.in[3] != nullptr;
    retInL_ = ret ? io.in[2] : nullptr;
    retInR_ = ret ? io.in[3] : nullptr;
    if (send != sendPort_ || ret != returnPort_)
    {
        sendPort_ = send;
        returnPort_ = ret;
        request(); // the processor in use may change (External ↔ Model fallback), with the dip
    }
}

void AnalogFilterSlot::capture(int ioFrame, int blockFrame, int frames) noexcept
{
    auto& fill = ret_[static_cast<std::size_t>(retFill_)];
    const auto bytes = sizeof(float) * static_cast<std::size_t>(frames);
    if (retInL_ != nullptr)
    {
        std::memcpy(fill.l + blockFrame, retInL_ + ioFrame, bytes);
        std::memcpy(fill.r + blockFrame, retInR_ + ioFrame, bytes);
        retSeen_ = true;
    }
    else if (retSeen_)
    {
        std::memset(fill.l + blockFrame, 0, bytes);
        std::memset(fill.r + blockFrame, 0, bytes);
    }
}

void AnalogFilterSlot::beginBlock(std::span<const trigger::TriggerEvent> events, float lfoValue) noexcept
{
    // The return frames collected during the previous block become this block's return (64-frame input latency).
    if (retSeen_)
    {
        retBlockL_ = ret_[static_cast<std::size_t>(retFill_)].l;
        retBlockR_ = ret_[static_cast<std::size_t>(retFill_)].r;
        retFill_ ^= 1;
    }
    else
        retBlockL_ = retBlockR_ = nullptr;
    retSeen_ = false;
    external_.setReturn(retBlockL_, retBlockR_);

    // AFS-006: the switch at the block boundary where the gain reached 0, then this block's gain curve.
    if (phase_ == Dip::Down && count_ == 0)
    {
        switchConfig();
        phase_ = Dip::Up;
    }
    dipBlock_ = phase_ != Dip::None;
    warmBlock_ = false;
    if (dipBlock_)
    {
        for (int i = 0; i < kBlockFrames; ++i)
        {
            if (phase_ == Dip::Down)
                count_ = count_ > 0 ? count_ - 1 : 0;
            else if (count_ < kDipSamples)
                ++count_;
            dipGain_[i] = static_cast<float>(count_) * (1.0f / kDipSamples);
        }
        if (phase_ == Dip::Up && count_ == kDipSamples)
            phase_ = Dip::None; // this block's curve still applies to dipPaths_
        // The last block down: the target's model warms up on its point's signal (one extra model block per dip).
        if (phase_ == Dip::Down && count_ == 0 && target_.active && target_.proc == Processor::Model)
        {
            warmBlock_ = true;
            warmed_ = false;
            models_[static_cast<std::size_t>(live_ ^ 1)].reset();
        }
    }
    else
        dipPaths_ = 0;

    // Modulation of this block.
    std::uint8_t trig = 0;
    for (const auto& e : events)
        if (e.kind == trigger::TriggerKind::Full && (cur_.route == Route::Master || e.track == cur_.track))
        {
            trig = static_cast<std::uint8_t>(std::min<int>(e.frame, kBlockFrames - 1) + 1);
            break;
        }
    control_.trig = trig;
    control_.envAmount = envAmount_ * ModelProcessor::kEnvOctaves;
    control_.envDecayMs = static_cast<float>(envDecayMs(envDecay_));
    control_.lfo = cur_.route == Route::Master ? 0.0f : std::clamp(lfoValue, -1.0f, 1.0f) * lfoAmount_ * ModelProcessor::kLfoOctaves;
}

std::uint32_t AnalogFilterSlot::claimTrack(stages::BlockBuffers& b) noexcept
{
    if (cur_.route == Route::Master || ! cur_.active || cur_.track < 0)
        return 0;
    const std::uint32_t bit = 1u << cur_.track;
    if ((b.activeTracks & bit) != 0)
        return 0;
    b.track[static_cast<std::size_t>(cur_.track)].clear(); // a silent source: the filter (or the return) still sounds
    return bit;
}

void AnalogFilterSlot::wrapHooks(mixer::Hooks& h) noexcept
{
    std::uint32_t need = dipBlock_ ? (dipPaths_ & 0xFFFFu) : 0u;
    if (cur_.route != Route::Master && cur_.track >= 0)
        need |= 1u << cur_.track;
    for (int t = 0; t < kTracks; ++t)
        if ((need >> t) & 1u)
        {
            auto& hook = hooks_[static_cast<std::size_t>(t)];
            hook.inner = h.insert[static_cast<std::size_t>(t)];
            h.insert[static_cast<std::size_t>(t)] = &hook;
        }
}

void AnalogFilterSlot::TrackHook::process(float* l, float* r) noexcept
{
    slot->processAt(Route::TrackPreFx, track, l, r, kBlockFrames);
    if (inner != nullptr)
        inner->process(l, r);
    slot->processAt(Route::TrackPostFx, track, l, r, kBlockFrames);
}

void AnalogFilterSlot::processAt(Route route, int track, float* l, float* r, int frames) noexcept
{
    if (frames != kBlockFrames)
        return;
    if (warmBlock_ && route == target_.route && (route == Route::Master || track == target_.track))
        warm(l, r);
    const bool point = route == cur_.route && (route == Route::Master || track == cur_.track);
    if (point)
    {
        visited_ = true;
        if (cur_.active)
            run(l, r);
    }
    // ACTIVE off: the DRIVE pot still drives the main output, as on the reference (F107: dsfDriveMaster is always in
    // the main path; hardware feedback: DRIVE did nothing with the filter off). Unity at DRIVE 0, so bit-transparent.
    if (route == Route::Master && ! cur_.active)
        drive(l, r);
    if (dipBlock_)
    {
        const bool master = route == Route::Master && ((dipPaths_ >> kMasterPath) & 1u) != 0;
        const bool trackPath = route == Route::TrackPostFx && track >= 0 && ((dipPaths_ >> (track & 15)) & 1u) != 0;
        if (master || trackPath)
            applyDip(l, r);
    }
}

void AnalogFilterSlot::run(float* l, float* r) noexcept
{
    drive(l, r);
    // OVER (AFS-011): peak of the filter input, held 0.5 s.
    const float peak = std::max(fx::dsp::vec::peakAbs(l, kBlockFrames), fx::dsp::vec::peakAbs(r, kBlockFrames));
    if (peak >= overPeak_ || overHold_ <= 0)
    {
        overPeak_ = peak;
        overHold_ = overHoldSamples_;
    }
    else
        overHold_ -= kBlockFrames;

    if (cur_.proc == Processor::External)
    {
        external_.process(l, r, kBlockFrames, control_);
        sent_ = true;
    }
    else
        models_[static_cast<std::size_t>(live_)].process(l, r, kBlockFrames, control_);
}

void AnalogFilterSlot::drive(float* l, float* r) noexcept
{
    driveRan_ = true;
    // Drive (F107): the 100-sample ramp, then the gain per sample (non-fused product, NEON 4-wide).
    if (driveLeft_ > 0)
    {
        for (int i = 0; i < kBlockFrames; ++i)
        {
            if (driveLeft_ > 0)
            {
                driveCur_ += driveStep_;
                if (--driveLeft_ == 0)
                    driveCur_ = driveTarget_;
            }
            driveGain_[i] = driveCur_;
        }
        fx::dsp::vec::mulv(l, driveGain_, kBlockFrames);
        fx::dsp::vec::mulv(r, driveGain_, kBlockFrames);
    }
    else if (driveCur_ != 1.0f)
    {
        fx::dsp::vec::mul(l, driveCur_, kBlockFrames);
        fx::dsp::vec::mul(r, driveCur_, kBlockFrames);
    }
}

void AnalogFilterSlot::warm(const float* l, const float* r) noexcept
{
    std::memcpy(warm_.l, l, sizeof warm_.l);
    std::memcpy(warm_.r, r, sizeof warm_.r);
    if (driveCur_ != 1.0f) // the drive as it is now (its ramp is the live path's)
    {
        fx::dsp::vec::mul(warm_.l, driveCur_, kBlockFrames);
        fx::dsp::vec::mul(warm_.r, driveCur_, kBlockFrames);
    }
    models_[static_cast<std::size_t>(live_ ^ 1)].process(warm_.l, warm_.r, kBlockFrames, control_);
    warmed_ = true;
}

void AnalogFilterSlot::applyDip(float* l, float* r) const noexcept
{
    fx::dsp::vec::mulv(l, dipGain_, kBlockFrames);
    fx::dsp::vec::mulv(r, dipGain_, kBlockFrames);
}

void AnalogFilterSlot::routeOutputs(stages::BlockBuffers& b) noexcept
{
    constexpr int fsPort = static_cast<int>(OutPort::FilterSend);
    if (sent_)
    {
        b.port[2 * fsPort] = external_.sendL();
        b.port[2 * fsPort + 1] = external_.sendR();
    }
}

void AnalogFilterSlot::endBlock() noexcept
{
    if (! visited_ || ! cur_.active)
    {
        // The point was not processed (a silent or muted track, ACTIVE off): the drive ramp (unless the master ran it)
        // and the OVER hold keep time.
        if (driveLeft_ > 0 && ! driveRan_)
        {
            const int n = std::min(driveLeft_, kBlockFrames);
            driveCur_ += driveStep_ * static_cast<float>(n);
            driveLeft_ -= n;
            if (driveLeft_ == 0)
                driveCur_ = driveTarget_;
        }
        overHold_ -= kBlockFrames;
        if (overHold_ <= 0)
            overPeak_ = 0.0f;
    }
    external_.advanceIdle(); // once per block: a ping keeps time / captures / analyses (no-op otherwise)
    overLevel_.store(overPeak_, std::memory_order_relaxed);
    visited_ = false;
    sent_ = false;
    driveRan_ = false;
}

int AnalogFilterSlot::latencySamples() const noexcept
{
    if (! cur_.active)
        return 0;
    return cur_.proc == Processor::External ? external_.latencySamples() : fx::dsp::afs::kLatency;
}
} // namespace engine::afs
