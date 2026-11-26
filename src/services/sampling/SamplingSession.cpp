#include "services/sampling/SamplingSession.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <optional>

namespace services::sampling
{
namespace
{
using core::sampling::LiveState;
constexpr double kLiveBars[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0}; // live.length (engine/sampling/LiveSampler.h)

// Take buffers a destroyed session still saw referenced (the engine's slot or a voice): they are freed by the next
// collect() of any session, or at process exit, once their count reached 0. Without this a session destroyed before
// the engine (tests; any composition root with that order) would leak every installed take: the engine's TrackSample
// DestroyFn only releases, and nobody else frees a buffer the pool does not own.
struct Orphans
{
    std::mutex mutex;
    std::vector<core::audio::SampleBuffer*> list;
    void adopt(core::audio::SampleBuffer* b)
    {
        std::lock_guard<std::mutex> lock(mutex);
        list.push_back(b);
    }
    void sweep()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = list.begin(); it != list.end();)
            if ((*it)->refs.load(std::memory_order_acquire) == 0)
            {
                core::audio::freeSampleBuffer(*it);
                it = list.erase(it);
            }
            else
                ++it;
    }
    ~Orphans() { sweep(); } // process exit: the engines are gone, every count is 0
};
Orphans& orphans()
{
    static Orphans o;
    return o;
}
} // namespace

SamplingSession::SamplingSession(engine::EngineBridge& b, library::Workers& w, SessionConfig c)
    : bridge_(b), workers_(w), config_(std::move(c))
{
    // NFR-010: takes cut by a crash or a power loss become WAVs again (WavWriter::salvage); logged by the
    // first frame() once `log` is set.
    salvaged_ = WavWriter::salvage(config_.recordings);
}

SamplingSession::~SamplingSession()
{
    *alive_ = false;
    recorder_.reset();
    if (live_.buffer != nullptr)
        core::audio::release(live_.buffer);
    live_.buffer = nullptr;
    collect(); // frees what the engine already gave back (the app destroys the engine first)
    for (auto* b : owned_) // still installed or sounding: freed once the engine lets go (Orphans)
        orphans().adopt(b);
    owned_.clear();
    if (hub_ != nullptr)
        hub_->release();
}

std::int64_t SamplingSession::ringFramesFor(double bpm, double rate) noexcept
{
    if (bpm <= 0)
        bpm = 120;
    const double eightBars = (8.0 * 4.0 + 2.0) * 60.0 / bpm; // 8 bars + a beat of margin on each side
    const double seconds = std::clamp(std::max(kMinRingSeconds, eightBars), kMinRingSeconds, kMaxRingSeconds);
    return static_cast<std::int64_t>(seconds * rate + 0.5);
}

bool SamplingSession::liveLengthFits(int index, double bpm, double rate, std::int64_t ringFrames) noexcept
{
    if (index < 0 || index > 5 || bpm <= 0)
        return false;
    const double frames = (kLiveBars[index] * 4.0 + 2.0) * 60.0 / bpm * rate + rate; // window + margins + 1 s
    return frames <= static_cast<double>(ringFrames - core::sampling::CaptureRing::kMaxWrite);
}

bool SamplingSession::post(std::string_view id, std::uint16_t scope, float value)
{
    if (! config_.resolveParam)
        return false;
    const core::ParamId p = config_.resolveParam(id);
    return p.valid() && bridge_.post(engine::ParamChange {p, scope, value, 0});
}

bool SamplingSession::ensureCapture(double bpm)
{
    const std::int64_t need = ringFramesFor(bpm, config_.sampleRate);
    if (hub_ != nullptr && (hub_->ring.capacity() >= need || recording() || live_.building || liveUnresolved()))
        return true;
    auto* h = new (std::nothrow) core::sampling::CaptureHub();
    if (h == nullptr)
        return false;
    h->sampleRate = config_.sampleRate;
    if (! h->ring.allocate(need))
    {
        h->release();
        return false;
    }
    h->retain(); // the engine's reference (its LiveTake DestroyFn releases it)
    if (! bridge_.post(engine::ObjectSwap {{engine::SwapSlot::LiveTake, core::sampling::kHubScene,
                                            core::sampling::kHubIndex},
                                           h}))
    {
        h->release();
        h->release();
        return false;
    }
    recorder_.reset();
    if (hub_ != nullptr)
        hub_->release();
    hub_ = h;
    recorder_ = std::make_unique<Recorder>(*h);
    if (log)
        log("SAMPLING ring frames=" + std::to_string(need) + " seconds=" +
            std::to_string(static_cast<double>(need) / config_.sampleRate));
    return true;
}

