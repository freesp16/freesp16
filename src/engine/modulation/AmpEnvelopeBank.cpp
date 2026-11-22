#include "engine/modulation/AmpEnvelopeBank.h"

#include "core/util/GainLaw.h"

#include <cstring>

namespace engine::modulation
{
namespace
{
using core::ampenv::EnvStage;
constexpr float kVelocityScale = 1.0f / (127.0f * 127.0f);
constexpr std::int32_t kAgeMax = 1 << 30; // ≈ 6.7 h at 44.1 kHz: past any finite HOLD

std::int16_t lockIndex(const trigger::LockSet* locks, int slot, EnvStage stage) noexcept
{
    float v = 0.0f;
    if (locks == nullptr || locks->values == nullptr || ! locks->get(slot, v))
        return -1;
    return static_cast<std::int16_t>(core::ampenv::clampIndex(stage, v));
}

std::int16_t lockVelocity(const trigger::LockSet* locks) noexcept
{
    float v = 0.0f;
    if (locks == nullptr || locks->values == nullptr || ! locks->get(kLockSlotVelocity, v))
        return -1;
    return static_cast<std::int16_t>(core::ampenv::clampIndex(EnvStage::Attack, v)); // 0–127
}
} // namespace

template <int N> void AmpEnvelopeLanes<N>::prepare(double sampleRate) noexcept
{
    tables_.prepare(sampleRate);
    for (int i = 0; i < 128; ++i)
        peakGain_[static_cast<std::size_t>(i)] = core::GainLaw::gain(i);
    for (int lane = 0; lane < N; ++lane)
    {
        base_[static_cast<std::size_t>(lane)] = AmpEnvParams {};
        reset(lane);
    }
}

template <int N> void AmpEnvelopeLanes<N>::setTempo(double bpm) noexcept
{
    tables_.setTempo(bpm);
}

template <int N> void AmpEnvelopeLanes<N>::setParams(int lane, const AmpEnvParams& p, double bpm) noexcept
{
    auto& base = base_[static_cast<std::size_t>(lane)];
    const bool bypassFlip = base.bypass != p.bypass;
    base = p;
    if (p.sync)
        tables_.setTempo(bpm);
    if (bypassFlip)
        bypassChanged(lane);
}

template <int N> void AmpEnvelopeLanes<N>::setThru(int lane, bool thru) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    if (thru == (stage_[i] == static_cast<std::uint8_t>(AmpEnvStage::Thru)))
        return;
    reset(lane);
    if (thru)
    {
        setConst(lane, AmpEnvStage::Thru, 1.0f, AmpEnvTables::kInfinite);
        level_[i] = 1.0f;
    }
}

template <int N> void AmpEnvelopeLanes<N>::useScalarKernels(bool scalar) noexcept
{
    k_ = scalar ? &envk::scalarKernels() : &envk::activeKernels();
}

template <int N> void AmpEnvelopeLanes<N>::reset(int lane) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    events_[i] = 0;
    gated_[i] = false;
    lockedRelease_[i] = -1;
    gateNote_[i] = false;
    age_[i] = kAgeMax;
    holdEnd_[i] = AmpEnvTables::kInfinite;
    peak_[i] = 0.0f;
    holdAfter_[i] = AmpEnvTables::kInfinite;
    level_[i] = 0.0f;
    setConst(lane, AmpEnvStage::Idle, 0.0f, AmpEnvTables::kInfinite);
}

template <int N> bool AmpEnvelopeLanes<N>::finished(int lane) const noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    return stage_[i] == static_cast<std::uint8_t>(AmpEnvStage::Idle) && events_[i] == 0;
}

// --- events ---------------------------------------------------------------------------------------------------------

template <int N> void AmpEnvelopeLanes<N>::enqueue(int lane, const Event& e) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    if (stage_[i] == static_cast<std::uint8_t>(AmpEnvStage::Thru))
        return;
    auto& q = queue_[i];
    int n = events_[i];
    if (n == kMaxEvents) // full: the newest event replaces the latest queued one
        --n;
    // Keep frame order (the router sends in order; a stable insertion makes out-of-order calls safe too).
    int at = n;
    while (at > 0 && q[static_cast<std::size_t>(at - 1)].frame > e.frame)
    {
        q[static_cast<std::size_t>(at)] = q[static_cast<std::size_t>(at - 1)];
        --at;
    }
    q[static_cast<std::size_t>(at)] = e;
    events_[i] = static_cast<std::uint8_t>(n + 1);
}

