// The BROWSE / PLAYBACK screens' local views (ui/screens/browse/BrowseSource.h, ui/screens/playback/SampleViews.h)
// over the real sample library (services/library) and the document model. Only the composition root sees
// both sides (ui and services never include each other), so the adapters live in app/. Message thread only.
//   LibraryBrowseSource  BROWSE: roots/list/search/free space from SampleLibrary, LOAD through TrackLoader (one undoable
//                        loadSample command; TrackSampleSync swaps the buffer in), preview through PreviewService.
//                        On the device BROWSE shows only the sampler's sample folders, as the stock firmware does:
//                        "Samples [INTERNAL]" (the internal disk's stock Samples, "$Ext:INTERNAL") and "Samples [USB]"
//                        (a stick's PIONEER DJ SAMPLER/Samples); not the app's own library folders, a stick's root or
//                        any other mount (services::library::samplerFolders). The library's
//                        locations (and so the sample paths projects store) are unchanged; only the view is scoped.
//   SampleViewsAdapter   PLAYBACK / TRACK MENU waveforms: the track's buffer (TrackSampleSync) and its overview pyramid
//                        (SamplePool::overview), converted once per buffer into ui::WaveOverview
#pragma once

#include "core/commands/Command.h"
#include "services/library/PreviewService.h"
#include "services/library/SampleLibrary.h"
#include "services/library/SamplePool.h"
#include "services/library/TrackLoader.h"
#include "services/library/TrackSampleSync.h"
#include "ui/screens/browse/BrowseSource.h"
#include "ui/screens/playback/SampleViews.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace app
{
class LibraryBrowseSource final : public ui::m1::BrowseSource
{
public:
    LibraryBrowseSource(services::library::SampleLibrary&, services::library::TrackLoader&,
                        services::library::PreviewService&, core::CommandBus&, bool samplerFoldersOnly = false);
    ~LibraryBrowseSource() override;

    std::vector<ui::m1::BrowseEntry> roots() override;
    void list(const std::string& path, ui::m1::BrowseSort,
              std::function<void(std::vector<ui::m1::BrowseEntry>)> done) override;
    void search(const std::string& text, std::function<void(std::vector<ui::m1::BrowseEntry>)> done) override;
    std::string freeSpace(const std::string& path) override;
    const std::string& assigned(int track1Based) override;
    void load(int track1Based, const std::string& path, std::function<void(float)> progress,
              std::function<void(bool ok, std::string message)> done) override;
    void unload(int track1Based) override;
    void preview(const std::string& path) override;
    bool previewing() const override;

private:
    int scene() const noexcept;
    std::vector<ui::m1::BrowseEntry> samplerRoots() const; // the device's roots (see above)
    services::library::SampleLibrary& library_;
    services::library::TrackLoader& loader_;
    services::library::PreviewService& preview_;
    core::CommandBus& bus_;
    bool samplerFoldersOnly_ = false;
    std::array<std::string, 16> assigned_{};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

class SampleViewsAdapter
{
public:
    SampleViewsAdapter(services::library::SamplePool&, services::library::TrackSampleSync&, core::CommandBus&);
    ~SampleViewsAdapter(); // uninstalls the provider
    void install();
    ui::m1::SampleView of(int track1Based);
    void collect(); // drops converted overviews of buffers no track uses any more (once per second)

private:
    struct Converted
    {
        std::vector<ui::WaveOverview::Level> levels;
        ui::WaveOverview overview;
    };
    services::library::SamplePool& pool_;
    services::library::TrackSampleSync& sync_;
    core::CommandBus& bus_;
    std::map<services::library::SamplePool::Handle, std::unique_ptr<Converted>> cache_;
};
} // namespace app
