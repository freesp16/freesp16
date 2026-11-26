#include "engine/sampling/SamplingStage.h"

#include "engine/sampling/Threshold.h"
#include "engine/trigger/TriggerRouter.h"

namespace engine::sampling
{
namespace
{
int roundi(float v) noexcept { return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f); }
int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
constexpr auto rx = std::memory_order_relaxed;
} // namespace

void SamplingStage::bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                               bridge::ParamBindings& b)
{
    if (! resolve)
        return;
    for (std::size_t i = 0; i < ids_.size(); ++i)
    {
        ids_[i] = resolve(kSamplingParamIds[i]);
        b.bind(ids_[i], bridge::ParamTarget::Sampling);
    }
    softLimiterId_ = resolve("mixer.softLimiter"); // bound by the mixer (ParamTarget::InputSoftLimiter); observed here
}

void SamplingStage::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    live_.prepare(sampleRate_);
    state_ = RecState::Idle;
    start_ = end_ = -1;
    stopRequested_ = false;
}

bool SamplingStage::onParam(const ParamChange& c) noexcept
{
    if (c.param.valid() && c.param == softLimiterId_)
    {
        input_.setLimiter(c.value >= 0.5f);
        return false; // the mixer keeps its copy of the switch
    }
    for (std::size_t i = 0; i < ids_.size(); ++i)
        if (ids_[i].valid() && c.param == ids_[i])
            return set(static_cast<SamplingParam>(i), c.scope, c.value);
    return false;
}

bool SamplingStage::set(SamplingParam p, std::uint16_t scope, float value) noexcept
{
    const int v = roundi(value);
    const bool busy = state_ == RecState::Standby || state_ == RecState::Recording;
    switch (p)
    {
        case SamplingParam::Source:
            if (! busy && live_.state() != LiveState::Recording) // SMP-001: locked while recording
                source_ = static_cast<Source>(clampi(v, 0, 3));
            return true;
        case SamplingParam::Threshold: threshold_ = clampi(v, 0, kThresholdPlay); return true; // the take keeps its copy
        case SamplingParam::Length: lengthBars_ = clampi(v, 0, 4); return true;
        case SamplingParam::Rec: rec(value >= 0.5f, scope); return true;
        case SamplingParam::PreRoll: preRollMs_ = clampi(v, 0, 100); return true;
        case SamplingParam::ScreenOpen: screenOpen_ = value >= 0.5f; return true;
        case SamplingParam::LiveMode: live_.setMode(value >= 0.5f); return true;
        case SamplingParam::LiveLength: live_.setLengthIndex(v); return true;
        case SamplingParam::LiveLoopAfterRec: live_.setLoopAfterRec(value >= 0.5f); liveLoop_ = value >= 0.5f; return true;
        case SamplingParam::LiveRec:
            if (value >= 0.5f)
            {
                live_.arm(scope >> 4 & 15, scope & 15, 0, liveLoop_); // the hub may arrive in this block's drain
            }
            else
                live_.disarm();
            return true;
        case SamplingParam::LiveNudge: live_.setNudge(v); return true;
        case SamplingParam::LiveTakeReady:
            live_.takeReady(scope & 15, static_cast<std::uint32_t>(v < 0 ? 0 : v));
            return true;
        case SamplingParam::LiveResolve:
            live_.resolve(v);
            return true;
        case SamplingParam::InputLatency: inputLatency_ = clampi(v, 0, 8192); return true;
        case SamplingParam::Quantize: quantize_ = clampi(v, 0, 5); return true; // the take keeps its copy
        case SamplingParam::Drop:
            if (value >= 0.5f)
                drop(scope);
            return true;
        case SamplingParam::Count: break;
    }
    return false;
}

void SamplingStage::install(const CaptureHub* hub) noexcept
{
    // rt-ok: the hub is the one swapped object the audio thread writes into (ring + status), by design (CaptureHub.h)
    auto* h = const_cast<CaptureHub*>(hub);
    if (h == hub_)
        return;
    const bool replaced = hub_ != nullptr; // the first hub keeps a REC / arm of this same block (same drain)
    hub_ = h;
    if (replaced || h == nullptr)
    {
        state_ = state_ == RecState::Standby || state_ == RecState::Recording ? RecState::Cancelled : state_;
        start_ = end_ = -1;
        stopRequested_ = false;
        live_.reset();
    }
    statusDirty_ = true;
}

void SamplingStage::rec(bool on, std::uint16_t scope) noexcept
{
    if (on)
    {
        if (state_ == RecState::Standby || state_ == RecState::Recording)
            return; // (without a hub the take waits in standby: the session posts the hub first, same drain)
        ++seq_;
        takeSource_ = live_.state() == LiveState::Recording ? ringSource_ : source_;
        ringSource_ = takeSource_;
        takeThreshold_ = threshold_;
        takeBars_ = lengthBars_;
        takeBeats_ = kQuantizeBeats[quantize_];
        takeScene_ = scope >> 4 & 15;
        takeTrack_ = scope & 15;
        start_ = end_ = -1;
        stopRequested_ = false;
        state_ = RecState::Standby;
        statusDirty_ = true;
        return;
    }
    if (state_ == RecState::Standby)
    {
        state_ = RecState::Cancelled;
        statusDirty_ = true;
    }
    else if (state_ == RecState::Recording)
        stopRequested_ = true;
}

void SamplingStage::drop(std::uint16_t scope) noexcept
{
    if ((state_ != RecState::Standby && state_ != RecState::Recording) || (scope >> 4 & 15) != takeScene_ ||
        (scope & 15) != takeTrack_)
        return; // a take that already ended (Done) is kept: the drop came too late
    state_ = RecState::Cancelled;
    stopRequested_ = false;
    statusDirty_ = true;
}