template <int N> void AmpEnvelopeLanes<N>::trigger(const trigger::TriggerEvent& t, bool gate) noexcept
{
    const int lane = t.track;
    if (lane < 0 || lane >= N)
        return;
    Event e;
    e.frame = static_cast<std::uint8_t>(t.frame < kFrames ? t.frame : kFrames - 1);
    switch (t.kind)
    {
        case trigger::TriggerKind::Release:
            e.type = EvRelease;
            break;
        case trigger::TriggerKind::Stop:
            e.type = EvKill;
            break;
        case trigger::TriggerKind::Full:
        case trigger::TriggerKind::Half:
        case trigger::TriggerKind::LockOnly:
            e.type = EvTrigger;
            e.kind = t.kind;
            e.source = t.source;
            e.gate = gate;
            e.velocity = static_cast<std::uint8_t>(t.velocity < 1 ? 1 : (t.velocity > 127 ? 127 : t.velocity));
            e.lockVelocity = lockVelocity(t.locks);
            e.lockAttack = lockIndex(t.locks, kLockSlotAttack, EnvStage::Attack);
            e.lockHold = lockIndex(t.locks, kLockSlotHold, EnvStage::Hold);
            e.lockRelease = lockIndex(t.locks, kLockSlotRelease, EnvStage::Release);
            break;
    }
    enqueue(lane, e);
}

template <int N> void AmpEnvelopeLanes<N>::release(int lane, int frame) noexcept
{
    if (lane < 0 || lane >= N)
        return;
    Event e;
    e.type = EvRelease;
    e.frame = static_cast<std::uint8_t>(frame < 0 ? 0 : (frame < kFrames ? frame : kFrames - 1));
    enqueue(lane, e);
}

template <int N> void AmpEnvelopeLanes<N>::kill(int lane, int frame) noexcept
{
    if (lane < 0 || lane >= N)
        return;
    Event e;
    e.type = EvKill;
    e.frame = static_cast<std::uint8_t>(frame < 0 ? 0 : (frame < kFrames ? frame : kFrames - 1));
    enqueue(lane, e);
}

template <int N> void AmpEnvelopeLanes<N>::applyEvent(int lane, const Event& e) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    const auto st = static_cast<AmpEnvStage>(stage_[i]);
    const bool active = st != AmpEnvStage::Idle;
    switch (e.type)
    {
        case EvKill:
            if (active && level_[i] > 0.0f)
                setLinear(lane, AmpEnvStage::Kill, level_[i], 0.0f, AmpEnvTables::kKillFrames);
            else if (active)
                reset(lane);
            return;
        case EvRelease: // GATE pad up (ENV-008); One Shot envelopes ignore it
            if (! gated_[i])
                return;
            gated_[i] = false;
            if (st == AmpEnvStage::Attack || st == AmpEnvStage::Hold)
                startRelease(lane);
            else if (st == AmpEnvStage::BypassIn || st == AmpEnvStage::Bypass)
                setLinear(lane, AmpEnvStage::BypassOut, level_[i], 0.0f, tables_.bypassRampFrames());
            return;
        default:
            break;
    }
    const auto& p = base_[i];
    bool restarts = false;
    switch (e.kind)
    {
        case trigger::TriggerKind::Full: // ENV-007: FULL always restarts; sequencer retrigger repeats only with Retrigger
            restarts = e.source != trigger::TriggerSource::Retrigger || p.retrigger || ! active;
            break;
        case trigger::TriggerKind::Half:
            restarts = active && p.retrigger;
            break; // idle track: nothing starts (SEQ-011)
        default:
            break;
    }
    if (restarts)
    {
        restart(lane, e);
        settle(lane); // attack 0 / hold 0: reach the segment that plays at this frame (a same-frame release sees it)
    }
    else if (active && e.lockRelease >= 0) // LockOnly / non-restarting HALF: the release lock still applies
        lockedRelease_[i] = e.lockRelease;
}

