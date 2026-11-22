// The UI's one view of the engine's audio → UI feeds (engine/EngineApi.h; 01-architecture/threading.md "Audio → UI"):
// the MeterFeed / PlayheadFeed snapshots and the lossless EngineEvent queue, read once per UI frame by the Shell and
// handed to every body widget (ShellApi::feeds(), WidgetArgs::feeds). Per-track levels in dB with the UI-side decay
// (threading.md "Timing": 20 dB per motion.meter-release), peak hold (motion.peak-hold), a 2 s clip latch, trigger
// flashes detected from the meter rise (UID-503), the per-track play positions (PLY-045), and the frame's engine
// events (recorded hits, switch done … drained from EngineBridge::popEvent, at most kMaxEvents per frame; the rest
// stays queued for the next frame — nothing is lost).
//
// Unattached (no bridge: tests, the host preview), the widgets fall back to the shell's UiState (master meter,
// pad-press flashes); a test can inject snapshots and events instead. No allocation; message thread only.
#pragma once

#include "engine/EngineApi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ui
{
struct UiState;

float toDb(float linear) noexcept; // linear peak → dBFS, −120 for silence

class EngineFeeds
{
public:
    static constexpr std::size_t kMaxEvents = 256; // per frame (the SPSC queue keeps the rest)
    static constexpr double kClipLatchMs = 2000;   // UI-216: the clip indicator latches 2 s (no token)

    void attach(engine::EngineBridge* bridge) noexcept { bridge_ = bridge; }
    const engine::EngineBridge* bridge() const noexcept { return bridge_; }
    // Test / host hook: the snapshots used while no bridge is attached.
    void inject(const engine::MeterFeed& m, const engine::PlayheadFeed& p) noexcept
    {
        injectedMeters_ = m;
        injectedPlayhead_ = p;
        injected_ = true;
    }
    // Test hook: an event delivered with the next poll (dropped when the frame's buffer is full).
    void injectEvent(const engine::EngineEvent& e) noexcept
    {
        if (pendingCount_ < pending_.size())
            pending_[pendingCount_++] = e;
    }
    void reset() noexcept;

    // Reads the feeds and drains the event queue once per frame (a second call with the same nowMs does nothing).
    // `state` may be null. The Shell calls it at the start of every frame.
    void poll(double nowMs, const UiState* state) noexcept;
    bool live() const noexcept { return bridge_ != nullptr || injected_; }

    struct Level
    {
        float db[2] = {-120.0f, -120.0f}, peakDb[2] = {-120.0f, -120.0f};
        double peakAtMs[2] = {-1e9, -1e9}, clipAtMs[2] = {-1e9, -1e9};
    };
    const Level& track(int track1Based) const noexcept
    {
        return tracks_[static_cast<std::size_t>((track1Based - 1) & 15)];
    }
    const Level& master() const noexcept { return master_; }
    const Level& send() const noexcept { return send_; }
    double lastHitMs(int track1Based) const noexcept
    {
        return hitMs_[static_cast<std::size_t>((track1Based - 1) & 15)];
    }
    // PLY-045: play position 0–1 of the track's voice, negative = idle.
    float position(int track1Based) const noexcept
    {
        return playhead_.trackPosition[static_cast<std::size_t>((track1Based - 1) & 15)];
    }
    const engine::PlayheadFeed& playhead() const noexcept { return playhead_; }
    const engine::MeterFeed& meters() const noexcept { return meters_; }
    // The engine events drained by this frame's poll (valid until the next poll).
    std::span<const engine::EngineEvent> events() const noexcept { return {events_.data(), eventCount_}; }
    std::uint64_t frames() const noexcept { return polls_; }
    std::uint64_t eventsSeen() const noexcept { return eventsSeen_; }

private:
    engine::EngineBridge* bridge_ = nullptr;
    engine::MeterFeed injectedMeters_{}, meters_{};
    engine::PlayheadFeed injectedPlayhead_{};
    bool injected_ = false;
    double lastPollMs_ = -1e18;
    std::uint64_t polls_ = 0, eventsSeen_ = 0;
    std::array<Level, 16> tracks_{};
    std::array<double, 16> hitMs_{};
    Level master_{}, send_{};
    engine::PlayheadFeed playhead_{};
    std::array<engine::EngineEvent, kMaxEvents> events_{}, pending_{};
    std::size_t eventCount_ = 0, pendingCount_ = 0;
};
} // namespace ui