core::Result<juce::File> SamplingSession::record(int scene, int track, int channels)
{
    if (hub_ == nullptr || ! recorder_)
        return core::Result<juce::File>::failure("Sampling is not ready", "no capture ring (ensureCapture)");
    const juce::File file = proposeName(config_.recordings, "REC");
    if (auto ok = recorder_->start(file, config_.sampleRate, channels); ! ok)
        return ok.error();
    const auto scope = static_cast<std::uint16_t>((scene & 15) << 4 | (track & 15));
    if (! post("sampling.rec", scope, 1.0f))
    {
        recorder_->cancel();
        return core::Result<juce::File>::failure("Sampling is not ready", "sampling.rec refused by the engine");
    }
    auto alive = alive_;
    auto& workers = workers_;
    recorder_->stop([this, alive, &workers](TakeInfo info) {
        workers.post([this, alive, info = std::move(info)] {
            if (*alive && onTake)
                onTake(info);
        });
    });
    return file;
}

void SamplingSession::stopRecord(int scene, int track)
{
    post("sampling.rec", static_cast<std::uint16_t>((scene & 15) << 4 | (track & 15)), 0.0f);
}

void SamplingSession::dropRecord(int scene, int track)
{
    post("sampling.drop", static_cast<std::uint16_t>((scene & 15) << 4 | (track & 15)), 1.0f);
}

bool SamplingSession::liveArm(int scene, int track)
{
    if (live_.building || liveUnresolved() || resetPending_ || saveAckPending_ ||
        ! ensureCapture(bridge_.playhead().bpm))
        return false;
    const std::uint32_t request = armRequest_ % 0xFFFFFEu + 1;
    const auto scope = static_cast<std::uint16_t>((scene & 15) << 4 | (track & 15));
    if (! post("live.rec", scope, static_cast<float>(1 + request)))
        return false;
    armRequest_ = request;
    armScene_ = scene & 15;
    armTrack_ = track & 15;
    armPosted_ = true;
    return true;
}

bool SamplingSession::armAwaitingEngine() const
{
    return armPosted_ && hub_ != nullptr && hub_->status.liveArmRequest.load(std::memory_order_acquire) != armRequest_;
}

bool SamplingSession::hasPendingTake() const
{
    if (hub_ == nullptr)
        return false;
    if (armAwaitingEngine())
        return true;
    const auto& status = hub_->status;
    const auto state = static_cast<LiveState>(status.liveState.load(std::memory_order_acquire));
    return (live_.building && live_.buffer == nullptr) ||
           (status.liveReset.load(std::memory_order_relaxed) == liveReset_ &&
            (state == LiveState::Armed || state == LiveState::Recording ||
             (state >= LiveState::Done && status.liveSeq.load(std::memory_order_relaxed) != live_.seq)));
}

bool SamplingSession::protectsLiveSlot(int scene, int track) const
{
    if (live_.dirty && live_.scene == scene && live_.track == track)
        return true;
    if (armAwaitingEngine())
        return armScene_ == scene && armTrack_ == track;
    if (! hasPendingTake() || hub_ == nullptr)
        return false;
    const auto& status = hub_->status;
    return status.liveScene.load(std::memory_order_relaxed) == scene &&
           status.liveTrack.load(std::memory_order_relaxed) == track;
}

void SamplingSession::liveDisplaced(const core::audio::SampleBuffer* buffer)
{
    if (buffer != live_.buffer)
        return;
    live_.attached = false;
    live_.rebuildWanted = false;
}

bool SamplingSession::installLive(int scene, int track, const core::audio::SampleBuffer* buffer)
{
    if (config_.installLive)
        return config_.installLive(scene, track, buffer);

    core::audio::retain(buffer); // the engine's slot reference
    if (bridge_.post(engine::ObjectSwap{
            {engine::SwapSlot::TrackSample, static_cast<std::uint8_t>(scene), static_cast<std::uint8_t>(track)},
            buffer}))
        return true;
    core::audio::release(buffer);
    return false;
}

