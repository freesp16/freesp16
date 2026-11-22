// Load to track (LIB-012, LIB-013; UI-410): the BROWSE screen's push on a file. The file is decoded into the pool on
// a worker (its content id computed for the new SampleRef), then core::commands::library::loadSample runs on the
// CommandBus — one undo step; the bridge hook (TrackSampleSync) swaps the buffer into the engine without stopping the
// transport. LOAD TO = FOLDER → TRACKS loads the folder's first 16 audio files (A→Z) onto tracks 1–16. "Load to the
// pad pressed next" arms a file; the next onPadPressed(track) loads it there.
// Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "core/commands/library/LibraryCommands.h"
#include "core/util/Result.h"
#include "services/library/SampleLibrary.h"
#include "services/library/SamplePool.h"

#include <functional>
#include <memory>

namespace services::library
{
enum class LoadTarget : std::uint8_t
{
    Track,
    NextPad
};

class TrackLoader
{
public:
    using Done = std::function<void(core::Result<void>, const LoadInfo&)>;

    TrackLoader(SampleLibrary&, SamplePool&, core::CommandBus&);
    ~TrackLoader();
    TrackLoader(const TrackLoader&) = delete;
    TrackLoader& operator=(const TrackLoader&) = delete;

    void load(int scene, int track, const juce::File& file, Done done = {});
    void loadFolder(int scene, const juce::File& folder, std::function<void(core::Result<void>)> done = {});

    void arm(const juce::File& file) { armed_ = file; } // LoadTarget::NextPad
    bool armed() const noexcept { return armed_ != juce::File(); }
    void disarm() { armed_ = juce::File(); }
    bool onPadPressed(int scene, int track, Done done = {}); // true when an armed file was loaded

    // The SampleRef a file gets: its alias path in the library tree (absolute when outside every location), the
    // file name as display name, `id` = the content id.
    core::model::SampleRef refFor(const juce::File&, const std::string& contentId) const;
    static core::commands::library::LoadDefaults defaultsFor(const core::audio::SampleBuffer&, const LoadInfo&);

private:
    SampleLibrary& library_;
    SamplePool& pool_;
    core::CommandBus& bus_;
    juce::File armed_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace services::library
