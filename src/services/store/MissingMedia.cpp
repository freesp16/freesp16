#include "services/store/MissingMedia.h"

#include "core/commands/CommandBusAccess.h"

#include <algorithm>
#include <map>
#include <set>

namespace services::store
{
namespace model = ::core::model;

namespace
{
std::string sampleName(const model::SampleRef& ref)
{
    const auto loc = splitLocation(ref.path);
    const auto& p = loc.path;
    const auto slash = p.find_last_of("/\\");
    auto name = slash == std::string::npos ? p : p.substr(slash + 1);
    if (name.empty())
        name = ref.displayName;
    return juce::String::fromUTF8(name.c_str()).toLowerCase().toStdString();
}

std::string trackPath(int s, int t)
{
    return "scene[" + std::to_string(s) + "].track[" + std::to_string(t) + "].playback.sample";
}
} // namespace

// ---- PoolEdit ----

PoolEdit::PoolEdit(std::string label, std::vector<model::SampleRef> entries, std::vector<TrackKey> keys, bool undoable)
    : label_(std::move(label)), entries_(std::move(entries)), keys_(std::move(keys)), undoable_(undoable)
{
}

void PoolEdit::apply(model::Project& p)
{
    p.sampleRefs.entries = entries_;
    for (const auto& k : keys_)
        p.scenes[k.scene & 15].tracks[k.track & 15].playback.sample = k.key;
}

std::unique_ptr<core::Command> PoolEdit::inverse(const model::Project& before) const
{
    std::vector<TrackKey> keys;
    keys.reserve(keys_.size());
    for (const auto& k : keys_)
        keys.push_back({k.scene, k.track, before.scenes[k.scene & 15].tracks[k.track & 15].playback.sample});
    return std::make_unique<PoolEdit>(label_, before.sampleRefs.entries, std::move(keys), undoable_);
}

void PoolEdit::collectChanges(core::ChangeSet& c) const
{
    c.add("sampleRefs");
    for (const auto& k : keys_)
        c.add(trackPath(k.scene & 15, k.track & 15));
}

std::size_t PoolEdit::sizeBytes() const
{
    std::size_t n = sizeof(*this) + label_.size();
    for (const auto& e : entries_)
        n += sizeof(e) + e.id.size() + e.path.size() + e.displayName.size();
    for (const auto& k : keys_)
        n += sizeof(k) + k.key.id.size();
    return n;
}

// ---- MissingMedia ----

MissingMedia::MissingMedia(core::CommandBus& bus, services::library::Workers& workers, std::function<SampleRoots()> roots)
    : bus_(bus), workers_(workers), roots_(std::move(roots))
{
}

MissingMedia::~MissingMedia()
{
    *alive_ = false;
    cancel_->store(true);
}

std::vector<model::SampleRef> MissingMedia::missing() const
{
    std::vector<model::SampleRef> out;
    if (! core::commands::hasProject(bus_))
        return out;
    for (const auto& e : core::commands::project(bus_).sampleRefs.entries)
        if (e.missing)
            out.push_back(e);
    return out;
}

std::vector<std::pair<int, int>> MissingMedia::silentTracks() const
{
    std::vector<std::pair<int, int>> out;
    if (! core::commands::hasProject(bus_))
        return out;
    const auto& p = core::commands::project(bus_);
    for (int s = 0; s < model::kScenes; ++s)
        for (int t = 0; t < model::kTracks; ++t)
        {
            const auto& key = p.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)].playback.sample;
            if (key.empty())
                continue;
            const auto* ref = p.sampleRefs.find(key);
            if (ref == nullptr || ref->missing)
                out.emplace_back(s, t);
        }
    return out;
}

int MissingMedia::refresh()
{
    if (! core::commands::hasProject(bus_))
        return 0;
    auto entries = core::commands::project(bus_).sampleRefs.entries;
    const auto roots = roots_ ? roots_() : SampleRoots {};
    bool changed = false;
    int missingCount = 0;
    for (auto& e : entries)
    {
        const auto file = resolveLocation(splitLocation(e.path), roots);
        const bool gone = ! file || ! file->existsAsFile();
        changed = changed || gone != e.missing;
        e.missing = gone;
        missingCount += gone ? 1 : 0;
    }
    if (changed)
        bus_.execute(std::make_unique<PoolEdit>("RESOLVE", std::move(entries), std::vector<PoolEdit::TrackKey> {}, false));
    return missingCount;
}

std::string MissingMedia::modelPathOf(const juce::File& file, const SampleRoots& roots)
{
    const auto rel = [&file](const juce::File& root) { return file.getRelativePathFrom(root).replaceCharacter('\\', '/'); };
    if (roots.bundle != juce::File() && file.isAChildOf(roots.bundle.getChildFile("samples")))
        return joinLocation({LocationKind::Bundle, {}, rel(roots.bundle.getChildFile("samples")).toStdString()});
    if (roots.library != juce::File() && file.isAChildOf(roots.library))
        return joinLocation({LocationKind::Library, {}, rel(roots.library).toStdString()});
    for (const auto& [name, root] : roots.externals)
        if (file.isAChildOf(root))
            return joinLocation({LocationKind::External, name, rel(root).toStdString()});
    return file.getFullPathName().toStdString();
}

