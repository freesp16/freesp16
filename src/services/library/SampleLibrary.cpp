#include "services/library/SampleLibrary.h"

#include "services/library/AudioFile.h"
#include "services/library/SidecarStore.h"

#include <algorithm>
#include <dirent.h>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace services::library
{
namespace
{
using core::Result;
constexpr const char* kLib = "$Lib";

juce::String displayName(const Location& l)
{
    if (l.alias == kLib)
        return "Library";
    const std::string_view a = l.alias;
    return juce::String(std::string(a.substr(0, 5) == "$Ext:" ? a.substr(5) : a));
}

const Location* findIn(const std::vector<Location>& locations, std::string_view alias) noexcept
{
    for (const auto& l : locations)
        if (l.alias == alias)
            return &l;
    return nullptr;
}

// "$Lib/Samples/x" → ("$Lib", "Samples/x"); a rest with "..", "." or an empty component is refused.
std::optional<std::pair<std::string, juce::String>> splitAlias(const juce::String& aliasPath)
{
    const int slash = aliasPath.indexOfChar('/');
    const juce::String alias = slash < 0 ? aliasPath : aliasPath.substring(0, slash);
    const juce::String rest = slash < 0 ? juce::String() : aliasPath.substring(slash + 1);
    if (! alias.startsWithChar('$'))
        return std::nullopt;
    if (rest.isNotEmpty())
    {
        juce::StringArray parts;
        parts.addTokens(rest, "/", "");
        for (const auto& p : parts)
            if (p.isEmpty() || p == "." || p == ".." || p.containsChar('\\'))
                return std::nullopt;
    }
    return std::make_pair(alias.toStdString(), rest);
}

// The names in a folder with their kind, by readdir (d_type; lstat only when the file system does not say): JUCE's
// directory iterator stats and wraps every entry, ≈ 40 µs each on macOS — most of a 10,000-entry listing.
struct DirItem
{
    juce::String name;
    bool folder;
};
std::vector<DirItem> readFolder(const juce::File& folder)
{
    std::vector<DirItem> out;
    const juce::String base = folder.getFullPathName();
    DIR* d = ::opendir(base.toRawUTF8());
    if (d == nullptr)
        return out;
    while (const dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') // LIB-003 (also ".", "..", macOS metadata)
            continue;
        bool dir = e->d_type == DT_DIR, file = e->d_type == DT_REG;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
        {
            struct stat st{};
            const juce::String full = base + "/" + juce::String::fromUTF8(e->d_name);
            // Symbolic links: a linked file is listed, a linked folder is not followed (no loops).
            if ((e->d_type == DT_LNK ? ::stat(full.toRawUTF8(), &st) : ::lstat(full.toRawUTF8(), &st)) != 0)
                continue;
            dir = S_ISDIR(st.st_mode) && e->d_type != DT_LNK;
            file = S_ISREG(st.st_mode);
        }
        if (dir || file)
            out.push_back({juce::String::fromUTF8(e->d_name), dir});
    }
    ::closedir(d);
    return out;
}

// `maySidecar`: whether a .json/.dat sibling may exist (a listing knows its folder's names).
Entry fileEntry(const juce::File& f, const juce::String& aliasPath, bool removable, bool maySidecar = true)
{
    Entry e;
    e.name = f.getFileName();
    e.file = f;
    e.aliasPath = aliasPath;
    e.removable = removable;
    if (auto info = probeAudioFile(f))
    {
        e.frames = info.value().frames;
        e.channels = info.value().channels;
        e.rate = info.value().sampleRate;
    }
    else
        e.readable = false;
    core::model::SampleMetadata meta;
    if (maySidecar && SidecarStore().tryRead(f, meta))
    {
        if (meta.bpm > 0)
            e.bpm = meta.bpm;
        if (meta.bars > 0)
            e.bars = meta.bars;
    }
    return e;
}

std::vector<Entry> listFolder(const juce::File& folder, const juce::String& aliasPath, bool removable, SortOrder sort)
{
    std::vector<Entry> out;
    std::vector<juce::File> audio;
    std::set<juce::String> sidecars; // names of .json/.dat siblings: no stat per file for the common no-sidecar case
    for (const auto& item : readFolder(folder))
    {
        const juce::String& name = item.name;
        const juce::File f = folder.getChildFile(name);
        if (item.folder)
        {
            Entry e;
            e.name = name, e.folder = true, e.file = f, e.aliasPath = aliasPath + "/" + name, e.removable = removable;
            out.push_back(std::move(e));
        }
        else if (isAudioFileName(name)) // LIB-010: non-audio files are not listed
            audio.push_back(f);
        else if (name.endsWithIgnoreCase(".json") || name.endsWithIgnoreCase(".dat"))
            sidecars.insert(name.upToLastOccurrenceOf(".", false, false));
    }
    out.reserve(out.size() + audio.size());
    for (const auto& f : audio)
        out.push_back(fileEntry(f, aliasPath + "/" + f.getFileName(), removable, sidecars.count(f.getFileName()) != 0));
    sortEntries(out, sort);
    return out;
}

void searchFolder(const juce::File& folder, const juce::String& aliasPath, bool removable, const juce::String& text,
                  std::vector<Entry>& out, std::size_t maxResults, int depth)
{
    if (depth > 32)
        return;
    for (const auto& item : readFolder(folder))
    {
        if (out.size() >= maxResults)
            return;
        const juce::String& name = item.name;
        const juce::File f = folder.getChildFile(name);
        if (item.folder)
            searchFolder(f, aliasPath + "/" + name, removable, text, out, maxResults, depth + 1);
        else if (isAudioFileName(name) && name.containsIgnoreCase(text))
            out.push_back(fileEntry(f, aliasPath + "/" + name, removable));
    }
}

std::vector<Location> withRemovable(const std::vector<Location>& current, std::vector<Location> removable)
{
    std::vector<Location> next;
    for (const auto& l : current)
        if (! l.removable)
            next.push_back(l);
    for (auto& l : removable)
        if (findIn(next, l.alias) == nullptr)
            next.push_back(std::move(l));
    return next;
}

bool sameLocations(const std::vector<Location>& a, const std::vector<Location>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].alias != b[i].alias || a[i].root != b[i].root || a[i].removable != b[i].removable)
            return false;
    return true;
}

