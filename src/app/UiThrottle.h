// The display steps down under audio load (threading.md "Threads": the display is the least important work). The frame
// loop asks once per frame whether to draw; while the engine is close to its deadline the UI draws every second frame
// (30 Hz instead of 60), giving its core — and the GPU driver's and X's share of it — back. Input is still read, and
// the document still runs, every frame.
//
// It steps down when, since the last sample, the audio device counted an xrun, the engine counted an overrun (a block
// slower than real time), or the engine's p95 block time reached `loadLimit` of the block period; it steps back up
// `holdMs` after the last such sample. The first sample only sets the baseline counts (start-up xruns do not count).
// Message thread only.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace app
{
class UiThrottle
{
public:
    struct Sample
    {
        double p95Fraction = 0; // the engine's p95 block time / the block period
        std::uint64_t xruns = 0, overruns = 0;
    };
    enum class Change
    {
        None,
        On,
        Off
    };

    explicit UiThrottle(double loadLimit = 0.8, double holdMs = 3000.0) noexcept
        : loadLimit_(loadLimit), holdMs_(holdMs)
    {
    }

    // Once per frame; `sample` = null when no new sample was taken this frame. True = draw this frame.
    bool draw(double nowMs, const Sample* sample)
    {
        if (sample != nullptr)
            update(nowMs, *sample);
        if (throttled_ && nowMs - lastTriggerMs_ >= holdMs_)
        {
            throttled_ = false;
            change_ = Change::Off;
        }
        if (! throttled_)
            return true;
        return (++frames_ & 1u) == 0;
    }

    bool throttled() const noexcept { return throttled_; }
    // The last step down's cause ("xrun +2", "overrun +1", "load p95 83%") and when it started.
    const std::string& reason() const noexcept { return reason_; }
    double sinceMs() const noexcept { return sinceMs_; }
    // The transition since the last call (for one log line each way).
    Change takeChange() noexcept
    {
        const auto c = change_;
        change_ = Change::None;
        return c;
    }

private:
    void update(double nowMs, const Sample& s)
    {
        if (! primed_)
        {
            primed_ = true;
            xruns_ = s.xruns;
            overruns_ = s.overruns;
            return;
        }
        char why[48] = {};
        if (s.xruns > xruns_)
            std::snprintf(why, sizeof why, "xrun +%llu", static_cast<unsigned long long>(s.xruns - xruns_));
        else if (s.overruns > overruns_)
            std::snprintf(why, sizeof why, "overrun +%llu", static_cast<unsigned long long>(s.overruns - overruns_));
        else if (s.p95Fraction >= loadLimit_)
            std::snprintf(why, sizeof why, "load p95 %.0f%%", s.p95Fraction * 100.0);
        xruns_ = s.xruns;
        overruns_ = s.overruns;
        if (why[0] == 0)
            return;
        lastTriggerMs_ = nowMs;
        if (! throttled_)
        {
            throttled_ = true;
            reason_ = why;
            sinceMs_ = nowMs;
            change_ = Change::On;
        }
    }

    double loadLimit_, holdMs_;
    bool primed_ = false, throttled_ = false;
    std::uint64_t xruns_ = 0, overruns_ = 0;
    double lastTriggerMs_ = 0, sinceMs_ = 0;
    unsigned frames_ = 0;
    std::string reason_;
    Change change_ = Change::None;
};
} // namespace app
