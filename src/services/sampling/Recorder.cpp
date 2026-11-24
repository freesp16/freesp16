#include "services/sampling/Recorder.h"

#include "services/library/SidecarStore.h"

#include "core/util/ThreadRole.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <pthread.h>
#include <sched.h>

namespace services::sampling
{
namespace
{
// High priority, below the audio thread (threading.md, core/util/ThreadRole.h); best effort: without the right (the
// desktop) it stays normal.
void raisePriority()
{
    (void) core::threads::apply(core::threads::Role::Recorder, "recorder");
}
} // namespace

Recorder::Recorder(core::sampling::CaptureHub& hub) : hub_(hub)
{
    hub_.retain();
    chunkFrames_ = static_cast<std::int64_t>(WavWriter::kChunkBytes / 6); // a stereo chunk's frames (64 KB)
    l_.assign(static_cast<std::size_t>(chunkFrames_), 0.0f);
    r_.assign(static_cast<std::size_t>(chunkFrames_), 0.0f);
    thread_ = std::thread([this] { run(); });
}

Recorder::~Recorder()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
    writer_.abandon();
    hub_.release();
}

core::Result<void> Recorder::start(const juce::File& file, double sampleRate, int channels)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (armed_.load(std::memory_order_acquire))
        return core::Result<void>::failure("A recording is already running", "Recorder::start while armed");
    if (auto ok = writer_.open(file, sampleRate, channels); ! ok)
        return ok;
    info_ = {};
    info_.file = file;
    info_.channels = channels == 1 ? 1 : 2;
    info_.sampleRate = sampleRate;
    info_.lowDisk = file.getParentDirectory().getBytesFreeOnVolume() < kLowDiskBytes;
    armSeq_ = hub_.status.takeSeq.load(std::memory_order_acquire);
    haveTake_ = false;
    pos_ = -1;
    finished_ = false;
    done_ = {};
    cancel_.store(false, std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
    wake_.notify_all();
    return {};
}

void Recorder::stop(std::function<void(TakeInfo)> done)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (finished_)
    {
        finished_ = false;
        TakeInfo r = result_;
        lock.unlock();
        if (done)
            done(std::move(r));
        return;
    }
    done_ = std::move(done);
}

void Recorder::cancel()
{
    cancel_.store(true, std::memory_order_release);
    wake_.notify_all();
}

void Recorder::complete(TakeInfo info)
{
    std::function<void(TakeInfo)> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        armed_.store(false, std::memory_order_release);
        done = std::move(done_);
        done_ = {};
        if (! done)
        {
            finished_ = true;
            result_ = info;
        }
    }
    if (done)
        done(std::move(info));
}

void Recorder::run()
{
    raisePriority();
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(kPollMs),
                           [this] { return quit_; });
            if (quit_)
                return;
            if (! armed_.load(std::memory_order_acquire))
                continue;
        }
        if (cancel_.exchange(false, std::memory_order_acq_rel))
        {
            writer_.abandon();
            TakeInfo info = info_;
            info.cancelled = true;
            complete(std::move(info));
            continue;
        }
        drain();
    }
}

bool Recorder::drain()
{
    auto& s = hub_.status;
    const auto state = static_cast<core::sampling::RecState>(s.state.load(std::memory_order_acquire));
    const std::uint32_t seq = s.takeSeq.load(std::memory_order_relaxed);
    if (! haveTake_)
    {
        if (seq == armSeq_)
            return false; // the engine has not taken the REC press yet
        haveTake_ = true;
        seq_ = seq;
        info_.takeSeq = seq;
        info_.scene = s.takeScene.load(std::memory_order_relaxed);
        info_.track = s.takeTrack.load(std::memory_order_relaxed);
    }
    auto fail = [this](std::string why)
    {
        writer_.abandon();
        TakeInfo info = info_;
        info.error = std::move(why);
        complete(std::move(info));
        return true;
    };
    if (seq != seq_)
        return fail("the take was superseded by a newer one");
    if (state == core::sampling::RecState::Cancelled)
    {
        writer_.abandon();
        TakeInfo info = info_;
        info.cancelled = true;
        complete(std::move(info));
        return true;
    }
    const std::int64_t start = s.takeStart.load(std::memory_order_relaxed);
    if (start < 0)
        return false; // standby: waiting for the threshold / PLAY
    if (pos_ < 0)
    {
        pos_ = start;
        info_.ringStart = start;
        info_.bpm = s.takeBpm.load(std::memory_order_relaxed);
        info_.bars = s.takeBars.load(std::memory_order_relaxed);
    }
    const std::int64_t end = s.takeEnd.load(std::memory_order_relaxed);
    const std::int64_t head = hub_.ring.head();
    const std::int64_t upto = end >= 0 ? std::min(end, head) : head;
    if (pos_ < upto)
        drains_.fetch_add(1, std::memory_order_relaxed);
    while (pos_ < upto)
    {
        const int n = static_cast<int>(std::min(chunkFrames_, upto - pos_));
        if (! hub_.ring.read(pos_, n, l_.data(), r_.data()))
            return fail("overrun: the recorder fell behind the capture ring");
        for (int i = 0; i < n; ++i)
        {
            info_.peak = std::max(info_.peak, std::fabs(l_[static_cast<std::size_t>(i)]));
            if (info_.channels == 2)
                info_.peak = std::max(info_.peak, std::fabs(r_[static_cast<std::size_t>(i)]));
        }
        if (auto ok = writer_.write(l_.data(), r_.data(), n); ! ok)
            return fail(ok.error().message + ": " + ok.error().detail);
        pos_ += n;
        written_.store(pos_ - start, std::memory_order_relaxed);
    }
    if (end < 0 || pos_ < end)
        return false;
    info_.frames = writer_.frames();
    if (auto ok = writer_.finish(); ! ok)
        return fail(ok.error().message + ": " + ok.error().detail);
    core::model::SampleMetadata md {};
    md.bpm = info_.bpm;
    md.bars = info_.bars;
    md.peak = info_.peak;
    if (auto ok = library::SidecarStore().write(info_.file, md); ! ok)
        info_.error = "sidecar: " + ok.error().message; // the take itself is fine
    TakeInfo info = info_;
    info.ok = true;
    complete(std::move(info));
    return true;
}
} // namespace services::sampling
