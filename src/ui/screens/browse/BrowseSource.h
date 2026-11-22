// What BROWSE needs from the sample library (LIB-001…018, UI-310…315). A local adapter, so ui does not depend on
// services::library (SampleLibrary::locations/list/search/freeBytes, SamplePool::request, PreviewService, the
// loadSample / unloadSample commands). The composition root installs an implementation over it with
// BrowseSource::install(); tests install a fake. With none installed BROWSE shows "Library not available".
// Every call is on the message thread; list/load may complete later (worker threads post back) — BROWSE never blocks
// and never touches the transport (UID-686).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ui::m1
{
struct BrowseEntry
{
    std::string name {}; // display name (file name with extension, folder name)
    std::string path {}; // alias path ("$Ext:USB/Samples/loops/Beat_120bpm_2bar.wav"), the key for every call
    bool folder = false;
    bool removable = false; // on a removable location (USB mark)
    std::string badge {};   // roots: "INTERNAL", "USB", or the location's name
    std::int64_t frames = 0;
    double rate = 0;
    std::optional<double> bpm {}, bars {};
};

enum class BrowseSort : std::uint8_t
{
    NameUp,
    NameDown
};

class BrowseSource
{
public:
    virtual ~BrowseSource() = default;
    virtual std::vector<BrowseEntry> roots() = 0; // LIB-001/002
    virtual void list(const std::string& path, BrowseSort, std::function<void(std::vector<BrowseEntry>)> done) = 0;
    // TEXT filter (LIB-014): a flat result list over every location.
    virtual void search(const std::string& text, std::function<void(std::vector<BrowseEntry>)> done)
    {
        (void)text;
        done({});
    }
    virtual std::string freeSpace(const std::string& path) = 0; // "1.0 GB FREE" (LIB-004)
    // Path of the track's sample ("" = empty); a reference to storage the source keeps, so asking never allocates.
    virtual const std::string& assigned(int track1Based) = 0;
    // Loads `path` onto the track (undoable command); progress 0–1, then done(ok, message).
    virtual void load(int track1Based, const std::string& path, std::function<void(float)> progress,
                      std::function<void(bool ok, std::string message)> done) = 0;
    virtual void unload(int track1Based) = 0;          // undoable
    virtual void preview(const std::string& path) = 0; // LIB-011, "" = stop
    virtual bool previewing() const = 0;

    static BrowseSource*& installed() noexcept
    {
        static BrowseSource* source = nullptr;
        return source;
    }
    static void install(BrowseSource* s) noexcept { installed() = s; }
};
} // namespace ui::m1
