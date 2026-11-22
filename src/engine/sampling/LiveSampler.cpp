#include "engine/sampling/LiveSampler.h"

#include "engine/trigger/TriggerRouter.h"
#include "engine/voice/PlaybackParams.h"

namespace engine::sampling
{
namespace
{
constexpr double kEps = 1e-9;

double ceilTo(double x, double unit) noexcept // smallest multiple of unit ≥ x (x ≥ −unit)
{
    const double q = x / unit;
    auto i = static_cast<std::int64_t>(q);
    if (static_cast<double>(i) < q - kEps)
        ++i;
    return static_cast<double>(i) * unit;
}
} // namespace

int boundaryFrame(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing, scene::SwitchUnit unit,
                  int patternSteps) noexcept
{
    if (! ctx.playing || timing.preCount)
        return -1;
    if (timing.boundary.has) // a queued scene / pattern switch lands in this block: its frame is the boundary
        return timing.boundary.frame;
    const double p0 = ctx.ppqStart, p1 = ctx.ppqEnd;
    if (! (p1 > p0))
        return -1;
    const double ps = timing.patternStartPpq;
    const double pass = static_cast<double>(patternSteps > 0 ? patternSteps : 16) * scene::kQuartersPerStep;
    // The pre-roll of a pre-count start (p0 < 0 < p1): the boundary is PPQ 0 (the pass start).
    const double rel = p0 - ps < 0.0 ? 0.0 : p0 - ps;
    double b = 0.0;
    switch (unit)
    {
        case scene::SwitchUnit::Step: b = ceilTo(rel, scene::kQuartersPerStep); break;
        case scene::SwitchUnit::Bar:
            b = ceilTo(rel, scene::kQuartersPerBar);
            b = b < pass ? b : pass;
            break;
        case scene::SwitchUnit::Pattern: b = rel <= kEps ? 0.0 : pass; break;
    }
    const double at = ps + b;
    if (at >= p1)
        return -1;
    const double ppf = (p1 - p0) / static_cast<double>(ctx.frames > 0 ? ctx.frames : kBlockFrames);
    const double f = (at - p0) / ppf + 1e-6;
    int frame = f <= 0.0 ? 0 : static_cast<int>(f);
    return frame < kBlockFrames ? frame : -1;
}

std::int64_t barsToFrames(double bars, double bpm, double sampleRate) noexcept
{
    if (bpm <= 0.0 || bars <= 0.0)
        return 0;
    return static_cast<std::int64_t>(bars * 4.0 * 60.0 / bpm * sampleRate + 0.5);
}

void LiveSampler::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    reset();
}

void LiveSampler::reset() noexcept
{
    state_ = LiveState::Idle;
    take_ = {};
    loopFrame_ = -1;
    statusDirty_ = true;
}

void LiveSampler::setMode(bool on) noexcept
{
    mode_ = on;
    if (! on && state_ == LiveState::Armed)
    {
        state_ = LiveState::Idle;
        statusDirty_ = true;
    }
}

void LiveSampler::setLengthIndex(int index) noexcept
{
    lengthIndex_ = index < 0 ? 0 : (index > 5 ? 5 : index);
}

void LiveSampler::arm(int scene, int track, std::int64_t lengthFrames, bool loopAfterRec) noexcept
{
    if (state_ == LiveState::Recording)
        return; // one take at a time (the running one completes first)
    scene_ = scene & 15;
    take_.track = static_cast<std::uint8_t>(track & 15);
    armedLength_ = lengthFrames;
    armedLoop_ = loopAfterRec;
    state_ = LiveState::Armed;
    statusDirty_ = true;
}

void LiveSampler::disarm() noexcept
{
    if (state_ == LiveState::Armed)
        state_ = LiveState::Idle;
    else if (state_ == LiveState::Recording)
        take_.length = -1; // afterWrite() ends it at the current head
    statusDirty_ = true;
}

void LiveSampler::setNudge(std::int32_t frames) noexcept
{
    pendingNudge_ = frames;
    const std::int32_t m = margin_;
    const std::int32_t n = frames < -m ? -m : (frames > m ? m : frames);
    if (n != take_.nudge)
    {
        take_.nudge = n;
        if (state_ == LiveState::Looping)
            state_ = LiveState::LoopPending; // the loop restarts with the moved window at the next boundary
    }
    statusDirty_ = true;
}

