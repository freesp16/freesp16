#include "ui/shell/Feeds.h"

#include "ui/shell/UiState.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
constexpr float kFlashRiseDb = 6.0f; // UID-503: a rise of ≥ 6 dB within one frame above −40 dB = a trigger
constexpr float kFlashFloorDb = -40.0f;
constexpr float kClipDb = -0.05f; // ≥ −0.05 dBFS counts as clipped

void decay(EngineFeeds::Level& l, const float in[2], double nowMs, double frameMs) noexcept
{
    // Meters decay in the UI (threading.md): 20 dB per motion.meter-release.
    const float fall = 20.0f * static_cast<float>(frameMs) / static_cast<float>(tk::motionMeterReleaseMs);
    for (int ch = 0; ch < 2; ++ch)
    {
        l.db[ch] = std::max(in[ch], l.db[ch] - fall);
        if (in[ch] >= l.peakDb[ch] || nowMs - l.peakAtMs[ch] > tk::motionPeakHoldMs)
        {
            l.peakDb[ch] = in[ch];
            l.peakAtMs[ch] = nowMs;
        }
        if (in[ch] >= kClipDb)
            l.clipAtMs[ch] = nowMs;
    }
}
} // namespace

float toDb(float linear) noexcept
{
    return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f;
}

void EngineFeeds::reset() noexcept
{
    auto* bridge = bridge_;
    *this = EngineFeeds{};
    bridge_ = bridge;
}

void EngineFeeds::poll(double nowMs, const UiState* state) noexcept
{
    if (nowMs == lastPollMs_)
        return;
    const double frameMs = lastPollMs_ < -1e17 ? 1000.0 / 60.0 : std::clamp(nowMs - lastPollMs_, 0.0, 100.0);
    lastPollMs_ = nowMs;
    ++polls_;

    // Events: the injected ones first, then the engine's queue up to the frame's capacity.
    eventCount_ = 0;
    for (std::size_t i = 0; i < pendingCount_ && eventCount_ < events_.size(); ++i)
        events_[eventCount_++] = pending_[i];
    pendingCount_ = 0;
    if (bridge_ != nullptr)
        while (eventCount_ < events_.size() && bridge_->popEvent(events_[eventCount_]))
            ++eventCount_;
    eventsSeen_ += eventCount_;

    const engine::MeterFeed* m = bridge_ != nullptr ? &bridge_->meters() : (injected_ ? &injectedMeters_ : nullptr);
    if (bridge_ != nullptr)
        playhead_ = bridge_->playhead();
    else if (injected_)
        playhead_ = injectedPlayhead_;
    if (m != nullptr)
    {
        meters_ = *m;
        for (std::size_t t = 0; t < 16; ++t)
        {
            const float in[2] = {toDb(m->trackPeak[t][0]), toDb(m->trackPeak[t][1])};
            const float before = std::max(tracks_[t].db[0], tracks_[t].db[1]);
            const float now = std::max(in[0], in[1]);
            if (now > kFlashFloorDb && now - before >= kFlashRiseDb)
                hitMs_[t] = nowMs;
            decay(tracks_[t], in, nowMs, frameMs);
        }
        const float master[2] = {toDb(m->masterPeak[0]), toDb(m->masterPeak[1])};
        const float send[2] = {toDb(m->sendPeak[0]), toDb(m->sendPeak[1])};
        decay(master_, master, nowMs, frameMs);
        decay(send_, send, nowMs, frameMs);
    }
    else if (state != nullptr) // no feed: the shell's master meter
        for (int ch = 0; ch < 2; ++ch)
        {
            master_.db[ch] = state->masterDb[ch];
            master_.peakDb[ch] = state->masterPeakDb[ch];
        }
    if (state != nullptr)
        for (std::size_t t = 0; t < 16; ++t)
            hitMs_[t] = std::max(hitMs_[t], state->tracks[t].lastHitMs); // pad presses (shell)
}
} // namespace ui
