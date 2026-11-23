// Real-time audio contract for hosted plugins (SwapSlot::TrackPlugin).
//
// Lives in core/audio/ so engine/ consumes it with zero knowledge of dynamic loading or plugin SDKs,
// while services/plugin/ implements it around dynamically loaded CLAP instances.
//
// Lifetime: intrusive atomic reference count (mirrors core::audio::SampleBuffer).
//   - The plugin service owns one reference while the plugin is mounted on a track.
//   - Posting ObjectSwap{SwapSlot::TrackPlugin, ...} retains one reference for the engine.
//   - When replaced or cleared, Engine's RetireQueue invokes releasePluginNode() on the message thread.
//   - The instance is destroyed only when the last reference drops (always on the message thread).
#pragma once

#include <atomic>
#include <cstdint>

namespace core::audio
{
struct PluginProcessContext
{
    double sampleRate = 44100.0;
    double bpm = 120.0;
    double ppqStart = 0.0;
    double ppqEnd = 0.0;
    int frames = 64;
    bool playing = false;
    bool startedThisBlock = false;
    bool stoppedThisBlock = false;
};

class PluginAudioNode
{
public:
    virtual ~PluginAudioNode() = default;

    void retain() const noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }
    void release() const noexcept
    {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
    }

    // ---- audio thread (strictly real-time: noexcept, no alloc, no locks, no I/O) ----
    virtual void noteOn(int frame, int note, float velocity) noexcept = 0;
    virtual void noteOff(int frame, int note) noexcept = 0;
    virtual void allNotesOff(int frame) noexcept = 0;
    // Renders `ctx.frames` stereo samples into outL / outR (overwriting).
    // Returns false when silent / bypassed / idle (outL / outR left untouched).
    virtual bool process(const PluginProcessContext& ctx, float* outL, float* outR) noexcept = 0;
    virtual bool active() const noexcept = 0;

private:
    mutable std::atomic<int> refs_ {1};
};

// RetireQueue / SwapTable destroy callback (runs on the message thread).
inline void releasePluginNode(const void* ptr) noexcept
{
    if (ptr != nullptr)
        static_cast<const PluginAudioNode*>(ptr)->release();
}

// Non-const view for TrackSource on the audio thread (SwapTable stores type-erased const void*).
inline PluginAudioNode* asPluginNode(const void* ptr) noexcept
{
    return const_cast<PluginAudioNode*>(static_cast<const PluginAudioNode*>(ptr));
}
} // namespace core::audio