void SamplingSession::acknowledgeSave(std::uint32_t seq)
{
    saveAckPending_ = ! post("live.resolve", 0, static_cast<float>(seq & 0xFFFFFFu));
}

bool SamplingSession::unloadLive()
{
    if (live_.buffer == nullptr || hasPendingTake())
        return false;
    if (live_.attached && ! (config_.removeLive ? config_.removeLive(live_.scene, live_.track, live_.buffer)
                                                : installLive(live_.scene, live_.track, nullptr)))
        return false;
    resetLive();
    return true;
}

void SamplingSession::resetLive()
{
    if (hub_ == nullptr)
        return;
    core::audio::release(live_.buffer);
    live_ = {};
    armPosted_ = false;
    saveAckPending_ = false;
    liveReset_ = liveReset_ % 0xFFFFFFu + 1;
    resetPending_ = ! post("live.resolve", 0, -static_cast<float>(liveReset_));
}

void SamplingSession::liveDisarm(int scene, int track)
{
    post("live.rec", static_cast<std::uint16_t>((scene & 15) << 4 | (track & 15)), 0.0f);
}

void SamplingSession::setNudge(std::int32_t frames)
{
    if (live_.buffer == nullptr || live_.attached)
        post("live.nudge", 0, static_cast<float>(frames));
    const std::int32_t m = live_.margin;
    live_.nudge = std::clamp(frames, -m, m);
    if (live_.attached && live_.nudge > live_.postCaptured)
        live_.rebuildWanted = true;
}

void SamplingSession::frame()
{
    if (! salvaged_.empty() && log)
    {
        for (const auto& line : salvaged_)
            log(line);
        salvaged_.clear();
    }
    collect();
    if (hub_ == nullptr)
        return;
    if (! armAwaitingEngine())
        armPosted_ = false;
    if (resetPending_)
        resetPending_ = ! post("live.resolve", 0, -static_cast<float>(liveReset_));
    if (saveAckPending_)
        acknowledgeSave(live_.seq);
    const auto& s = hub_->status;
    const auto state = static_cast<LiveState>(s.liveState.load(std::memory_order_acquire));
    if (s.liveReset.load(std::memory_order_relaxed) != liveReset_)
        return;
    const std::uint32_t seq = s.liveSeq.load(std::memory_order_relaxed);
    if (state >= LiveState::Done && seq != live_.seq && ! live_.building)
    {
        core::audio::release(live_.buffer); // the old playback slot keeps its own reference
        live_.buffer = nullptr;
        live_.dirty = false;
        live_.attached = false;
        live_.seq = seq;
        live_.start = s.liveStart.load(std::memory_order_relaxed);
        live_.length = s.liveLength.load(std::memory_order_relaxed);
        live_.margin = s.liveMargin.load(std::memory_order_relaxed);
        live_.nudge = s.liveNudge.load(std::memory_order_relaxed);
        live_.scene = s.liveScene.load(std::memory_order_relaxed);
        live_.track = s.liveTrack.load(std::memory_order_relaxed);
        live_.channels = s.ringChannels.load(std::memory_order_relaxed);
        live_.bpm = s.liveBpm.load(std::memory_order_relaxed);
        live_.bars = s.liveBars.load(std::memory_order_relaxed);
        live_.rebuildWanted = false;
        buildLive(false);
    }
    else if (live_.rebuildWanted && ! live_.building &&
             hub_->ring.head() >= live_.start + live_.length + live_.margin)
    {
        live_.rebuildWanted = false;
        buildLive(true);
    }
}

