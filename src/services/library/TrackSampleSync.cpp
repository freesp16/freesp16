#include "services/library/TrackSampleSync.h"

#include "core/model/Project.h"

#include <cstdio>
#include <cstring>

namespace services::library
{
bool parseSamplePath(const std::string& path, int& scene, int& track) noexcept
{
    int s = -1, t = -1, used = 0;
    if (std::sscanf(path.c_str(), "scene[%d].track[%d].playback.sample%n", &s, &t, &used) != 2 ||
        used != static_cast<int>(path.size()))
        return false;
    if (s < 0 || s >= TrackSampleSync::kScenes || t < 0 || t >= TrackSampleSync::kTracks)
        return false;
    scene = s, track = t;
    return true;
}

TrackSampleSync::TrackSampleSync(SamplePool& pool, engine::EngineBridge& bridge) : pool_(pool), bridge_(bridge) {}

TrackSampleSync::~TrackSampleSync()
{
    *alive_ = false;
    for (auto& s : slots_)
        if (s.buffer != nullptr)
            core::audio::release(s.buffer);
}

void TrackSampleSync::onChange(const core::ChangeSet& changes, const core::model::Project& project)
{
    if (changes.touches("sampleRefs"))
    {
        syncAll(project); // a relink may move any slot's file
        return;
    }
    for (const auto& p : changes.paths())
    {
        int s = 0, t = 0;
        if (parseSamplePath(p, s, t))
            sync(s, t, project);
    }
}

void TrackSampleSync::syncAll(const core::model::Project& project)
{
    for (int s = 0; s < kScenes; ++s)
        for (int t = 0; t < kTracks; ++t)
            sync(s, t, project);
}

void TrackSampleSync::sync(int scene, int track, const core::model::Project& project)
{
    Slot& sl = slot(scene, track);
    const auto& key =
        project.scenes[static_cast<std::size_t>(scene)].tracks[static_cast<std::size_t>(track)].playback.sample;
    const core::model::SampleRef* ref = key.empty() ? nullptr : project.sampleRefs.find(key);
    const std::string path = ref != nullptr ? ref->path : std::string();
    if (sl.key == key.id && sl.path == path)
        return;
    sl.key = key.id;
    sl.path = path;
    const std::uint32_t generation = ++sl.generation;
    if (key.empty())
    {
        sl.loading = false;
        install(scene, track, nullptr);
        return;
    }
    if (ref == nullptr)
    {
        sl.loading = false;
        install(scene, track, nullptr);
        if (onError)
            onError(scene, track, core::Error{"Sample missing from the project's pool", key.id});
        return;
    }
    sl.loading = true;
    std::weak_ptr<bool> alive = alive_;
    pool_.request(*ref,
                  [this, alive, scene, track, generation](core::Result<Handle> r, const LoadInfo&)
                  {
                      if (alive.expired())
                          return;
                      Slot& now = slot(scene, track);
                      if (now.generation != generation)
                          return; // superseded by a later change (or its undo)
                      now.loading = false;
                      if (! r)
                      {
                          install(scene, track, nullptr);
                          if (onError)
                              onError(scene, track, r.error());
                          return;
                      }
                      install(scene, track, r.value());
                  });
}

void TrackSampleSync::install(int scene, int track, Handle buffer)
{
    Slot& sl = slot(scene, track);
    if (sl.buffer == buffer)
        return;
    core::audio::retain(buffer); // the document's use
    const Handle old = sl.buffer;
    sl.buffer = buffer;
    core::audio::retain(buffer); // the engine's slot reference (released by its TrackSample destroy function)
    const engine::ObjectSwap swap{engine::SwapSlot{engine::SwapSlot::TrackSample, static_cast<std::uint8_t>(scene),
                                                   static_cast<std::uint8_t>(track)},
                                  buffer};
    if (! bridge_.post(swap))
        core::audio::release(buffer);
    core::audio::release(old);
    if (onInstalled)
        onInstalled(scene, track, buffer);
}

void TrackSampleSync::replace(Handle oldBuffer, Handle newBuffer)
{
    if (oldBuffer == nullptr || oldBuffer == newBuffer)
        return;
    for (int s = 0; s < kScenes; ++s)
        for (int t = 0; t < kTracks; ++t)
            if (slot(s, t).buffer == oldBuffer)
                install(s, t, newBuffer);
}
} // namespace services::library
