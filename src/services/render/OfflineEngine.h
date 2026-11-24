// A private Engine run offline (spec 01-architecture/module-layout.md "services/render instantiates its
// own Engine and runs it offline (same code as playback)", audio-engine.md "Modes" Renderer; REN-001).
//
//   any non-audio thread (the render worker, the render CLI, the app's diagnostic `cmd render`):
//     OfflineEngine e({rate, ports});
//     e.begin(project, samples, feeder);   every value of the project, then one TrackSample swap per slot
//     e.bridge().post(...);                the caller's own set-up (scene queue, tempo, song mode)
//     e.settle();                          stopped: blocks until the paced queue is empty and the swaps installed
//     e.bridge().post(TransportCmd{Play}); e.process() per 64-frame block, e.out(port, ch) = that block
//
// The engine is engine::createEngine — the live engine's code, parameters bound the same way (EngineSetup from the
// built-in registry) — and never a device: nothing here touches the live audio thread. It is told render.offline = 1
// first (the TEMPORARY test tone stays off). Determinism: the same project,
// samples and post order give bit-identical output on the same build (no clock, no thread timing enters the audio:
// every post is flushed at a block boundary by the caller's thread, QueuedBridge budget 512 per block).
//
// Samples: a SampleSnapshot holds one reference per (scene, track) slot to buffers that are already decoded (the live
// TrackSampleSync's, or the CLI's own); the offline engine gets one more reference per installed slot (released by the
// engine's TrackSample destroy function), as TrackSampleSync does. The snapshot may be released on any thread
// (SampleBuffer::release is an atomic decrement; the pool frees zero-reference buffers on its own thread).
#pragma once

#include "core/audio/SampleBuffer.h"
#include "core/util/arrangement/ArrangementPlan.h"
#include "engine/EngineApi.h"
#include "services/render/QueuedBridge.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::model
{
struct Project;
}

namespace services::plugin
{
class ClapHostService;
}

namespace services::library
{
class TrackSampleSync;
}

namespace services::render
{
using SampleHandle = const core::audio::SampleBuffer*;

// One retained buffer per (scene, track) slot, null = empty.
class SampleSnapshot
{
public:
    static constexpr int kSlots = 256;
    SampleSnapshot() = default;
    ~SampleSnapshot();
    SampleSnapshot(const SampleSnapshot&);
    SampleSnapshot& operator= (const SampleSnapshot&);
    SampleSnapshot(SampleSnapshot&&) noexcept;
    SampleSnapshot& operator= (SampleSnapshot&&) noexcept;

    void set(int scene, int track, SampleHandle) noexcept; // retains the new buffer, releases the old one
    SampleHandle at(int scene, int track) const noexcept { return slots_[index(scene, track)]; }
    int count() const noexcept;                            // slots holding a buffer

    // Message thread: the buffers the document uses now (TrackSampleSync::current).
    static SampleSnapshot of(const services::library::TrackSampleSync&);

private:
    static std::size_t index(int s, int t) noexcept { return static_cast<std::size_t>((s & 15) * 16 + (t & 15)); }
    void releaseAll() noexcept;
    std::array<SampleHandle, kSlots> slots_ {};
};

class SampleSnapshot;
// Posts every value of a project to an engine: the composition root's project forwarding (app/ProjectFeed.h —
// models, FX, patterns, sequencer, ribbon, scene switch, routing), the arrangement plan with `status` (null = none)
// and the slice tables of the snapshot's samples (a slice table needs its buffer's frame count). Called on the
// render thread with the job's own copy of the project, before the snapshot's TrackSample swaps are posted.
using ProjectFeeder = std::function<void(const core::model::Project&, engine::EngineBridge&,
                                         core::arrangement::SongStatus*, const SampleSnapshot&)>;

// The EngineSetup of the built-in parameter registry (the app's own set-up: every id bound, the FX routes).
engine::EngineSetup builtinEngineSetup();

class OfflineEngine
{
public:
    struct Config
    {
        double sampleRate = 44100.0;
        int outPorts = 1; // logical ports rendered (engine::OutPort order): 1 = Main, 2 = Main + OUT 3-4, …
        std::string pluginsDir{};
    };
    explicit OfflineEngine(Config);
    ~OfflineEngine();
    OfflineEngine(const OfflineEngine&) = delete;
    OfflineEngine& operator= (const OfflineEngine&) = delete;

    // Queues every value of `project` (feeder) and the snapshot's samples. Nothing runs yet.
    void begin(const core::model::Project&, const SampleSnapshot&, const ProjectFeeder&,
               core::arrangement::SongStatus* status = nullptr);
    // Stopped blocks until the queue is empty (≥ 8 blocks), then 8 more (the strip ramps settle). Returns the number
    // of blocks run before the 8 (the diagnostic's "preroll"). Engine events of the set-up are dropped.
    int settle();

    QueuedBridge& bridge() noexcept { return queue_; }
    engine::AudioEngine& engine() noexcept { return *engine_; }
    const engine::PlayheadFeed& playhead() const { return engine_->bridge().playhead(); }
    bool popEvent(engine::EngineEvent& e) { return engine_->bridge().popEvent(e); }

    // One 64-frame block: the queue's next period, then Engine::process.
    void process() noexcept;
    const float* out(int port, int ch) const noexcept
    {
        return buffers_[static_cast<std::size_t>(2 * port + (ch & 1))].data();
    }
    double sampleRate() const noexcept { return config_.sampleRate; }
    int outPorts() const noexcept { return config_.outPorts; }
    std::uint64_t blocks() const noexcept { return blocks_; }

private:
    Config config_;
    std::unique_ptr<engine::AudioEngine> engine_;
    QueuedBridge queue_;
    std::unique_ptr<plugin::ClapHostService> plugins_;
    std::vector<std::array<float, engine::kBlockFrames>> buffers_;
    std::vector<float*> pointers_;
    std::uint64_t blocks_ = 0;
};

// FNV-1a 64 over bytes, continuing from `h` (the render check's hash of the float output, block by block).
std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t h = 1469598103934665603ull) noexcept;
} // namespace services::render
