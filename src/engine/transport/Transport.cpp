#include "engine/transport/Transport.h"

#include "core/util/musictime/MusicTime.h"

// The position arithmetic (position(), rebase()'s two-sum) must not be re-associated. The device build
// compiles the engine with -funsafe-math-optimizations (cmake/TargetFlags.cmake), which would fold the two-sum's
// error term to 0 (measured: 2e-9 quarter after 1 h with a tempo change every block). This file opts out; it does no
// per-sample work, so it loses nothing.
#if defined(__GNUC__) && ! defined(__clang__)
#pragma GCC optimize("no-unsafe-math-optimizations", "no-associative-math", "no-reciprocal-math", "fp-contract=off")
#endif

namespace engine::transport
{
namespace mt = core::musictime;

double Transport::position(std::int64_t extraFrames) const noexcept
{
    return anchorHi_ + (anchorLo_ + static_cast<double>(frames_ + extraFrames) * ppf_);
}

void Transport::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    state_ = State::Stopped;
    wasPlaying_ = false;
    stopPending_ = stopEdge_ = StopAction::None;
    locatedPending_ = locatedEdge_ = preCountBlock_ = false;
    startFrame_ = 0;
    locate(0.0);
    updateRate();
}

void Transport::rebase() noexcept
{
    if (frames_ == 0)
        return;
    // The new anchor is the position the last block reported as its end (hi = p, so the next block's p0 is p1 exactly)
    // plus the remainder r = (hi + lo + d) − p that p's rounding dropped (lo = r, carried forward: no drift).
    const double d = static_cast<double>(frames_) * ppf_;
    const double p = anchorHi_ + (anchorLo_ + d);
    const double s = anchorHi_ + d; // two-sum: s + e == hi + d exactly
    const double bb = s - anchorHi_;
    const double e = (anchorHi_ - (s - bb)) + (d - bb);
    double r = (s - p) + (e + anchorLo_); // s − p is exact (Sterbenz: s and p differ by a few ulps at most)
    if (p + r != p)
        r *= 0.5; // keep hi + lo == hi (|r| exceeds half an ulp by ≤ ulp(d) in rare ties; costs < 1e-13 quarter)
    if (p + r != p)
        r = 0.0;
    anchorHi_ = p;
    anchorLo_ = r;
    frames_ = 0;
}

void Transport::locate(double ppq) noexcept
{
    anchorHi_ = ppq;
    anchorLo_ = 0.0;
    frames_ = 0;
}

void Transport::updateRate() noexcept
{
    ppf_ = mt::ppqPerFrame(effectiveBpm(), sampleRate_);
}

void Transport::setTempo(double bpm) noexcept
{
    const double b = mt::clampBpm(bpm);
    if (b == bpm_)
        return;
    rebase();
    bpm_ = b;
    updateRate();
}

void Transport::setNudge(int direction, float amount) noexcept
{
    const int dir = direction > 0 ? 1 : (direction < 0 ? -1 : 0);
    const float a = amount < 0.0f ? 0.0f : (amount > 0.5f ? 0.5f : amount);
    if (dir == nudgeDir_ && a == nudgeAmount_)
        return;
    rebase();
    nudgeDir_ = dir;
    nudgeAmount_ = a;
    updateRate();
}

void Transport::stopAtZero() noexcept
{
    if (! (anchorHi_ == 0.0 && anchorLo_ == 0.0 && frames_ == 0))
        locatedPending_ = true;
    locate(0.0);
    state_ = State::Stopped;
}

void Transport::command(const TransportCmd& c) noexcept
{
    const double v = c.value == c.value ? c.value : 0.0; // NaN = 0
    switch (c.kind)
    {
        case TransportCmd::Play:
        {
            if (state_ == State::Playing || state_ == State::PreCount)
                break;
            const int bars = v < 0.5 ? 0 : (v < 1.5 ? 1 : kMaxPreCountBars);
            locate(-mt::kQuartersPerBar * bars);
            locatedPending_ = true;
            state_ = bars > 0 ? State::PreCount : State::Playing;
            break;
        }
        case TransportCmd::Continue:
            if (state_ == State::Stopped || state_ == State::Paused)
                state_ = State::Playing; // the anchor holds the paused / located position
            break;
        case TransportCmd::Pause: command({TransportCmd::Stop, 0.0}); break;
        case TransportCmd::HardStop: command({TransportCmd::Stop, 2.0}); break;
        case TransportCmd::Stop:
        {
            const int kind = v < 0.5 ? 0 : (v < 1.5 ? 1 : 2);
            if (kind == 0)
            {
                if (state_ == State::Playing)
                {
                    rebase(); // freeze the position: Continue resumes on exactly this PPQ
                    state_ = State::Paused;
                }
                else if (state_ == State::PreCount)
                    stopAtZero();
            }
            else
            {
                if (kind == 2 || state_ == State::Stopped)
                    stopPending_ = StopAction::All;
                else if (stopPending_ == StopAction::None)
                    stopPending_ = StopAction::Loops;
                stopAtZero();
            }
            break;
        }
        case TransportCmd::SetTempo: setTempo(v); break;
        case TransportCmd::Locate:
            if (state_ == State::PreCount)
                break;
            locate(v > 0.0 ? v : 0.0);
            locatedPending_ = true;
            break;
    }
}

void Transport::advance(BlockContext& ctx) noexcept
{
    stopEdge_ = stopPending_;
    stopPending_ = StopAction::None;
    locatedEdge_ = locatedPending_;
    locatedPending_ = false;
    startFrame_ = 0;
    preCountBlock_ = false;

    const bool moving = state_ == State::Playing || state_ == State::PreCount;
    if (moving && correction_ != nullptr)
    {
        double b = effectiveBpm(), phase = 0.0;
        correction_(correctionUser_, position(0), b, phase);
        if (phase != 0.0)
        {
            locate(position(0) + phase);
            locatedEdge_ = true;
        }
        const double base = 1.0 + nudgeDir_ * static_cast<double>(nudgeAmount_);
        const double corrected = b > 0.0 ? b / base : 0.0;
        if (b != effectiveBpm() && corrected != syncBpm_)
        {
            rebase();
            syncBpm_ = corrected;
            updateRate();
        }
    }

    const double p0 = position(0);
    double p1 = p0;
    if (moving)
    {
        p1 = position(kBlockFrames);
        frames_ += kBlockFrames;
        if (state_ == State::PreCount)
        {
            if (mt::inBlock(0.0, p0, ppf_, kBlockFrames) || p0 >= 0.0)
            {
                state_ = State::Playing; // the pre-count ends inside this block: playback starts on startFrame()
                startFrame_ = mt::frameOf(0.0, p0, ppf_);
            }
            else
                preCountBlock_ = true;
        }
    }

    const bool playing = state_ == State::Playing;
    ctx.ppqStart = p0;
    ctx.ppqEnd = p1;
    ctx.bpm = effectiveBpm();
    ctx.playing = playing;
    ctx.startedThisBlock = playing && ! wasPlaying_;
    ctx.stoppedThisBlock = ! playing && wasPlaying_;
    wasPlaying_ = playing;
}
} // namespace engine::transport
