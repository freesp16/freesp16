// Keeps the engine's track samples in step with the document (LIB-012/030; threading.md "Object swap protocol"):
// whenever a command (or its undo, EDT-U4/U5) changes `scene[s].track[t].playback.sample` or the pool records
// (`sampleRefs`), the slot's sample is requested from the SamplePool and, once decoded, handed to the engine as
// ObjectSwap{SwapSlot{TrackSample, s, t}, buffer} through EngineBridge::post. The transport never stops (UI-410):
// the old buffer plays until the new one is installed at a block start.
//
// References: the sync holds the document's one reference per slot that uses a buffer (the pool's "document use"),
// and retains one more for the engine with every posted swap (the engine's destroy function for TrackSample slots
// releases it). A refused post releases that reference again.
//
// Wiring: the composition root calls onChange() from the CommandBus bridge hook (core::commands::setBridge) next to
// the parameter translator, and syncAll() after a project is opened. Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "core/util/Result.h"
#include "engine/EngineApi.h"
#include "services/library/SamplePool.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace core::model
{
struct Project;
}

namespace services::library
{
class TrackSampleSync
{
public:
    using Handle = SamplePool::Handle;
    static constexpr int kScenes = 16, kTracks = 16;

    TrackSampleSync(SamplePool&, engine::EngineBridge&);
    ~TrackSampleSync(); // gives back the document references (the engine keeps its own)
    TrackSampleSync(const TrackSampleSync&) = delete;
    TrackSampleSync& operator=(const TrackSampleSync&) = delete;

    void onChange(const core::ChangeSet&, const core::model::Project&);
    void syncAll(const core::model::Project&, bool replacingProject = false);
    // Live takes are outside the project. A null buffer clears the actual audio slot.
    bool installLive(int scene, int track, Handle);

    // The buffer the document uses at (scene, track): nullptr while empty or loading.
    Handle current(int scene, int track) const noexcept { return slot(scene, track).buffer; }
    bool loading(int scene, int track) const noexcept { return slot(scene, track).loading; }
    // Moves every slot holding `oldBuffer` to `newBuffer` (SamplePool::onReplaced; LIB-031).
    void replace(Handle oldBuffer, Handle newBuffer);

    std::function<void(int scene, int track, const core::Error&)> onError;
    std::function<void(int scene, int track, Handle)> onInstalled;

private:
    struct Slot
    {
        std::string key, path; // SampleKey id and its pool path, as last synced
        Handle buffer = nullptr;
        std::uint32_t generation = 0;
        bool loading = false, live = false;
    };
    Slot& slot(int s, int t) noexcept { return slots_[static_cast<std::size_t>((s & 15) * 16 + (t & 15))]; }
    const Slot& slot(int s, int t) const noexcept { return slots_[static_cast<std::size_t>((s & 15) * 16 + (t & 15))]; }
    void sync(int scene, int track, const core::model::Project&, bool replacingProject = false);
    bool install(int scene, int track, Handle);

    SamplePool& pool_;
    engine::EngineBridge& bridge_;
    std::array<Slot, kScenes * kTracks> slots_{};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// "scene[3].track[7].playback.sample" → (3, 7); false for any other path.
bool parseSamplePath(const std::string& path, int& scene, int& track) noexcept;
} // namespace services::library