void LiveSampler::takeReady(int track, std::uint32_t seq) noexcept
{
    if ((seq & 0xFFFFFFu) != (seq_ & 0xFFFFFFu) || (track & 15) != take_.track)
        return;
    if (state_ == LiveState::Done && take_.loopAfterRec)
    {
        state_ = LiveState::LoopPending;
        statusDirty_ = true;
    }
}

void LiveSampler::unload(int track) noexcept
{
    if ((track & 15) != take_.track)
        return;
    if (state_ == LiveState::LoopPending || state_ == LiveState::Looping)
    {
        state_ = LiveState::Done;
        statusDirty_ = true;
    }
}

void LiveSampler::schedule(const BlockContext& ctx, int frame, std::int64_t head, int latency,
                           trigger::TriggerRouter* router, CaptureStatus* status) noexcept
{
    if (frame >= 0 && state_ == LiveState::Armed && mode_)
    {
        bpm_ = ctx.bpm > 0.0 ? ctx.bpm : 120.0;
        bars_ = kLiveBars[lengthIndex_];
        const std::int64_t len = armedLength_ > 0 ? armedLength_ : barsToFrames(bars_, bpm_, sampleRate_);
        if (armedLength_ > 0)
            bars_ = static_cast<float>(static_cast<double>(len) * bpm_ / (240.0 * sampleRate_));
        latency_ = latency;
        take_.start = head + frame + latency;
        take_.length = len;
        take_.loopAfterRec = armedLoop_;
        margin_ = static_cast<std::int32_t>(60.0 / bpm_ * sampleRate_ + 0.5);
        const std::int32_t m = margin_;
        take_.nudge = pendingNudge_ < -m ? -m : (pendingNudge_ > m ? m : pendingNudge_);
        ++seq_;
        state_ = LiveState::Recording;
        statusDirty_ = true;
    }
    else if (frame >= 0 && state_ == LiveState::LoopPending && router != nullptr)
    {
        // Locks in ParamIndex slots (engine::voice::lockslot): the window inside the take buffer, looped.
        namespace ls = voice::lockslot;
        const auto start = static_cast<float>(margin_ + take_.nudge);
        lockValues_[ls::Start] = start;
        lockValues_[ls::LoopStart] = start;
        lockValues_[ls::Length] = static_cast<float>(take_.length);
        lockValues_[ls::Pitch] = 0.0f;
        lockValues_[ls::TimeStretch] = 0.0f;
        lockValues_[ls::Loop] = 1.0f;
        lockValues_[ls::Reverse] = 0.0f;
        locks_.mask = (1ull << ls::Start) | (1ull << ls::LoopStart) | (1ull << ls::Length) | (1ull << ls::Pitch) |
                      (1ull << ls::TimeStretch) | (1ull << ls::Loop) | (1ull << ls::Reverse);
        locks_.values = lockValues_.data();
        locks_.change = lockChange_.data(); // Revert: the values belong to the loop note only
        const trigger::TriggerEvent e {take_.track, 127, 0, trigger::TriggerKind::Full,
                                       trigger::TriggerSource::Sequencer, static_cast<std::uint8_t>(frame), false,
                                       &locks_};
        if (router->push(e))
        {
            loopFrame_ = head + frame;
            state_ = LiveState::Looping;
            statusDirty_ = true;
        }
    }
    if (statusDirty_)
    {
        publish(status);
        statusDirty_ = false;
    }
}

void LiveSampler::afterWrite(std::int64_t head, CaptureStatus* status) noexcept
{
    if (state_ != LiveState::Recording)
        return;
    if (take_.length < 0) // disarmed while recording: the take ends at the head (≥ 1 frame)
        take_.length = head > take_.start ? head - take_.start : 1;
    if (head >= take_.start + take_.length)
    {
        state_ = LiveState::Done;
        publish(status);
    }
}

void LiveSampler::publish(CaptureStatus* s) const noexcept
{
    if (s == nullptr)
        return;
    constexpr auto rx = std::memory_order_relaxed;
    s->liveStart.store(take_.start, rx);
    s->liveLength.store(take_.length, rx);
    s->liveTrack.store(take_.track, rx);
    s->liveScene.store(scene_, rx);
    s->liveNudge.store(take_.nudge, rx);
    s->liveMargin.store(margin_, rx);
    s->liveBpm.store(static_cast<float>(bpm_), rx);
    s->liveBars.store(bars_, rx);
    s->loopStartFrame.store(loopFrame_, rx);
    s->liveSeq.store(seq_, rx);
    s->liveState.store(static_cast<std::int32_t>(state_), std::memory_order_release);
}
} // namespace engine::sampling