std::vector<MissingMedia::Match> MissingMedia::search(const std::vector<model::SampleRef>& missing,
                                                      const juce::File& folder, const SampleRoots& roots,
                                                      const std::atomic<bool>& cancel, RelinkReport& report)
{
    std::vector<Match> matches;
    std::set<std::string> wanted;
    for (const auto& m : missing)
        wanted.insert(sampleName(m));
    std::map<std::string, std::vector<juce::File>> byName;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
    {
        if (cancel.load(std::memory_order_relaxed) || report.filesScanned >= kMaxFilesScanned)
            break;
        ++report.filesScanned;
        const auto& f = entry.getFile();
        if (f.getFileName().startsWithChar('.') || f.isSymbolicLink())
            continue;
        const auto name = f.getFileName().toLowerCase().toStdString();
        if (wanted.count(name) != 0)
            byName[name].push_back(f);
    }
    if (report.filesScanned >= kMaxFilesScanned)
        report.notes.push_back("search stopped after " + std::to_string(kMaxFilesScanned) + " files");
    std::map<std::string, std::string> hashes; // file → content id (one hash per file)
    for (const auto& m : missing)
    {
        if (cancel.load(std::memory_order_relaxed))
        {
            report.cancelled = true;
            break;
        }
        auto it = byName.find(sampleName(m));
        bool found = false;
        if (it != byName.end())
        {
            auto& files = it->second;
            std::sort(files.begin(), files.end(),
                      [](const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); });
            for (const auto& f : files)
            {
                if (! isContentId(m.id)) // an imported pool id: the name is all there is
                {
                    report.notes.push_back(m.path + ": matched by name only");
                    matches.push_back({m.id, modelPathOf(f, roots), f});
                    found = true;
                    break;
                }
                const auto key = f.getFullPathName().toStdString();
                auto h = hashes.find(key);
                if (h == hashes.end())
                {
                    auto id = contentId(f); // reads the file; never writes it (PRJ-014)
                    h = hashes.emplace(key, id ? id.value() : std::string()).first;
                }
                if (h->second == m.id)
                {
                    matches.push_back({m.id, modelPathOf(f, roots), f});
                    found = true;
                    break;
                }
                report.notes.push_back(f.getFullPathName().toStdString() + ": same name, different content");
            }
        }
        if (! found)
            report.unmatched.push_back(m);
    }
    return matches;
}

void MissingMedia::relink(const juce::File& folder, std::function<void(RelinkReport)> done)
{
    if (searching_)
    {
        RelinkReport r;
        r.cancelled = true;
        r.notes.push_back("a search is already running");
        done(std::move(r));
        return;
    }
    auto list = missing();
    if (list.empty() || ! folder.isDirectory())
    {
        RelinkReport r;
        r.unmatched = std::move(list);
        if (! folder.isDirectory())
            r.notes.push_back(folder.getFullPathName().toStdString() + " is not a folder");
        done(std::move(r));
        return;
    }
    searching_ = true;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    struct Job
    {
        RelinkReport report;
        std::vector<Match> matches;
    };
    auto job = std::make_shared<Job>();
    std::weak_ptr<bool> alive = alive_;
    workers_.run([job, list = std::move(list), folder, roots = roots_ ? roots_() : SampleRoots {}, cancel = cancel_]
                 { job->matches = search(list, folder, roots, *cancel, job->report); },
                 [this, alive, job, cancel = cancel_, done = std::move(done)]
                 {
                     if (alive.expired())
                         return;
                     searching_ = false;
                     auto report = std::move(job->report);
                     if (cancel->load() || report.cancelled || ! core::commands::hasProject(bus_))
                     {
                         report.cancelled = true;
                         report.relinked = 0;
                         done(std::move(report));
                         return;
                     }
                     // Applied to the pool as it is now (it may have changed while the worker searched).
                     auto entries = core::commands::project(bus_).sampleRefs.entries;
                     int relinked = 0;
                     for (const auto& m : job->matches)
                         for (auto& e : entries)
                             if (e.id == m.id && e.missing)
                             {
                                 e.path = m.path;
                                 e.missing = false;
                                 ++relinked;
                             }
                     if (relinked > 0)
                         bus_.execute(std::make_unique<PoolEdit>("RELINK", std::move(entries),
                                                                 std::vector<PoolEdit::TrackKey> {}));
                     report.relinked = relinked;
                     report.unmatched = missing();
                     done(std::move(report));
                 });
}

void MissingMedia::cancel() noexcept { cancel_->store(true); }

void MissingMedia::leaveEmpty(const std::vector<std::string>& ids)
{
    if (! core::commands::hasProject(bus_))
        return;
    const std::set<std::string> drop(ids.begin(), ids.end());
    const auto& p = core::commands::project(bus_);
    std::vector<model::SampleRef> entries;
    for (const auto& e : p.sampleRefs.entries)
        if (drop.count(e.id) == 0 || e.id.empty())
            entries.push_back(e);
    std::vector<PoolEdit::TrackKey> keys;
    for (int s = 0; s < model::kScenes; ++s)
        for (int t = 0; t < model::kTracks; ++t)
        {
            const auto& key = p.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)].playback.sample;
            if (! key.empty() && drop.count(key.id) != 0)
                keys.push_back({static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(t), model::SampleKey {}});
        }
    if (entries.size() == p.sampleRefs.entries.size() && keys.empty())
        return;
    bus_.execute(std::make_unique<PoolEdit>("CLEAR", std::move(entries), std::move(keys)));
}
} // namespace services::store
