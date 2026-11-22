#include "engine/midi/MidiInQueue.h"

#include <cmath>

namespace engine::midi
{
void AudioClock::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    nominalNsPerFrame_ = 1.0e9 / sampleRate_;
    anchor_ = {0, 0, nominalNsPerFrame_, true};
    predictedNs_ = 0.0;
    init_ = false;
    synthetic_ = true;
    periodFrames_ = kBlockFrames;
}

void AudioClock::period(std::int64_t frame, std::int64_t hostNs, int frames) noexcept
{
    if (frames > 0)
        periodFrames_ = frames;
    if (hostNs == 0)
    {
        // No device clock: synthetic host time, frame · nsPerFrame (deterministic for tests and renders).
        synthetic_ = true;
        init_ = false;
        anchor_.frame = frame;
        anchor_.hostNs = static_cast<std::int64_t>(std::llround(static_cast<double>(frame) * nominalNsPerFrame_));
        anchor_.nsPerFrame = nominalNsPerFrame_;
        anchor_.valid = true;
        return;
    }
    synthetic_ = false;
    const double measured = static_cast<double>(hostNs);
    if (! init_)
    {
        init_ = true;
        predictedNs_ = measured;
        anchor_ = {frame, hostNs, nominalNsPerFrame_, true};
        return;
    }
    const double df = static_cast<double>(frame - anchor_.frame);
    const double predicted = predictedNs_ + df * anchor_.nsPerFrame;
    const double e = measured - predicted;
    if (df <= 0.0 || std::fabs(e) > static_cast<double>(kResyncNs))
    {
        predictedNs_ = measured; // re-anchor (device restart, xrun); keep the rate estimate
        anchor_.frame = frame;
        anchor_.hostNs = hostNs;
        return;
    }
    // Second-order DLL per period (Adriaensen, "Using a DLL to filter time"): ω = 2π·B·T.
    const double periodSeconds = df * anchor_.nsPerFrame * 1.0e-9;
    const double w = 2.0 * 3.141592653589793 * kBandwidthHz * periodSeconds;
    const double b = 1.4142135623730951 * w, c = w * w;
    predictedNs_ = predicted + b * e;
    double npf = anchor_.nsPerFrame + c * e / df;
    const double lo = nominalNsPerFrame_ * 0.999, hi = nominalNsPerFrame_ * 1.001; // ±1000 ppm
    anchor_.nsPerFrame = npf < lo ? lo : (npf > hi ? hi : npf);
    anchor_.frame = frame;
    anchor_.hostNs = static_cast<std::int64_t>(std::llround(predictedNs_));
}

void MidiInScheduler::schedule(const MidiEvent& e, double at) noexcept
{
    if (numPending_ == kMaxPending)
    {
        ++dropped_;
        return;
    }
    pending_[static_cast<std::size_t>(numPending_++)] = {e, at, seq_++};
}

void MidiInScheduler::collect(core::midi::MidiHub& hub, std::uint32_t portMask, const AudioClock& clock,
                              std::int64_t blockStart, int latency) noexcept
{
    for (int p = 0; p < core::midi::kMaxPorts; ++p)
    {
        if (! (portMask >> p & 1u))
            continue;
        MidiEvent e;
        for (int n = 0; n < static_cast<int>(core::midi::kInQueue) && hub.popIn(p, e); ++n)
        {
            ++received_;
            schedule(e, clock.frameOf(e.hostTimeNs) + static_cast<double>(latency));
        }
    }
    // The due events of this block: insertion into due_ by (time, arrival order); the pending list stays unordered
    // (a few events per block: O(n·k) with tiny n).
    numDue_ = 0;
    const double end = static_cast<double>(blockStart + kBlockFrames);
    const double start = static_cast<double>(blockStart);
    int kept = 0;
    for (int i = 0; i < numPending_; ++i)
    {
        Pending& p = pending_[static_cast<std::size_t>(i)];
        if (p.at >= end - 1e-3 || numDue_ == kMaxDue)
        {
            pending_[static_cast<std::size_t>(kept++)] = p;
            continue;
        }
        double at = p.at;
        if (at < start - 1e-3) // 1e-3 frame: host-time rounding (ns) is not lateness
        {
            ++late_;
            at = start;
        }
        int f = static_cast<int>(std::floor(at - start + 1e-3));
        f = f < 0 ? 0 : (f > kBlockFrames - 1 ? kBlockFrames - 1 : f);
        DueEvent d {p.event, p.at, f}; // exactFrame unclamped: a late tick keeps its real time (the follower)
        int j = numDue_++;
        while (j > 0 && due_[static_cast<std::size_t>(j - 1)].exactFrame > p.at)
        {
            due_[static_cast<std::size_t>(j)] = due_[static_cast<std::size_t>(j - 1)];
            --j;
        }
        due_[static_cast<std::size_t>(j)] = d;
    }
    numPending_ = kept;
}
} // namespace engine::midi