template <int N> void AmpEnvelopeLanes<N>::restart(int lane, const Event& e) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    const auto& p = base_[i];
    gated_[i] = gateNote_[i] = e.gate;
    lockedRelease_[i] = e.lockRelease; // locks apply at this trigger and revert at the next (ENV-010)
    age_[i] = -static_cast<std::int32_t>(e.frame); // the block's end adds kFrames
    const int velIndex = e.lockVelocity >= 0 ? e.lockVelocity : p.velocity;
    float peak = peakGain_[static_cast<std::size_t>(velIndex > 127 ? 127 : velIndex)];
    if (p.velocitySensitive) // PLY-041 / ENV-005: the ceiling scaled by (vel/127)²
        peak *= static_cast<float>(e.velocity) * static_cast<float>(e.velocity) * kVelocityScale;
    const auto hold = tables_.hold(static_cast<std::uint8_t>(e.lockHold >= 0 ? e.lockHold : p.hold), p.sync);
    holdEnd_[i] = hold;
    if (p.bypass) // the peak and HOLD are still the note's: a live bypass off returns to them
    {
        peak_[i] = peak;
        setLinear(lane, AmpEnvStage::BypassIn, level_[i], 1.0f, tables_.bypassRampFrames());
        return;
    }
    const auto attack = tables_.attack(static_cast<std::uint8_t>(e.lockAttack >= 0 ? e.lockAttack : p.attack), p.sync);
    startAttack(lane, peak, attack, hold);
}

template <int N> void AmpEnvelopeLanes<N>::bypassChanged(int lane) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    const auto st = static_cast<AmpEnvStage>(stage_[i]);
    const std::int32_t ramp = tables_.bypassRampFrames();
    if (base_[i].bypass) // on: unity while the voice plays (a released GATE note keeps fading out)
    {
        if (st == AmpEnvStage::Attack || st == AmpEnvStage::Hold ||
            (st == AmpEnvStage::Release && (gated_[i] || ! gateNote_[i])))
            setLinear(lane, AmpEnvStage::BypassIn, level_[i], 1.0f, ramp);
        return;
    }
    if (st != AmpEnvStage::BypassIn && st != AmpEnvStage::Bypass)
        return;
    const std::int32_t hold = holdEnd_[i], age = age_[i];
    if (hold != AmpEnvTables::kInfinite && age >= hold) // past its HOLD: the release, from the current level
    {
        startRelease(lane);
        return;
    }
    // Within HOLD (the attack counts as HOLD, ENV-002): the 2 ms ramp to the peak, then the rest of the hold.
    holdAfter_[i] =
        hold == AmpEnvTables::kInfinite ? AmpEnvTables::kInfinite : (hold - age > ramp ? hold - age - ramp : 0);
    setLinear(lane, AmpEnvStage::Attack, level_[i], peak_[i], ramp);
}

// --- segments -------------------------------------------------------------------------------------------------------

template <int N> void AmpEnvelopeLanes<N>::setConst(int lane, AmpEnvStage st, float value, std::int32_t frames) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    stage_[i] = static_cast<std::uint8_t>(st);
    shape_[i] = Const;
    a_[i] = target_[i] = value;
    d_[i] = 0.0f;
    snap_[i] = true;
    pos_[i] = 0;
    len_[i] = frames;
}

template <int N>
void AmpEnvelopeLanes<N>::setLinear(int lane, AmpEnvStage st, float from, float to, std::int32_t frames) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    stage_[i] = static_cast<std::uint8_t>(st);
    shape_[i] = Linear;
    a_[i] = from;
    target_[i] = to;
    d_[i] = frames > 0 ? (to - from) / static_cast<float>(frames) : 0.0f;
    snap_[i] = true;
    pos_[i] = 0;
    len_[i] = frames;
}

