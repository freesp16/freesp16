// Audio → UI feeds of the engine (threading.md "Audio → UI: triple-buffered snapshots"; audio-engine.md steps 13–14):
// the playhead and the meters are triple-buffered snapshots published once per device period; engine events are a
// lossless SPSC queue drained once per UI frame (EngineBridge::popEvent).
//
//   audio thread:   meters.addBlock(...) once per 64-frame block (NEON peak + sum of squares), then once per period
//                   publish(blockIndex, playhead) — wait-free, no allocation
//   message thread: playheadSnapshot() / meterSnapshot() / popEvent()  (EngineBridge)
//
// Meter semantics (EngineApi.h MeterFeed): linear peak and RMS of the last device period; the UI converts to dB and
// decays (threading.md "Timing": meters decay in the UI, not in the engine).
#pragma once

#include "engine/EngineApi.h"
#include "engine/dsp/simd/Kernels.h"
#include "engine/fx/FxSlotState.h"

#include "core/util/SpscQueue.h"
#include "core/util/TripleBuffer.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace engine::feeds
{
// Accumulates one device period of per-block meter values (audio thread only).
class MeterAccumulator
{
public:
    static constexpr int kChannels = 2;

    void reset() noexcept
    {
        masterPeak_ = {}, masterSum_ = {}, sendPeak_ = {}, inputPeak_ = {};
        for (auto& t : trackPeak_)
            t = {};
        frames_ = 0;
    }

    // One engine block of the master out (planar L/R, `frames` = kBlockFrames).
    void addMaster(const float* l, const float* r, int frames) noexcept
    {
        const auto a = simd::peakSum(l, frames), b = simd::peakSum(r, frames);
        masterPeak_[0] = a.peak > masterPeak_[0] ? a.peak : masterPeak_[0];
        masterPeak_[1] = b.peak > masterPeak_[1] ? b.peak : masterPeak_[1];
        masterSum_[0] += a.sumSquares;
        masterSum_[1] += b.sumSquares;
        frames_ += frames;
    }
    void addSend(const float* l, const float* r, int frames) noexcept { addPeaks(sendPeak_, l, r, frames); }
    void addInput(const float* l, const float* r, int frames) noexcept { addPeaks(inputPeak_, l, r, frames); }
    void addTrack(int track, const float* l, const float* r, int frames) noexcept
    {
        addPeaks(trackPeak_[static_cast<std::size_t>(track)], l, r, frames);
    }

    // Values the mixer already measured for this block (engine::mixer::MeterBlock), no second pass.
    void addMasterValues(float peakL, float peakR, float sumSqL, float sumSqR, int frames) noexcept
    {
        masterPeak_[0] = peakL > masterPeak_[0] ? peakL : masterPeak_[0];
        masterPeak_[1] = peakR > masterPeak_[1] ? peakR : masterPeak_[1];
        masterSum_[0] += sumSqL;
        masterSum_[1] += sumSqR;
        frames_ += frames;
    }
    void addSendPeak(float l, float r) noexcept { maxInto(sendPeak_, l, r); }
    void addTrackPeak(int track, float l, float r) noexcept { maxInto(trackPeak_[static_cast<std::size_t>(track)], l, r); }

    void setAfsOverLevel(float v) noexcept { afsOverLevel_ = v; } // already held by the slot: the latest value

    // Writes every field of the snapshot (TripleBuffer contract) and resets the accumulation.
    void fill(MeterFeed& m, std::uint64_t blockIndex) noexcept
    {
        m.blockIndex = blockIndex;
        m.masterPeak = masterPeak_;
        const float inv = frames_ > 0 ? 1.0f / static_cast<float>(frames_) : 0.0f;
        m.masterRms = {std::sqrt(masterSum_[0] * inv), std::sqrt(masterSum_[1] * inv)}; // once per period, not per sample
        m.sendPeak = sendPeak_;
        m.inputPeak = inputPeak_;
        m.trackPeak = trackPeak_;
        m.afsOverLevel = afsOverLevel_;
        reset();
    }

private:
    static void maxInto(std::array<float, 2>& p, float l, float r) noexcept
    {
        p[0] = l > p[0] ? l : p[0];
        p[1] = r > p[1] ? r : p[1];
    }
    static void addPeaks(std::array<float, 2>& p, const float* l, const float* r, int frames) noexcept
    {
        const float a = simd::peakAbs(l, frames), b = simd::peakAbs(r, frames);
        p[0] = a > p[0] ? a : p[0];
        p[1] = b > p[1] ? b : p[1];
    }

    std::array<float, 2> masterPeak_ {}, masterSum_ {}, sendPeak_ {}, inputPeak_ {};
    std::array<std::array<float, 2>, kTracks> trackPeak_ {};
    float afsOverLevel_ = 0.0f;
    int frames_ = 0;
};

// The three feeds the bridge exposes. One writer (audio thread), one reader (message thread) each.
class EngineFeeds
{
public:
    static constexpr std::size_t kEventQueueSize = 1024;

    // ---- audio thread ----
    MeterAccumulator& meters() noexcept { return accumulator_; }
    // Publishes the period's meters and the playhead (every field of `playhead` is copied).
    void publish(std::uint64_t blockIndex, const PlayheadFeed& playhead) noexcept
    {
        accumulator_.fill(meterFeed_.write(), blockIndex);
        meterFeed_.publish();
        PlayheadFeed& p = playheadFeed_.write();
        p = playhead;
        p.blockIndex = blockIndex;
        playheadFeed_.publish();
    }
    // Lossless event to the UI; false when the UI has not drained 1024 events (the event is dropped and counted).
    bool pushEvent(const EngineEvent& e) noexcept
    {
        if (events_.push(e))
            return true;
        ++droppedEvents_;
        return false;
    }
    std::uint32_t droppedEvents() const noexcept { return droppedEvents_; } // audio thread view
    // The effect panels' feed (FxSlotBank::publish fills every field), once per period after publish().
    fx::FxMeterFeed& fxWrite() noexcept { return fxFeed_.write(); }
    void fxPublish() noexcept { fxFeed_.publish(); }

    // ---- message thread ----
    const PlayheadFeed& playhead() const noexcept { return playheadFeed_.read(); }
    const MeterFeed& meterSnapshot() const noexcept { return meterFeed_.read(); }
    const fx::FxMeterFeed& fxSnapshot() const noexcept { return fxFeed_.read(); }
    bool popEvent(EngineEvent& out) noexcept { return events_.pop(out); }

private:
    MeterAccumulator accumulator_;
    core::TripleBuffer<MeterFeed> meterFeed_;
    core::TripleBuffer<PlayheadFeed> playheadFeed_;
    core::TripleBuffer<fx::FxMeterFeed> fxFeed_;
    core::SpscQueue<EngineEvent, kEventQueueSize> events_;
    std::uint32_t droppedEvents_ = 0;
};
} // namespace engine::feeds