Location volumeLocation(const juce::File& mount, const juce::String& name)
{
    Location l;
    l.alias = "$Ext:" + services::store::safeFileName(name.toStdString());
    const juce::File toraiz = mount.getChildFile("TORAIZ").getChildFile("Samples"); // M p.41
    l.root = toraiz.isDirectory() ? toraiz : mount;
    l.removable = true;
    return l;
}

bool isSameOrParent(const std::string& mount, const std::string& path)
{
    if (mount.empty() || path.empty())
        return false;
    std::string m = mount, p = path;
    while (m.size() > 1 && m.back() == '/')
        m.pop_back();
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p == m || (p.compare(0, m.size(), m) == 0 && p.size() > m.size() && (m == "/" || p[m.size()] == '/'));
}
} // namespace

void sortEntries(std::vector<Entry>& entries, SortOrder sort)
{
    std::stable_sort(entries.begin(), entries.end(),
                     [sort](const Entry& a, const Entry& b)
                     {
                         if (a.folder != b.folder)
                             return a.folder;
                         const int c = a.name.compareNatural(b.name, false);
                         return sort == SortOrder::AToZ ? c < 0 : c > 0;
                     });
}

// ---- providers ----

// ---- the internal disk ----

juce::File InternalDisk::stockRoot(const juce::File& mount)
{
    const juce::File stock = mount.getChildFile(kStockFolder);
    return stock.isDirectory() ? stock : mount;
}

juce::File InternalDisk::samplesRoot(const juce::File& mount)
{
    const juce::File samples = stockRoot(mount).getChildFile("Samples");
    return samples.isDirectory() ? samples : stockRoot(mount);
}

juce::File InternalDisk::projectsFolder(const juce::File& mount)
{
    return stockRoot(mount).getChildFile("Projects");
}

std::vector<juce::File> InternalDisk::projects(const juce::File& mount)
{
    std::vector<juce::File> out;
    const juce::File folder = projectsFolder(mount);
    if (! folder.isDirectory())
        return out;
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*", juce::File::FollowSymlinks::no))
        if (! f.getFileName().startsWithChar('.') && f.getFileExtension().equalsIgnoreCase(".prj"))
            out.push_back(f);
    std::sort(out.begin(), out.end(),
              [](const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural(b.getFileName()) < 0; });
    return out;
}