template <int N>
void AmpEnvelopeLanes<N>::setGeo(int lane, AmpEnvStage st, float asymptote, double term, double rho, float target,
                                 std::int32_t frames, bool snap) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    stage_[i] = static_cast<std::uint8_t>(st);
    shape_[i] = Geo;
    a_[i] = asymptote;
    term_[i] = term;
    target_[i] = target;
    snap_[i] = snap;
    pos_[i] = 0;
    len_[i] = frames;
    double r = rho; // ρ^(2^b) by squaring: per-run powers without libm
    for (auto& row : rhoPow_)
    {
        row[i] = r;
        r *= r;
    }
}

template <int N>
void AmpEnvelopeLanes<N>::startAttack(int lane, float peak, AmpEnvTables::Segment attack, std::int32_t hold) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    const float from = level_[i];
    peak_[i] = peak;
    if (from > 0.0f && attack.frames < AmpEnvTables::kDeclickFrames) // restart from a level: never a step
        attack = tables_.declickAttack();
    std::int32_t len = attack.frames;
    bool complete = true;
    if (hold != AmpEnvTables::kInfinite && hold < len) // HOLD includes the attack: release starts at hold (ENV-002)
    {
        len = hold;
        complete = false;
        holdAfter_[i] = kNoHold;
    }
    else
        holdAfter_[i] = hold == AmpEnvTables::kInfinite ? AmpEnvTables::kInfinite : hold - len;

    if (base_[i].attackCurve == EnvCurve::Linear || attack.frames == 0)
    {
        setLinear(lane, AmpEnvStage::Attack, from, peak, attack.frames);
        len_[i] = len;
        snap_[i] = complete;
    }
    else
        setGeo(lane, AmpEnvStage::Attack, peak, static_cast<double>(from) - static_cast<double>(peak), attack.rho, peak,
               len, complete);
}

template <int N> void AmpEnvelopeLanes<N>::startRelease(int lane) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    const auto& p = base_[i];
    const float from = level_[i];
    const auto idx = static_cast<std::uint8_t>(lockedRelease_[i] >= 0 ? lockedRelease_[i] : p.release);
    const auto seg = tables_.release(idx, p.sync);
    if (seg.frames == AmpEnvTables::kInfinite) // RELEASE = INFINITY: never completes (the voice ends with the sample)
        setConst(lane, AmpEnvStage::Release, from, AmpEnvTables::kInfinite);
    else if (! (from > 0.0f))
        setConst(lane, AmpEnvStage::Idle, 0.0f, AmpEnvTables::kInfinite);
    else if (p.releaseCurve == EnvCurve::Linear)
        setLinear(lane, AmpEnvStage::Release, from, 0.0f, seg.frames);
    else
        setGeo(lane, AmpEnvStage::Release, 0.0f, static_cast<double>(from), seg.rho, 0.0f, seg.frames, true);
}

template <int N> void AmpEnvelopeLanes<N>::enterNext(int lane) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    switch (static_cast<AmpEnvStage>(stage_[i]))
    {
        case AmpEnvStage::Attack:
            if (holdAfter_[i] == kNoHold || holdAfter_[i] == 0)
                startRelease(lane);
            else
                setConst(lane, AmpEnvStage::Hold, peak_[i], holdAfter_[i]);
            return;
        case AmpEnvStage::Hold:
            startRelease(lane);
            return;
        case AmpEnvStage::BypassIn:
            setConst(lane, AmpEnvStage::Bypass, 1.0f, AmpEnvTables::kInfinite);
            return;
        case AmpEnvStage::Release:
        case AmpEnvStage::Kill:
        case AmpEnvStage::BypassOut:
            gated_[i] = false;
            setConst(lane, AmpEnvStage::Idle, 0.0f, AmpEnvTables::kInfinite);
            return;
        case AmpEnvStage::Idle:
        case AmpEnvStage::Bypass:
        case AmpEnvStage::Thru:
            return; // infinite segments
    }
}

// --- rendering ------------------------------------------------------------------------------------------------------