void SamplingSession::buildLive(bool /*full: the ring now holds the trailing margin*/)
{
    struct Out
    {
        std::optional<BuiltTake> take;
        std::string error;
    };
    auto out = std::make_shared<Out>();
    const TakeWindow w {live_.start, live_.length, live_.margin, live_.channels, config_.sampleRate};
    const auto* hub = hub_;
    hub->retain();
    live_.building = true;
    const auto seq = live_.seq;
    const auto reset = liveReset_;
    auto alive = alive_;
    workers_.run(
        [hub, w, out]
        {
            auto r = buildTake(hub->ring, w);
            if (r)
                out->take = std::move(r.value());
            else
                out->error = r.error().message + " (" + r.error().detail + ")";
            hub->release();
        },
        [this, alive, out, seq, reset]
        {
            if (! *alive || reset != liveReset_ || seq != live_.seq)
                return; // the project or take changed; `out` frees the buffer
            live_.building = false;
            if (live_.buffer != nullptr && ! live_.attached)
                return; // a track edit has already replaced the take in the playback slot
            if (! out->take || ! out->take->buffer || ! installLive(live_.scene, live_.track, out->take->buffer.get()))
            {
                if (live_.buffer == nullptr)
                    acknowledgeSave(seq); // no playable take to keep; REC can try again
                if (log)
                    log(out->take ? "LIVE install refused by the engine" : "LIVE build failed: " + out->error);
                return;
            }
            auto* b = out->take->buffer.release();
            owned_.push_back(b);
            core::audio::retain(b); // ours (live_.buffer)
            const bool newTake = live_.buffer == nullptr;
            if (newTake)
                live_.dirty = true; // a nudge rebuild keeps the existing save state
            core::audio::release(live_.buffer);
            live_.buffer = b;
            live_.attached = true;
            live_.preCaptured = out->take->preCaptured;
            live_.postCaptured = out->take->postCaptured;
            live_.rebuildWanted = live_.postCaptured < live_.margin;
            post("live.takeReady", static_cast<std::uint16_t>(live_.scene << 4 | live_.track),
                 static_cast<float>(seq & 0xFFFFFFu));
            if (log && newTake)
                log("LIVE take seq=" + std::to_string(seq) + " track=" + std::to_string(live_.track + 1) +
                    " frames=" + std::to_string(live_.length) + " margin=" + std::to_string(live_.margin));
            if (onLiveTake)
                onLiveTake(live_.scene, live_.track, b);
        });
}

void SamplingSession::saveLive(const juce::File& target, std::function<void(core::Result<juce::File>)> done)
{
    if (saving_ || live_.buffer == nullptr || live_.length <= 0)
    {
        if (done)
            done(core::Result<juce::File>::failure(saving_ ? "A live take is already saving" : "No live take to save",
                                                   "saveLive"));
        return;
    }

    const auto nudge = live_.nudge;
    if (nudge < -live_.preCaptured || nudge > live_.postCaptured)
    {
        if (done)
        {
            const char* reason = nudge < -live_.preCaptured || ! live_.attached
                                     ? "Reduce the nudge to the captured audio before saving"
                                     : "Wait for the nudged audio to be captured, or reduce the nudge";
            done(core::Result<juce::File>::failure(reason, "saveLive"));
        }
        return;
    }

    saving_ = true;
    const auto* buffer = live_.buffer;
    core::audio::retain(buffer);
    const std::int64_t from = static_cast<std::int64_t>(live_.margin) + nudge, to = from + live_.length;
    core::model::SampleMetadata md {};
    md.bpm = live_.bpm;
    md.bars = live_.bars;
    auto result = std::make_shared<std::optional<core::Result<juce::File>>>();
    auto alive = alive_;
    const auto seq = live_.seq;
    const auto reset = liveReset_;
    workers_.run(
        [buffer, from, to, target, md, result]
        {
            result->emplace(saveBuffer(*buffer, from, to, target, md));
            core::audio::release(buffer);
        },
        [this, alive, result, done = std::move(done), seq, reset, nudge]
        {
            if (! *alive || ! *result)
                return;
            saving_ = false;
            if ((**result).ok() && reset == liveReset_ && seq == live_.seq && nudge == live_.nudge)
            {
                live_.dirty = false;
                acknowledgeSave(seq);
            }
            if (done)
                done(std::move(**result));
        });
}

void SamplingSession::collect()
{
    orphans().sweep();
    for (auto it = owned_.begin(); it != owned_.end();)
    {
        if ((*it)->refs.load(std::memory_order_acquire) == 0)
        {
            core::audio::freeSampleBuffer(*it);
            it = owned_.erase(it);
        }
        else
            ++it;
    }
}
} // namespace services::sampling