std::vector<Location> ProcMountsProvider::parse(std::string_view text, std::string_view internalMount,
                                                const std::vector<std::string>& skip)
{
    std::vector<Location> out;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line))
    {
        std::istringstream fields(line);
        std::string device, mount, type;
        if (! (fields >> device >> mount >> type))
            continue;
        if (type != "vfat" && type != "msdos" && type != "exfat" && type != "fuseblk")
            continue;
        // /proc/mounts escapes spaces as \040.
        std::string decoded;
        for (std::size_t i = 0; i < mount.size(); ++i)
        {
            if (mount.compare(i, 4, "\\040") == 0)
                decoded += ' ', i += 3;
            else
                decoded += mount[i];
        }
        if (decoded == "/" || decoded.rfind("/boot", 0) == 0)
            continue;
        if (std::any_of(skip.begin(), skip.end(), [&](const std::string& p) { return isSameOrParent(decoded, p); }))
            continue; // the data directory's own mount (the launcher's bind mount of the stick's data folder)
        const juce::File f(decoded);
        Location l;
        if (! internalMount.empty() && isSameOrParent(std::string(internalMount), decoded)
            && isSameOrParent(decoded, std::string(internalMount)))
        {
            l.alias = InternalDisk::kAlias;
            l.root = InternalDisk::samplesRoot(f);
            l.removable = true; // unmounted while the unit is in USB storage mode (docs/storage.md §2)
        }
        else
            l = volumeLocation(f, f.getFileName().isEmpty() ? juce::String("USB") : f.getFileName());
        l.readOnly = true;
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<Location> ProcMountsProvider::poll()
{
    if (! mounts_.existsAsFile())
        return {};
    juce::FileInputStream in(mounts_);
    return parse(in.readEntireStreamAsString().toStdString(), internal_, skip_); // /proc files report size 0
}

std::vector<Location> HostVolumesProvider::poll()
{
    std::vector<Location> out;
    const juce::String user = juce::SystemStats::getLogonName();
    for (const juce::File& base :
         {juce::File("/Volumes"), juce::File("/media/" + user), juce::File("/run/media/" + user)})
    {
        if (! base.isDirectory())
            continue;
        for (const auto& v :
             base.findChildFiles(juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no))
        {
            if (v.getFileName().startsWithChar('.') || v.isSymbolicLink()) // /Volumes/<boot> is a link to /
                continue;
            out.push_back(volumeLocation(v, v.getFileName()));
        }
    }
    return out;
}

// ---- SampleLibrary ----

SampleLibrary::SampleLibrary(Workers& workers) : workers_(workers), ops_(std::make_shared<FileOps>(juce::File())) {}

SampleLibrary::~SampleLibrary()
{
    *alive_ = false;
}

Result<void> SampleLibrary::setRoot(const juce::File& root)
{
    for (const char* sub : kLayout)
        if (! root.getChildFile(sub).createDirectory())
            return Result<void>::failure("Cannot create the library folder",
                                         root.getChildFile(sub).getFullPathName().toStdString());
    root_ = root;
    Location lib;
    lib.alias = kLib;
    lib.root = root;
    addLocation(lib);
    auto ops = std::make_shared<FileOps>(root.getChildFile("System").getChildFile("trash"));
    ops_ = std::move(ops);
    return {};
}

void SampleLibrary::addLocation(Location l)
{
    for (auto& existing : locations_)
        if (existing.alias == l.alias)
        {
            existing = std::move(l);
            return;
        }
    locations_.push_back(std::move(l));
}

void SampleLibrary::removeLocation(std::string_view alias)
{
    std::erase_if(locations_, [alias](const Location& l) { return l.alias == alias; });
}

const Location* SampleLibrary::findLocation(std::string_view alias) const noexcept
{
    return findIn(locations_, alias);
}

void SampleLibrary::setLocationProvider(std::shared_ptr<LocationProvider> p)
{
    provider_ = std::move(p);
}

void SampleLibrary::pollLocations()
{
    if (! provider_)
        return;
    auto provider = provider_;
    auto found = std::make_shared<std::vector<Location>>();
    std::weak_ptr<bool> alive = alive_;
    workers_.run([provider, found] { *found = provider->poll(); },
                 [this, alive, found]
                 {
                     if (alive.expired())
                         return;
                     auto next = withRemovable(locations_, std::move(*found));
                     if (sameLocations(next, locations_))
                         return;
                     locations_ = std::move(next);
                     if (onLocationsChanged)
                         onLocationsChanged();
                 });
}

std::optional<juce::File> SampleLibrary::resolve(const juce::String& aliasPath) const
{
    const auto split = splitAlias(aliasPath);
    if (! split)
        return std::nullopt;
    const Location* l = findLocation(split->first);
    if (l == nullptr)
        return std::nullopt;
    return split->second.isEmpty() ? l->root : l->root.getChildFile(split->second);
}

std::optional<juce::String> SampleLibrary::aliasPathOf(const juce::File& f) const
{
    const Location* best = nullptr;
    for (const auto& l : locations_)
        if ((f == l.root || f.isAChildOf(l.root)) &&
            (best == nullptr || l.root.getFullPathName().length() > best->root.getFullPathName().length()))
            best = &l;
    if (best == nullptr)
        return std::nullopt;
    if (f == best->root)
        return juce::String(best->alias);
    return juce::String(best->alias) + "/" + f.getRelativePathFrom(best->root).replaceCharacter('\\', '/');
}

services::store::SampleRoots SampleLibrary::roots() const
{
    services::store::SampleRoots r;
    r.library = root_;
    for (const auto& l : locations_)
        if (std::string_view(l.alias).substr(0, 5) == "$Ext:")
            r.externals[l.alias.substr(5)] = l.root;
    return r;
}

std::vector<Entry> SampleLibrary::listIn(const std::vector<Location>& locations, const juce::String& aliasPath,
                                         SortOrder sort)
{
    if (aliasPath.isEmpty())
    {
        std::vector<Entry> out;
        for (const auto& l : locations)
        {
            Entry e;
            e.name = displayName(l), e.folder = true, e.file = l.root, e.aliasPath = l.alias, e.removable = l.removable;
            out.push_back(std::move(e));
        }
        return out; // locations keep their order: library first
    }
    const auto split = splitAlias(aliasPath);
    if (! split)
        return {};
    const Location* l = findIn(locations, split->first);
    if (l == nullptr)
        return {};
    if (l->alias == kLib && split->second.isEmpty()) // only the audio folders of the library root
    {
        std::vector<Entry> out;
        for (const char* sub : kAudioFolders)
            if (const juce::File f = l->root.getChildFile(sub); f.isDirectory())
            {
                Entry e;
                e.name = sub, e.folder = true, e.file = f, e.aliasPath = aliasPath + "/" + sub;
                out.push_back(std::move(e));
            }
        return out;
    }
    const juce::File folder = split->second.isEmpty() ? l->root : l->root.getChildFile(split->second);
    if (! folder.isDirectory())
        return {};
    return listFolder(folder, aliasPath, l->removable, sort);
}

std::vector<Entry> SampleLibrary::searchIn(const std::vector<Location>& locations, const juce::String& text,
                                           std::size_t maxResults)
{
    std::vector<Entry> out;
    if (text.trim().isEmpty())
        return out;
    for (const auto& l : locations)
    {
        if (l.alias == kLib)
        {
            for (const char* sub : kAudioFolders)
                if (const juce::File f = l.root.getChildFile(sub); f.isDirectory())
                    searchFolder(f, juce::String(l.alias) + "/" + sub, l.removable, text.trim(), out, maxResults, 0);
        }
        else if (l.root.isDirectory())
            searchFolder(l.root, l.alias, l.removable, text.trim(), out, maxResults, 0);
    }
    sortEntries(out, SortOrder::AToZ);
    return out;
}

std::vector<Entry> SampleLibrary::listNow(const juce::String& aliasPath, SortOrder sort) const
{
    return listIn(locations_, aliasPath, sort);
}

std::vector<Entry> SampleLibrary::searchNow(const juce::String& text, std::size_t maxResults) const
{
    return searchIn(locations_, text, maxResults);
}

void SampleLibrary::list(const juce::String& aliasPath, SortOrder sort, std::function<void(std::vector<Entry>)> done)
{
    auto result = std::make_shared<std::vector<Entry>>();
    workers_.run([locations = locations_, aliasPath, sort, result] { *result = listIn(locations, aliasPath, sort); },
                 [done = std::move(done), result]
                 {
                     if (done)
                         done(std::move(*result));
                 });
}

void SampleLibrary::search(const juce::String& text, std::function<void(std::vector<Entry>)> done)
{
    auto result = std::make_shared<std::vector<Entry>>();
    workers_.run([locations = locations_, text, result] { *result = searchIn(locations, text, kMaxSearchResults); },
                 [done = std::move(done), result]
                 {
                     if (done)
                         done(std::move(*result));
                 });
}

std::int64_t SampleLibrary::freeBytes(const Location& l) const
{
    return l.root.getBytesFreeOnVolume();
}

namespace
{
Result<juce::File> refuseInUse(const juce::File& f)
{
    return Result<juce::File>::failure("This sample is used by the open project", f.getFullPathName().toStdString());
}

Result<juce::File> refuseReadOnly(const juce::File& f)
{
    return Result<juce::File>::failure("This location is read-only", f.getFullPathName().toStdString());
}

template <typename Op> void runOp(Workers& w, Op op, SampleLibrary::FileDone done)
{
    auto result = std::make_shared<std::optional<Result<juce::File>>>();
    w.run([op = std::move(op), result] { result->emplace(op()); },
          [done = std::move(done), result]
          {
              if (done)
                  done(std::move(**result));
          });
}
} // namespace

bool SampleLibrary::isReadOnly(const juce::File& f) const
{
    for (const auto& l : locations_)
        if (l.readOnly && l.root != juce::File() && (f == l.root || f.isAChildOf(l.root)))
            return true;
    return false;
}

void SampleLibrary::copy(const juce::File& source, const juce::File& destFolder, FileDone done)
{
    if (isReadOnly(destFolder))
        return done ? done(refuseReadOnly(destFolder)) : void();
    runOp(workers_, [ops = ops_, source, destFolder] { return ops->copy(source, destFolder); }, std::move(done));
}

void SampleLibrary::move(const juce::File& source, const juce::File& destFolder, bool force, FileDone done)
{
    if (isReadOnly(source) || isReadOnly(destFolder))
        return done ? done(refuseReadOnly(isReadOnly(source) ? source : destFolder)) : void();
    if (! force && inUse(source))
        return done ? done(refuseInUse(source)) : void();
    runOp(workers_, [ops = ops_, source, destFolder] { return ops->move(source, destFolder); }, std::move(done));
}

void SampleLibrary::rename(const juce::File& source, const juce::String& newName, bool force, FileDone done)
{
    if (isReadOnly(source))
        return done ? done(refuseReadOnly(source)) : void();
    if (! force && inUse(source))
        return done ? done(refuseInUse(source)) : void();
    runOp(workers_, [ops = ops_, source, newName] { return ops->rename(source, newName); }, std::move(done));
}

void SampleLibrary::trash(const juce::File& source, bool force, FileDone done)
{
    if (isReadOnly(source))
        return done ? done(refuseReadOnly(source)) : void();
    if (! force && inUse(source))
        return done ? done(refuseInUse(source)) : void();
    if (root_ == juce::File())
        return done ? done(Result<juce::File>::failure("No library root")) : void();
    runOp(workers_, [ops = ops_, source] { return ops->trash(source); }, std::move(done));
}

void SampleLibrary::newFolder(const juce::File& parent, const juce::String& name, FileDone done)
{
    if (isReadOnly(parent))
        return done ? done(refuseReadOnly(parent)) : void();
    runOp(workers_, [ops = ops_, parent, name] { return ops->newFolder(parent, name); }, std::move(done));
}

void SampleLibrary::importFiles(const juce::StringArray& paths, const juce::File& target,
                                std::function<void(std::vector<juce::File>)> done)
{
    const juce::File dest = target == juce::File() ? root_.getChildFile("Samples") : target;
    auto result = std::make_shared<std::vector<juce::File>>();
    if (isReadOnly(dest))
    {
        if (done)
            done({});
        return;
    }
    workers_.run(
        [ops = ops_, paths, dest, result]
        {
            for (const auto& p : paths)
            {
                const juce::File f(p);
                if (! f.existsAsFile() || ! isAudioFileName(f.getFileName()))
                    continue;
                if (auto r = ops->copy(f, dest))
                    result->push_back(r.value());
            }
        },
        [done = std::move(done), result]
        {
            if (done)
                done(std::move(*result));
        });
}
} // namespace services::library