template <int N> void AmpEnvelopeLanes<N>::renderRun(int lane, float* g, int n) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    while (n > 0)
    {
        int m = n;
        bool ends = false;
        if (len_[i] >= 0)
        {
            const std::int32_t rem = len_[i] - pos_[i];
            if (rem <= m)
            {
                m = rem;
                ends = true;
            }
        }
        if (m > 0)
        {
            switch (shape_[i])
            {
                case Const:
                    k_->fillConstant(g, m, a_[i]);
                    break;
                case Linear:
                    k_->fillLinear(g, m, a_[i], d_[i], static_cast<float>(pos_[i] + 1));
                    break;
                default:
                {
                    // Lanes t·ρ^1…4 from the double state; step ρ^4. Then advance the state by ρ^m (binary powering).
                    const double t = term_[i], r1 = rhoPow_[0][i], r2 = rhoPow_[1][i], r4 = rhoPow_[2][i];
                    const envk::GeoSeed seed {{static_cast<float>(t * r1), static_cast<float>(t * r2),
                                              static_cast<float>(t * r2 * r1), static_cast<float>(t * r4)},
                                             static_cast<float>(r4)};
                    k_->fillGeometric(g, m, a_[i], seed);
                    double f = 1.0;
                    for (int b = 0, mm = m; mm != 0; ++b, mm >>= 1)
                        if (mm & 1)
                            f *= rhoPow_[static_cast<std::size_t>(b)][i];
                    term_[i] = t * f;
                    break;
                }
            }
            pos_[i] += m;
            if (ends && snap_[i])
                g[m - 1] = target_[i]; // land exactly on the target (0 is exact silence)
            level_[i] = g[m - 1];
            g += m;
            n -= m;
        }
        else if (ends && snap_[i])
            level_[i] = target_[i]; // zero-length segment (attack 0): jump to its end value
        if (ends)
            enterNext(lane);
    }
}

template <int N> void AmpEnvelopeLanes<N>::settle(int lane) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    while (len_[i] >= 0 && pos_[i] >= len_[i])
    {
        if (snap_[i])
            level_[i] = target_[i];
        enterNext(lane);
    }
}

template <int N> bool AmpEnvelopeLanes<N>::constantBlock(int lane, float& value) const noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    if (events_[i] != 0 || shape_[i] != Const || (len_[i] >= 0 && len_[i] - pos_[i] < kFrames))
        return false;
    value = a_[i];
    return true;
}

template <int N> void AmpEnvelopeLanes<N>::advanceConstant(int lane, int n) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    level_[i] = a_[i];
    age_[i] = age_[i] < kAgeMax ? age_[i] + n : age_[i];
    if (len_[i] < 0)
        return;
    pos_[i] += n;
    if (pos_[i] >= len_[i])
        enterNext(lane);
}

template <int N> void AmpEnvelopeLanes<N>::renderGain(int lane, float* gain) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    float v = 0.0f;
    if (constantBlock(lane, v))
    {
        k_->fillConstant(gain, kFrames, v);
        advanceConstant(lane, kFrames);
        return;
    }
    int at = 0;
    const int n = events_[i];
    events_[i] = 0; // events applied below; finished() stays false until then because stage_ changes first
    for (int e = 0; e < n; ++e)
    {
        const Event ev = queue_[i][static_cast<std::size_t>(e)];
        renderRun(lane, gain + at, ev.frame - at);
        at = ev.frame;
        applyEvent(lane, ev);
    }
    renderRun(lane, gain + at, kFrames - at);
    age_[i] = age_[i] < kAgeMax ? age_[i] + kFrames : age_[i];
}

template <int N> void AmpEnvelopeLanes<N>::process(int lane, float* l, float* r) noexcept
{
    const auto i = static_cast<std::size_t>(lane);
    float v = 0.0f;
    if (constantBlock(lane, v))
    {
        if (stage_[i] == static_cast<std::uint8_t>(AmpEnvStage::Thru))
            return;
        advanceConstant(lane, kFrames);
        if (v == 1.0f)
            return; // bypass / unity peak: bit-transparent (NFR-030)
        if (v == 0.0f)
        {
            std::memset(l, 0, sizeof(float) * kFrames);
            std::memset(r, 0, sizeof(float) * kFrames);
            return;
        }
        k_->applyConstant(l, r, v, kFrames);
        return;
    }
    alignas(16) float gain[kFrames];
    renderGain(lane, gain);
    k_->applyGain(l, r, gain, kFrames);
}

template class AmpEnvelopeLanes<1>;
template class AmpEnvelopeLanes<kTracks>;
} // namespace engine::modulation
