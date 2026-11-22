#include "engine/modulation/LfoStage.h"

#include "core/util/lfo/LfoLaw.h"
#include "engine/mixer/Mixer.h"
#include "engine/trigger/SourceSet.h"
#include "engine/voice/TrackVoice.h"

namespace engine::modulation
{
namespace
{
using T = bridge::ParamTarget;

int roundInt(float v) noexcept { return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f); }
int clampInt(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
float clampF(float v, float lo, float hi) noexcept { return ! (v >= lo) ? lo : (v > hi ? hi : v); }

template <class E> bool setEnum(E& e, float v, int count) noexcept
{
    const auto n = static_cast<E>(clampInt(roundInt(v), 0, count - 1));
    if (n == e)
        return false;
    e = n;
    return true;
}
bool setByte(std::uint8_t& b, float v) noexcept
{
    const auto n = static_cast<std::uint8_t>(clampInt(roundInt(v), 0, 127));
    if (n == b)
        return false;
    b = n;
    return true;
}
bool setBool(bool& b, float v) noexcept
{
    const bool n = v >= 0.5f;
    if (n == b)
        return false;
    b = n;
    return true;
}
} // namespace

bool applyLfoParam(LfoParams& p, LfoField field, float value) noexcept
{
    switch (field)
    {
        case LfoField::Speed:
        {
            const float v = clampF(value, 0.0f, core::lfo::kKnobMax);
            if (v == p.speed)
                return false;
            p.speed = v;
            return true;
        }
        case LfoField::Depth: return setByte(p.depth, value);
        case LfoField::Phase: return setByte(p.phase, value);
        case LfoField::Shape: return setEnum(p.shape, value, kLfoShapes);
        case LfoField::Destination:
        {
            const int i = roundInt(value);
            const core::ParamId id = i < 0 || i >= 0xFFFF ? core::ParamId {} : core::ParamId {static_cast<std::uint16_t>(i)};
            if (id == p.destination)
                return false;
            p.destination = id;
            return true;
        }
        case LfoField::Sync: return setBool(p.sync, value);
        case LfoField::TriggerMode: return setEnum(p.mode, value, 3);
        case LfoField::TriggerOn: return setEnum(p.triggerOn, value, 2);
        case LfoField::RandomStart: return setByte(p.randomStart, value);
        case LfoField::Bypass: return setBool(p.bypass, value);
        case LfoField::Count: break;
    }
    return false;
}

void LfoStage::prepare(double sampleRate) noexcept
{
    for (auto& l : lfo_)
        l.prepare(sampleRate);
    locks_ = {};
    amount_ = {};
    speedSet_.fill(-1.0f);
    applied_ = {}; // the engine prepares the matrix (all slots cleared) together with the stage
    dirty_ = 0xFFFF;
}

void LfoStage::setParam(LfoField field, std::uint16_t scope, float value) noexcept
{
    if (scope > 0xFF)
        return; // not a track scope
    const int scene = scope >> 4, track = scope & 15;
    if (applyLfoParam(params_[scene][track], field, value) && scene == scene_)
        dirty_ = static_cast<std::uint16_t>(dirty_ | (1u << track));
}

void LfoStage::configure(int track, const LfoParams& p) noexcept
{
    auto& l = lfo_[static_cast<std::size_t>(track)];
    const auto& lk = locks_[static_cast<std::size_t>(track)];
    const float speed = (lk.mask & 1u) != 0 ? lk.value[0] : p.speed;
    const float depth = (lk.mask & 2u) != 0 ? lk.value[1] : static_cast<float>(p.depth);
    const float phase = (lk.mask & 4u) != 0 ? lk.value[2] : static_cast<float>(p.phase);
    l.setShape(p.shape);
    if (p.sync)
        l.setSyncDivision(core::lfo::syncDivisionIndex(speed));
    else
    {
        l.setSyncDivision(-1);
        auto& last = speedSet_[static_cast<std::size_t>(track)];
        if (last != speed) // exp2 on a change only (never per block)
        {
            l.setSpeedHz(core::lfo::speedHz(speed));
            last = speed;
        }
    }
    l.setDepth(core::lfo::depth01(depth));
    l.setPhaseOffset(core::lfo::phase01(phase));
    l.setSeed(core::lfo::seedOf(p.randomStart));
    l.setOneShot(p.mode == LfoTriggerMode::OneShot);
}

// SEQ-021: the step's locks of slots 18–20 apply from its trigger; the others revert unless their last lock held.
void LfoStage::applyLocks(int track, const trigger::LockSet* locks) noexcept
{
    auto& lk = locks_[static_cast<std::size_t>(track)];
    static constexpr int kSlots[3] = {kLockSlotLfoSpeed, kLockSlotLfoDepth, kLockSlotLfoPhase};
    std::uint8_t mask = 0, hold = 0;
    for (int k = 0; k < 3; ++k)
    {
        float v = 0.0f;
        const auto bit = static_cast<std::uint8_t>(1u << k);
        if (locks != nullptr && locks->get(kSlots[k], v))
        {
            mask = static_cast<std::uint8_t>(mask | bit);
            lk.value[k] = clampF(v, 0.0f, core::lfo::kKnobMax);
            if (locks->change != nullptr && locks->change[kSlots[k]] != 0) // 1 Hold, 2 Glide (held)
                hold = static_cast<std::uint8_t>(hold | bit);
        }
        else if ((lk.hold & bit) != 0)
        {
            mask = static_cast<std::uint8_t>(mask | bit); // held from an earlier step
            hold = static_cast<std::uint8_t>(hold | bit);
        }
    }
    if (mask != lk.mask || mask != 0)
        dirty_ = static_cast<std::uint16_t>(dirty_ | (1u << track));
    lk.mask = mask;
    lk.hold = hold;
}

void LfoStage::process(const BlockContext& ctx, int scene, std::span<const trigger::TriggerEvent> events,
                       ModMatrix& matrix) noexcept
{
    scene &= 15;
    if (scene != scene_)
    {
        scene_ = scene;
        dirty_ = 0xFFFF; // the scene's own LFO settings; the phases keep running
    }
    const bool start = ctx.startedThisBlock;
    const double ppqPerFrame = ctx.frames > 0 ? (ctx.ppqEnd - ctx.ppqStart) / ctx.frames : 0.0;

    // Triggers in frame order: locks of sequencer steps (ratchet repeats re-apply nothing), restarts (LFO-007).
    std::uint16_t restart = 0;
    std::array<std::uint8_t, kTracks> frame {};
    for (const auto& e : events)
    {
        if (e.track >= kTracks)
            continue;
        const bool full = e.kind == trigger::TriggerKind::Full, half = e.kind == trigger::TriggerKind::Half;
        if ((full || half || e.kind == trigger::TriggerKind::LockOnly) && e.source != trigger::TriggerSource::Retrigger &&
            e.source != trigger::TriggerSource::Repeat)
            applyLocks(e.track, e.locks);
        const auto& p = params_[scene][e.track];
        if (p.mode == LfoTriggerMode::Free)
            continue;
        if (full || (half && p.triggerOn == LfoTriggerOn::FullAndHalf))
        {
            restart = static_cast<std::uint16_t>(restart | (1u << e.track));
            frame[e.track] = e.frame; // the block's last restart wins
        }
    }

    for (int t = 0; t < kTracks; ++t)
    {
        const auto ti = static_cast<std::size_t>(t);
        auto& l = lfo_[ti];
        const LfoParams& p = params_[scene][t];
        if (((dirty_ >> t) & 1u) != 0)
            configure(t, p);
        if (start) // every mode restarts with the transport (Free "runs from transport start"; renders repeat)
            l.restartAt(ctx.ppqStart);
        if (((restart >> t) & 1u) != 0)
            l.restartAt(ctx.ppqStart + ppqPerFrame * frame[ti]);
        const float v = l.advanceBlock(ctx.bpm, ctx.ppqStart, ctx.playing);

        const bool active = ! p.bypass && p.destination.valid() && l.depth() > 0.0f; // LFO-009
        const core::ParamId dest = active ? p.destination : core::ParamId {};
        if (applied_[ti].valid() && applied_[ti] != dest)
            matrix.clear(t, applied_[ti], ModSource::Lfo);
        applied_[ti] = {};
        amount_[ti] = 0.0f;
        if (active && matrix.set(t, dest, ModSource::Lfo, ModKind::Offset, v * l.depth())) // FX-084: lfoOut · depth
        {
            applied_[ti] = dest;
            amount_[ti] = v * l.depth();
        }
    }
    dirty_ = 0;
}

// ---- TrackModSink ----

bool TrackModSink::modulate(int track, core::ParamId dest, const ModValue& v) noexcept { return apply(track, dest, &v); }

void TrackModSink::release(int track, core::ParamId dest) noexcept { apply(track, dest, nullptr); }

bool TrackModSink::apply(int track, core::ParamId dest, const ModValue* v) noexcept
{
    if (bindings_ == nullptr || sources_ == nullptr || track < 0 || track >= kTracks)
        return false;
    const T target = bindings_->find(dest);
    const int pitch = static_cast<int>(T::PlaybackFirst) + static_cast<int>(voice::PlaybackField::Pitch);
    if (static_cast<int>(target) == pitch)
    {
        // −24…+24 st normalised (playback.pitch); the sum in one voice slot, clamped against the pitch the notes play
        // (the stored pitch or the step's lock), so an Override lands on its value and an Offset stays in range.
        float cents = 0.0f;
        if (v != nullptr)
        {
            const float base = sources_->sources[static_cast<std::size_t>(track)].basePitchCents();
            const float base01 = (base + kPitchSpanCents * 0.5f) / kPitchSpanCents;
            cents = v->apply(clampF(base01, 0.0f, 1.0f)) * kPitchSpanCents - kPitchSpanCents * 0.5f - base;
        }
        sources_->sources[static_cast<std::size_t>(track)].voice().setModCents(static_cast<int>(voice::ModSource::Lfo), cents);
        return true;
    }
    const int env = static_cast<int>(target) - static_cast<int>(T::EnvFirst);
    if (env >= 0 && env <= static_cast<int>(AmpEnvField::Release))
    {
        const AmpEnvParams& base = sources_->envParams[scene_][track];
        AmpEnvParams p = base;
        if (v != nullptr)
        {
            std::uint8_t* field = nullptr;
            int range = 127;
            switch (static_cast<AmpEnvField>(env))
            {
                case AmpEnvField::Velocity: field = &p.velocity; break;
                case AmpEnvField::Attack: field = &p.attack; break;
                case AmpEnvField::Hold: field = &p.hold; range = 128; break;
                case AmpEnvField::Release: field = &p.release; range = 128; break;
                default: return false;
            }
            const float r = static_cast<float>(range);
            *field = static_cast<std::uint8_t>(clampInt(roundInt(v->apply(static_cast<float>(*field) / r) * r), 0, range));
        }
        sources_->env.setParams(track, p, bpm_);
        return true;
    }
    if (mixer_ == nullptr)
        return false;
    mixer::Mixer::StripMod which {};
    float lo = 0.0f, range = 127.0f;
    switch (target)
    {
        case T::TrackVolume:
            which = mixer::Mixer::StripMod::Volume;
            break;
        case T::TrackPan:
            which = mixer::Mixer::StripMod::Pan;
            lo = -64.0f;
            range = 128.0f;
            break;
        case T::TrackSendAmount:
            which = mixer::Mixer::StripMod::Send;
            break;
        default: return false;
    }
    // The mixer applies (override or its stored-or-locked value) + offset, clamped to the range: an override is
    // absolute, never relative to the stored value (a USER item on a locked step lands on its value).
    const float offset = v != nullptr ? v->offset01 * range : 0.0f;
    const bool overridden = v != nullptr && v->hasOverride;
    mixer_->setModulation(track, which, offset, overridden, overridden ? v->override01 * range + lo : 0.0f);
    return true;
}
} // namespace engine::modulation