void SamplingStage::publishTake() noexcept
{
    if (hub_ == nullptr)
        return;
    auto& s = hub_->status;
    s.takeStart.store(start_, rx);
    s.takeEnd.store(end_, rx);
    s.takeChannels.store(takeSource_ == Source::LineInL || takeSource_ == Source::LineInR ? 1 : 2, rx);
    s.takeTrack.store(takeTrack_, rx);
    s.takeScene.store(takeScene_, rx);
    s.takeBars.store(takeBars_, rx);
    s.takeQuantize.store(takeBeats_, rx);
    s.takeSeq.store(seq_, rx);
    s.state.store(static_cast<std::int32_t>(state_), std::memory_order_release);
    statusDirty_ = false;
}

void SamplingStage::input(stages::BlockBuffers& b) noexcept
{
    input_.process(b);
}

void SamplingStage::schedule(const BlockContext& ctx, const stages::BlockBuffers::Timing& timing,
                             const scene::ScenePatternSwitcher& switcher, trigger::TriggerRouter* router) noexcept
{
    if (hub_ == nullptr)
        return;
    const int frame = boundaryFrame(ctx, timing, switcher.unit(), switcher.currentLength());
    live_.schedule(ctx, frame, hub_->ring.head(), latencyFor(ringSource()), router, &hub_->status);
    // REC QUANTIZE: the take's boundary in this block (capture() starts it there) and the countdown for the UI.
    gridFrame_ = -1;
    if (state_ == RecState::Standby && takeBeats_ > 0)
    {
        double left = -1.0;
        gridFrame_ = gridFrame(ctx, timing, static_cast<double>(takeBeats_), switcher.currentLength(), left);
        hub_->status.startInBeats.store(static_cast<float>(left), rx);
    }
}

void SamplingStage::capture(const BlockContext& ctx, const stages::BlockBuffers& b, const float* tapL,
                            const float* tapR, int startFrame) noexcept
{
    if (hub_ == nullptr)
        return;
    // The SOURCE block (SMP-001): fixed while a take records (sampling or live).
    const bool sampling = state_ == RecState::Standby || state_ == RecState::Recording;
    if (! sampling && live_.state() != LiveState::Recording)
        ringSource_ = source_;
    const float* l = zeros_;
    const float* r = zeros_;
    switch (ringSource_)
    {
        case Source::LineIn:
            if (b.inputL != nullptr)
            {
                l = b.inputL;
                r = b.inputR != nullptr ? b.inputR : b.inputL;
            }
            break;
        case Source::LineInL:
            if (b.inputL != nullptr)
                l = r = b.inputL;
            break;
        case Source::LineInR:
            if (b.inputL != nullptr)
                l = r = b.inputR != nullptr ? b.inputR : b.inputL;
            break;
        case Source::Resample:
            if (tapL != nullptr)
            {
                l = tapL;
                r = tapR != nullptr ? tapR : tapL;
            }
            break;
    }
    auto& ring = hub_->ring;
    const std::int64_t head = ring.head(); // ring index of this block's frame 0
    ring.write(l, r, kBlockFrames);
    const std::int64_t after = head + kBlockFrames;

    if (state_ == RecState::Standby)
    {
        std::int64_t start = -1;
        if (takeBeats_ > 0)
        {
            if (gridFrame_ >= 0)
                start = head + gridFrame_ + latencyFor(takeSource_);
        }
        else if (takeThreshold_ == kThresholdOff)
            start = head;
        else if (takeThreshold_ == kThresholdPlay)
        {
            if (ctx.startedThisBlock && ctx.playing)
                start = head + startFrame + latencyFor(takeSource_);
        }
        else if (const int f = firstAbove(l, r, kBlockFrames, thresholdGain(takeThreshold_)); f >= 0)
        {
            const auto pre = static_cast<std::int64_t>(preRollMs_ * sampleRate_ / 1000.0 + 0.5);
            start = head + f - pre;
            const std::int64_t oldest = ring.oldest();
            start = start < oldest ? oldest : start;
        }
        if (start >= 0)
        {
            start_ = start;
            state_ = RecState::Recording;
            if (takeBars_ > 0)
                end_ = start_ + barsToFrames(takeBars_, ctx.bpm > 0.0 ? ctx.bpm : 120.0, sampleRate_);
            hub_->status.takeBpm.store(static_cast<float>(ctx.bpm > 0.0 ? ctx.bpm : 120.0), rx);
            statusDirty_ = true;
        }
    }
    if (state_ == RecState::Recording)
    {
        if (stopRequested_)
        {
            const bool synced = takeThreshold_ == kThresholdPlay || takeBeats_ > 0; // a synced start's latency
            const std::int64_t now = head + (synced ? latencyFor(takeSource_) : 0);
            const std::int64_t end = now > start_ ? now : start_ + 1;
            end_ = end_ >= 0 && end_ < end ? end_ : end;
            stopRequested_ = false;
            statusDirty_ = true;
        }
        if (end_ >= 0 && after >= end_)
        {
            state_ = RecState::Done;
            statusDirty_ = true;
        }
    }
    if (statusDirty_)
        publishTake();
    live_.afterWrite(after, &hub_->status);

    // Meters and liveness (SMP-003).
    auto& s = hub_->status;
    s.peakL.store(input_.peak(0), rx);
    s.peakR.store(input_.peak(1), rx);
    if (input_.clipped())
        s.clips.store(s.clips.load(rx) + 1, rx);
    s.latency.store(latencyFor(ringSource_), rx);
    s.ringChannels.store(ringSource_ == Source::LineInL || ringSource_ == Source::LineInR ? 1 : 2, rx);
    s.blocks.store(s.blocks.load(rx) + 1, std::memory_order_release);
}
} // namespace engine::sampling
