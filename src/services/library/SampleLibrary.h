// The sample library (LIB-001…004, LIB-010, LIB-014…018):
// one root plus extra locations shown as one folder tree, listings and search on a worker, file operations.
//
// Alias paths name every node of the tree (the same strings a SampleRef::path holds, FMT-011):
//   ""                          the tree root: one folder per location
//   "$Lib/Samples/Drums"        the library root's folders (only its audio folders are browsable:
//                               Samples/, RenderedAudio/, Recordings/)
//   "$Ext:<name>/…"             an added or removable location
// Listing drops names starting with '.' and non-audio files (LIB-003, LIB-010); length/channels/rate come from the
// file header only, BPM/bars from the sidecar (SidecarStore). Budget: 10,000 entries ≤ 300 ms on the A9 (bench_decode
// `decode_listing`).
//
// Removable volumes come from a LocationProvider: host = the mounted volumes (/Volumes on macOS, /media and /run/media
// on Linux), device = /proc/mounts polled once per second on a worker (vfat/msdos/exfat mounts); a stick's
// `TORAIZ/Samples` folder is used as its root when present and it has no PIONEER DJ SAMPLER folder (M p.41). Message thread API; work runs on Workers.
#pragma once

#include "core/util/Result.h"
#include "services/library/FileOps.h"
#include "services/library/Workers.h"
#include "services/store/SampleRefs.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace services::library
{
enum class SortOrder : std::uint8_t
{
    AToZ,
    ZToA
}; // browse.sort (UI-312); folders always first

struct Location
{
    std::string alias; // "$Lib", "$Ext:<name>" (LIB-002)
    juce::File root;
    bool removable = false;
    bool readOnly = false; // the stock firmware's tree, a stick's TORAIZ folder (UI-315)
};

struct Entry
{
    juce::String name;
    bool folder = false;
    std::int64_t frames = 0; // 0 for folders and unreadable files
    int channels = 0;
    double rate = 0;
    std::optional<double> bpm, bars;
    bool removable = false;
    juce::String aliasPath; // "$Lib/Samples/kick.wav" — what loadSample stores as SampleRef::path
    juce::File file;
    bool readable = true; // false: an audio name whose header does not parse (shown, never loaded)
};

class LocationProvider
{
public:
    virtual ~LocationProvider() = default;
    virtual std::vector<Location> poll() = 0; // worker thread
};

// The reference hardware's internal disk (research docs/storage.md §1: one FAT32 partition mounted at /mnt/emmc whose
// stock top folder holds Samples/, Projects/, RenderedAudio/, System/, Scenes/). Its stock tree is the
// read-only library location "$Ext:INTERNAL" (never written) and its Projects/*.prj are what `cmd import-sp16
// internal:…` opens. The stock folder name is an interop path constant, never shown in the UI.
struct InternalDisk
{
    static constexpr const char* kDefaultMount = "/mnt/emmc";
    static constexpr const char* kStockFolder = "PIONEER DJ SAMPLER";
    static constexpr const char* kAlias = "$Ext:INTERNAL";
    static juce::File stockRoot(const juce::File& mount);      // <mount>/<stock folder> when it is a folder, else mount
    static juce::File samplesRoot(const juce::File& mount);    // <stock root>/Samples when present, else the stock root
    static juce::File projectsFolder(const juce::File& mount); // <stock root>/Projects
    static std::vector<juce::File> projects(const juce::File& mount); // its *.prj, no '.' names (P5), A→Z
};

// The sampler's sample folders among `locations`, in their order: what BROWSE shows on the device, as the stock
// firmware does (addendum p.80 shows the top level as "Samples [INTERNAL]"; p.84: only a stick's PIONEER DJ SAMPLER/
// Samples is read by BROWSE). Not the app's own library folders, a stick's whole tree or a system mount. The internal
// disk's location (its root already is the stock Samples folder) and each removable location with a
// PIONEER DJ SAMPLER/Samples folder; every other location is left out. Stats a few folders.
struct SamplerFolder
{
    std::string aliasPath; // "$Ext:INTERNAL", "$Ext:sda1/PIONEER DJ SAMPLER/Samples"
    std::string name;      // "Samples"; with two or more sticks each stick's is "Samples (<mount name>)"
    bool internal = false;
};
std::vector<SamplerFolder> samplerFolders(std::span<const Location> locations);

// Linux /proc/mounts: FAT/exFAT mounts become removable "$Ext:<mount name>" locations. On the
// device every one is read-only (the stick also carries the app's data and its launcher; the app writes only to
// $Lib), the internal disk's mount becomes "$Ext:INTERNAL" (InternalDisk), and mounts that are the data directory or
// one of its parents (the launcher bind-mounts the stick's data folder over /root/data) are skipped.
class ProcMountsProvider final : public LocationProvider
{
public:
    explicit ProcMountsProvider(juce::File mounts = juce::File("/proc/mounts"),
                                std::string internalMount = InternalDisk::kDefaultMount,
                                std::vector<std::string> skip = {})
        : mounts_(std::move(mounts)), internal_(std::move(internalMount)), skip_(std::move(skip))
    {
    }
    std::vector<Location> poll() override;
    static std::vector<Location> parse(std::string_view mountsText,
                                       std::string_view internalMount = InternalDisk::kDefaultMount,
                                       const std::vector<std::string>& skip = {});

private:
    juce::File mounts_;
    std::string internal_;
    std::vector<std::string> skip_;
};

// Host: every folder in the volume folders (/Volumes, /media/<user>, /run/media/<user>) except the boot volume.
class HostVolumesProvider final : public LocationProvider
{
public:
    std::vector<Location> poll() override;
};

class SampleLibrary
{
public:
    explicit SampleLibrary(Workers& workers);
    ~SampleLibrary();
    SampleLibrary(const SampleLibrary&) = delete;
    SampleLibrary& operator=(const SampleLibrary&) = delete;

    // LIB-001: creates Projects/ Samples/ RenderedAudio/ Recordings/ Scenes/ Profiles/ System/ under `root` and makes
    // it the "$Lib" location.
    core::Result<void> setRoot(const juce::File& root);
    const juce::File& root() const noexcept { return root_; }
    static constexpr const char* kLayout[] = {"Projects", "Samples",  "RenderedAudio", "Recordings",
                                              "Scenes",   "Profiles", "System"};
    static constexpr const char* kAudioFolders[] = {"Samples", "RenderedAudio", "Recordings"};

    std::span<const Location> locations() const noexcept { return locations_; }
    void addLocation(Location); // LIB-002; replaces one with the same alias
    void removeLocation(std::string_view alias);
    const Location* findLocation(std::string_view alias) const noexcept;

    // Removable volumes: poll() runs the provider on a worker; changes arrive on the message thread (pump) and call
    // `onLocationsChanged`. The owner calls pollLocations() once per second.
    void setLocationProvider(std::shared_ptr<LocationProvider>);
    void pollLocations();
    std::function<void()> onLocationsChanged;

    // Alias path ↔ file (refuses "..", absolute rests and unknown aliases; FMT-011 rules of services::store).
    std::optional<juce::File> resolve(const juce::String& aliasPath) const;
    std::optional<juce::String> aliasPathOf(const juce::File&) const;
    services::store::SampleRoots roots() const;

    // Listing (worker) — `done` runs on the message thread.
    void list(const juce::String& aliasPath, SortOrder, std::function<void(std::vector<Entry>)> done);
    void search(const juce::String& text, std::function<void(std::vector<Entry>)> done); // LIB-014, all locations
    // The same, only below `scopes` (alias paths of folders, "$Ext:INTERNAL" or "$Ext:sda1/PIONEER DJ SAMPLER/Samples").
    void search(const juce::String& text, std::vector<juce::String> scopes,
                std::function<void(std::vector<Entry>)> done);
    // The same work, synchronously (worker threads, tests, bench).
    std::vector<Entry> listNow(const juce::String& aliasPath, SortOrder) const;
    std::vector<Entry> searchNow(const juce::String& text, std::size_t maxResults = kMaxSearchResults) const;
    static constexpr std::size_t kMaxSearchResults = 2000;

    std::int64_t freeBytes(const Location&) const; // LIB-004
    // `f` is (inside) a read-only location's root — every file operation below refuses to change it.
    bool isReadOnly(const juce::File& f) const;

    // File operations (LIB-015) on a worker; samples the open project uses need `force` (the UI warns first). Nothing
    // is created, moved, renamed or trashed in a read-only location (copying out of one is fine).
    using FileDone = std::function<void(core::Result<juce::File>)>;
    void copy(const juce::File& source, const juce::File& destFolder, FileDone done);
    void move(const juce::File& source, const juce::File& destFolder, bool force, FileDone done);
    void rename(const juce::File& source, const juce::String& newName, bool force, FileDone done);
    void trash(const juce::File& source, bool force, FileDone done);
    void newFolder(const juce::File& parent, const juce::String& name, FileDone done);
    // LIB-016 (host drop target): copies the audio files among `paths` into `target` (default
    // $Lib/Samples); `done` gets the new files.
    void importFiles(const juce::StringArray& paths, const juce::File& target,
                     std::function<void(std::vector<juce::File>)> done);

    // Which files the open project uses (the pool's answer); move/rename/trash refuse them without `force`.
    void setInUse(std::function<bool(const juce::File&)> inUse) { inUse_ = std::move(inUse); }
    bool inUse(const juce::File& f) const { return inUse_ && inUse_(f); }
    FileOps& fileOps() noexcept { return *ops_; }

    // The worker side, over a snapshot of the locations.
    static std::vector<Entry> listIn(const std::vector<Location>&, const juce::String& aliasPath, SortOrder);
    static std::vector<Entry> searchIn(const std::vector<Location>&, const juce::String& text, std::size_t maxResults);
    static std::vector<Entry> searchIn(const std::vector<Location>&, const std::vector<juce::String>& scopes,
                                       const juce::String& text, std::size_t maxResults);

private:
    Workers& workers_;
    juce::File root_;
    std::vector<Location> locations_;
    std::shared_ptr<LocationProvider> provider_;
    std::shared_ptr<FileOps> ops_;
    std::function<bool(const juce::File&)> inUse_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

void sortEntries(std::vector<Entry>&, SortOrder);
} // namespace services::library
