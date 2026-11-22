#include "app/LibraryAdapters.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/library/LibraryCommands.h"
#include "core/model/Project.h"

#include <cstdio>

namespace app
{
namespace
{
namespace lib = services::library;

ui::m1::BrowseEntry toBrowse(const lib::Entry& e)
{
    ui::m1::BrowseEntry b;
    b.name = e.name.toStdString();
    b.path = e.aliasPath.toStdString();
    b.folder = e.folder;
    b.removable = e.removable;
    b.frames = e.frames;
    b.rate = e.rate;
    b.bpm = e.bpm;
    b.bars = e.bars;
    return b;
}

std::vector<ui::m1::BrowseEntry> toBrowse(const std::vector<lib::Entry>& in)
{
    std::vector<ui::m1::BrowseEntry> out;
    out.reserve(in.size());
    for (const auto& e : in)
        if (e.readable || e.folder)
            out.push_back(toBrowse(e));
    return out;
}

std::string aliasName(std::string_view alias)
{
    constexpr std::string_view ext = "$Ext:";
    return std::string(alias.substr(0, ext.size()) == ext ? alias.substr(ext.size()) : alias);
}
} // namespace

LibraryBrowseSource::LibraryBrowseSource(lib::SampleLibrary& library, lib::TrackLoader& loader,
                                         lib::PreviewService& preview, core::CommandBus& bus)
    : library_(library), loader_(loader), preview_(preview), bus_(bus)
{
    for (auto& s : assigned_)
        s.reserve(256);
}

LibraryBrowseSource::~LibraryBrowseSource() = default;

int LibraryBrowseSource::scene() const noexcept
{
    return core::commands::hasProject(bus_) ? core::commands::project(bus_).settings.currentScene & 15 : 0;
}

std::vector<ui::m1::BrowseEntry> LibraryBrowseSource::roots()
{
    std::vector<ui::m1::BrowseEntry> out;
    for (const auto& l : library_.locations())
    {
        if (l.alias == "$Lib") // LIB-001: the library root's audio folders are its browsable roots
        {
            for (const char* folder : lib::SampleLibrary::kAudioFolders)
                out.push_back({.name = folder, .path = l.alias + "/" + folder, .folder = true, .badge = "INTERNAL"});
            continue;
        }
        out.push_back({.name = aliasName(l.alias),
                       .path = l.alias,
                       .folder = true,
                       .removable = l.removable,
                       .badge = l.removable ? "USB" : aliasName(l.alias)});
    }
    return out;
}

void LibraryBrowseSource::list(const std::string& path, ui::m1::BrowseSort sort,
                               std::function<void(std::vector<ui::m1::BrowseEntry>)> done)
{
    library_.list(juce::String(path), sort == ui::m1::BrowseSort::NameDown ? lib::SortOrder::ZToA : lib::SortOrder::AToZ,
                  [done = std::move(done)](std::vector<lib::Entry> entries) { done(toBrowse(entries)); });
}

void LibraryBrowseSource::search(const std::string& text, std::function<void(std::vector<ui::m1::BrowseEntry>)> done)
{
    library_.search(juce::String(text),
                    [done = std::move(done)](std::vector<lib::Entry> entries) { done(toBrowse(entries)); });
}

std::string LibraryBrowseSource::freeSpace(const std::string& path)
{
    const std::string_view p = path;
    const lib::Location* best = nullptr;
    for (const auto& l : library_.locations()) // the location the path is in (LIB-004)
        if (p.substr(0, l.alias.size()) == l.alias && (best == nullptr || l.alias.size() > best->alias.size()))
            best = &l;
    if (best == nullptr)
        return {};
    const double bytes = static_cast<double>(library_.freeBytes(*best));
    char buf[32];
    if (bytes >= 1e9)
        std::snprintf(buf, sizeof buf, "%.1f GB FREE", bytes / 1e9);
    else
        std::snprintf(buf, sizeof buf, "%.0f MB FREE", bytes / 1e6);
    return buf;
}

const std::string& LibraryBrowseSource::assigned(int track1Based)
{
    auto& out = assigned_[static_cast<std::size_t>((track1Based - 1) & 15)];
    if (! core::commands::hasProject(bus_))
    {
        out.clear();
        return out;
    }
    const auto& p = core::commands::project(bus_);
    const auto& key = p.scenes[static_cast<std::size_t>(scene())]
                          .tracks[static_cast<std::size_t>((track1Based - 1) & 15)]
                          .playback.sample;
    const auto* ref = key.empty() ? nullptr : p.sampleRefs.find(key);
    if (ref == nullptr)
        out.clear();
    else if (out != ref->path)
        out = ref->path;
    return out;
}

void LibraryBrowseSource::load(int track1Based, const std::string& path, std::function<void(float)> progress,
                               std::function<void(bool, std::string)> done)
{
    const auto file = library_.resolve(juce::String(path));
    if (! file || ! file->existsAsFile())
    {
        if (done)
            done(false, "File not found");
        return;
    }
    if (progress)
        progress(0);
    preview_.stop(); // loading the file ends its preview (LIB-011)
    std::weak_ptr<bool> alive = alive_;
    loader_.load(scene(), (track1Based - 1) & 15, *file,
                 [alive, progress, done](core::Result<void> r, const lib::LoadInfo&)
                 {
                     if (alive.expired())
                         return;
                     if (progress)
                         progress(1);
                     if (done)
                         done(r.ok(), r.ok() ? std::string{} : r.error().message);
                 });
}

void LibraryBrowseSource::unload(int track1Based)
{
    bus_.execute(core::commands::library::unloadSample(scene(), (track1Based - 1) & 15));
}

void LibraryBrowseSource::preview(const std::string& path)
{
    if (path.empty())
    {
        preview_.stop();
        return;
    }
    if (const auto file = library_.resolve(juce::String(path)); file && file->existsAsFile())
        preview_.focus(*file);
    else
        preview_.focus(juce::File());
}

bool LibraryBrowseSource::previewing() const
{
    return preview_.playing() != juce::File();
}

SampleViewsAdapter::SampleViewsAdapter(lib::SamplePool& pool, lib::TrackSampleSync& sync, core::CommandBus& bus)
    : pool_(pool), sync_(sync), bus_(bus)
{
}

SampleViewsAdapter::~SampleViewsAdapter()
{
    ui::m1::SampleViews::instance().setProvider({});
}

void SampleViewsAdapter::install()
{
    ui::m1::SampleViews::instance().setProvider([this](int t) { return of(t); });
}

ui::m1::SampleView SampleViewsAdapter::of(int track1Based)
{
    if (! core::commands::hasProject(bus_))
        return {};
    const auto& p = core::commands::project(bus_);
    const int scene = p.settings.currentScene & 15, track = (track1Based - 1) & 15;
    const auto h = sync_.current(scene, track);
    if (h == nullptr)
        return {};
    const auto& ov = pool_.overview(h);
    auto& slot = cache_[h];
    // A freed buffer's address can come back for a new one: the overview's level storage tells them apart.
    const bool stale = ! slot || slot->levels.size() != ov.levels.size() ||
                       (! ov.levels.empty() && slot->levels[0].min != ov.levels[0].min.data());
    if (stale)
    {
        if (! slot)
            slot = std::make_unique<Converted>();
        slot->levels.clear();
        for (const auto& l : ov.levels)
            slot->levels.push_back({l.min.data(), l.max.data(), static_cast<std::int64_t>(l.min.size()),
                                    static_cast<double>(l.framesPerBin)});
        slot->overview.levels = slot->levels;
        slot->overview.frames = h->frames;
    }
    slot->overview.ch = h->ch; // the exact columns read the samples (WaveOverview::ch)
    slot->overview.channels = h->channels;
    ui::m1::SampleView v;
    v.overview = &slot->overview;
    v.frames = h->frames;
    v.sampleRate = h->sampleRate;
    const auto& key = p.scenes[static_cast<std::size_t>(scene)].tracks[static_cast<std::size_t>(track)].playback.sample;
    if (const auto* ref = p.sampleRefs.find(key))
    {
        std::string_view name = ref->displayName;
        if (name.empty())
        {
            name = ref->path;
            if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos)
                name.remove_prefix(slash + 1);
        }
        v.name = name;
    }
    return v;
}

void SampleViewsAdapter::collect()
{
    for (auto it = cache_.begin(); it != cache_.end();)
    {
        bool used = false;
        for (int s = 0; s < 16 && ! used; ++s)
            for (int t = 0; t < 16 && ! used; ++t)
                used = sync_.current(s, t) == it->first;
        it = used ? std::next(it) : cache_.erase(it);
    }
}
} // namespace app
