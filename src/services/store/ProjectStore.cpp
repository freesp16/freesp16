#include "services/store/ProjectStore.h"

#include "core/audio/SampleBuffer.h"
#include "core/commands/CommandBusAccess.h"
#include "core/model/ModelOps.h"
#include "services/store/CodecRegistry.h"
#include "services/store/NativeCodec.h"
#include "services/store/Resume.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace services::store
{
namespace model = ::core::model;

namespace
{
constexpr std::size_t kMaxNameLength = 64;

bool isGenerated(const juce::String& rel)
{
    if (rel == "meta.json" || rel == "project.json")
        return true;
    return rel.startsWith("patterns/scene-") && rel.endsWith(".json") && ! rel.fromFirstOccurrenceOf("/", false, false)
                                                                               .containsChar('/');
}

bool looksLikeBundle(const juce::File& f)
{
    return f.isDirectory() && f.getFileName().endsWithIgnoreCase(kNativeExtension.data()) &&
           ! f.getFileName().contains(".tmp-") && f.getChildFile("project.json").existsAsFile();
}

std::string timestampName()
{
    const auto t = juce::Time::getCurrentTime();
    return t.formatted("%Y%m%d-%H%M%S").toStdString() + "-" + juce::String(t.getMilliseconds()).paddedLeft('0', 3).toStdString();
}

juce::File uniqueChild(const juce::File& folder, const std::string& stem, const char* ext)
{
    auto f = folder.getChildFile(juce::String::fromUTF8(stem.c_str()) + ext);
    for (int n = 2; f.exists(); ++n)
        f = folder.getChildFile(juce::String::fromUTF8(stem.c_str()) + "-" + juce::String(n) + ext);
    return f;
}

// The string value of `key` in the first bytes of a JSON text (no escapes expected: ISO timestamps).
std::string scanString(const std::string& text, std::string_view key)
{
    const auto k = text.find("\"" + std::string(key) + "\"");
    if (k == std::string::npos)
        return {};
    auto colon = text.find(':', k + key.size() + 2);
    if (colon == std::string::npos)
        return {};
    const auto open = text.find('"', colon);
    if (open == std::string::npos)
        return {};
    const auto close = text.find('"', open + 1);
    if (close == std::string::npos || close - open > 64)
        return {};
    return text.substr(open + 1, close - open - 1);
}

std::string readHead(const juce::File& f, int bytes)
{
    juce::FileInputStream in(f);
    if (! in.openedOk())
        return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    const int n = in.read(out.data(), bytes);
    out.resize(static_cast<std::size_t>(std::max(0, n)));
    return out;
}

// True when two bundles hold the same JSON files (hard links of one version share inodes; copies compare bytes).
bool sameVersion(const juce::File& a, const juce::File& b)
{
    std::vector<juce::String> names {"meta.json", "project.json"};
    for (const auto& e : juce::RangedDirectoryIterator(a.getChildFile("patterns"), false, "*.json", juce::File::findFiles))
        names.push_back("patterns/" + e.getFile().getFileName());
    int countB = 0;
    for (const auto& e : juce::RangedDirectoryIterator(b.getChildFile("patterns"), false, "*.json", juce::File::findFiles))
        countB += e.getFile().exists() ? 1 : 0;
    if (static_cast<int>(names.size()) - 2 != countB)
        return false;
    for (const auto& n : names)
    {
        const auto fa = a.getChildFile(n), fb = b.getChildFile(n);
        struct stat sa {}, sb {};
        if (::stat(fa.getFullPathName().toRawUTF8(), &sa) != 0 || ::stat(fb.getFullPathName().toRawUTF8(), &sb) != 0)
            return false;
        if (sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev)
            continue;
        if (sa.st_size != sb.st_size)
            return false;
        juce::MemoryBlock da, db;
        if (! fa.loadFileAsData(da) || ! fb.loadFileAsData(db) || da != db)
            return false;
    }
    return true;
}

// Sizes of the pattern files of a default project (a scene without content), per scene index.
const std::array<std::int64_t, model::kScenes>& emptySceneSizes()
{
    static std::array<std::int64_t, model::kScenes> sizes {};
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       const auto d = model::newDefaultProject();
                       for (const auto& f : NativeCodec::encode(*d))
                           for (int s = 0; s < model::kScenes; ++s)
                               if (f.name == "patterns/scene-" + juce::String(s + 1).paddedLeft('0', 2).toStdString() +
                                                  ".json")
                                   sizes[static_cast<std::size_t>(s)] = static_cast<std::int64_t>(f.text.size());
                   });
    return sizes;
}

// Replaces the value of "created" in the encoded meta.json (NativeCodec::encode stamps created = modified).
void keepCreated(std::string& meta, const std::string& created)
{
    if (created.empty())
        return;
    const auto k = meta.find("\"created\"");
    if (k == std::string::npos)
        return;
    const auto open = meta.find('"', meta.find(':', k));
    const auto close = open == std::string::npos ? open : meta.find('"', open + 1);
    if (close == std::string::npos)
        return;
    meta.replace(open + 1, close - open - 1, created);
}

std::string errnoText(int e) { return e == 0 ? std::string("unknown error") : std::string(std::strerror(e)); }

Result<void> busyError() { return Result<void>::failure("Another save or load is running: try again when it ends"); }

void setFraction(Progress* p, float f)
{
    if (p != nullptr)
        p->fraction.store(f, std::memory_order_relaxed);
}
} // namespace

// ---- names ----

std::optional<std::string_view> validateProjectName(std::u32string_view name) noexcept
{
    if (name.empty())
        return "Enter a name";
    if (name.size() > kMaxNameLength)
        return "A name can have at most 64 characters";
    if (name.front() == U'.')
        return "A name cannot start with \".\"";
    for (const char32_t c : name)
    {
        if (c < 0x20 || c == 0x7F)
            return "A name cannot contain control characters";
        if (std::u32string_view(U"/\\:*?\"<>|").find(c) != std::u32string_view::npos)
            return "A name cannot contain / \\ : * ? \" < > |";
    }
    if (name.back() == U' ' || name.back() == U'.')
        return "A name cannot end with a space or \".\"";
    return std::nullopt;
}

std::optional<std::string_view> validateProjectName(const juce::String& name) noexcept
{
    std::u32string text;
    for (auto p = name.getCharPointer(); ! p.isEmpty();)
        text.push_back(static_cast<char32_t>(p.getAndAdvance()));
    return validateProjectName(std::u32string_view(text));
}

juce::File bundleFor(const juce::File& folder, const juce::String& name)
{
    return folder.getChildFile(name + kNativeExtension.data());
}

// ---- input guards ----

bool jsonNestingWithin(std::string_view text, int maxDepth) noexcept
{
    int depth = 0;
    bool inString = false, escaped = false;
    for (const char c : text)
    {
        if (inString)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '[' || c == '{')
        {
            if (++depth > maxDepth)
                return false;
        }
        else if ((c == ']' || c == '}') && depth > 0)
            --depth;
    }
    return true;
}

Result<void> checkBundleInputs(const juce::File& bundle)
{
    std::vector<juce::File> files {bundle.getChildFile("meta.json"), bundle.getChildFile("project.json")};
    for (const auto& e :
         juce::RangedDirectoryIterator(bundle.getChildFile("patterns"), false, "*.json", juce::File::findFiles))
        files.push_back(e.getFile());
    for (const auto& f : files)
    {
        if (! f.existsAsFile())
            continue; // the codec reports a missing required file
        if (f.getSize() > kMaxProjectFileBytes)
            return Result<void>::failure("The project file " + f.getFullPathName().toStdString() + " is too large");
        juce::MemoryBlock data;
        if (! f.loadFileAsData(data))
            return Result<void>::failure("Cannot read " + f.getFullPathName().toStdString());
        if (! jsonNestingWithin({static_cast<const char*>(data.getData()), data.getSize()}))
            return Result<void>::failure("The project file " + f.getFullPathName().toStdString() + " is damaged",
                                         "nested deeper than " + std::to_string(kMaxJsonDepth));
    }
    return {};
}

// ---- headers and backups ----

// Header reads use POSIX calls only: on macOS JUCE's directory and attribute calls go through NSURL/CoreFoundation
// (≈ 4 ms per bundle measured on the host); on Linux they are the same syscalls.
std::optional<ProjectInfo> readProjectInfo(const juce::File& bundle, bool external)
{
    const std::string dir = bundle.getFullPathName().toStdString();
    const auto name = bundle.getFileName();
    if (! name.endsWithIgnoreCase(kNativeExtension.data()) || name.contains(".tmp-"))
        return std::nullopt;
    struct stat st {};
    if (::stat((dir + "/project.json").c_str(), &st) != 0 || ! S_ISREG(st.st_mode))
        return std::nullopt;
    ProjectInfo info;
    info.path = bundle;
    info.name = bundle.getFileNameWithoutExtension();
    info.external = external;
    std::string head;
    struct stat metaStat {};
    const bool hasMeta = ::stat((dir + "/meta.json").c_str(), &metaStat) == 0;
    if (const int fd = hasMeta ? ::open((dir + "/meta.json").c_str(), O_RDONLY) : -1; fd >= 0)
    {
        head.resize(2048);
        const auto n = ::read(fd, head.data(), head.size());
        head.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
        ::close(fd);
    }
    const auto modified = scanString(head, "modified");
    info.modified = modified.empty() ? juce::Time() : juce::Time::fromISO8601(juce::String(modified));
    if (info.modified.toMilliseconds() == 0)
        info.modified = juce::Time(static_cast<std::int64_t>((hasMeta ? metaStat : st).st_mtime) * 1000);
    const auto& empty = emptySceneSizes();
    if (DIR* d = ::opendir((dir + "/patterns").c_str()))
    {
        while (const dirent* e = ::readdir(d))
        {
            int s = 0;
            char tail[8] = {};
            if (std::sscanf(e->d_name, "scene-%2d.%5s", &s, tail) != 2 || std::strcmp(tail, "json") != 0 || s < 1 ||
                s > model::kScenes)
                continue;
            struct stat fs {};
            if (::fstatat(::dirfd(d), e->d_name, &fs, 0) == 0 && fs.st_size > empty[static_cast<std::size_t>(s - 1)])
                ++info.scenesWithContent;
        }
        ::closedir(d);
    }
    return info;
}

std::vector<juce::File> listBackups(const juce::File& folder)
{
    std::vector<juce::File> out;
    if (! folder.isDirectory())
        return out;
    AtomicWriter::recoverLeftovers(folder);
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findDirectories))
        if (looksLikeBundle(entry.getFile()))
            out.push_back(entry.getFile());
    std::sort(out.begin(), out.end(),
              [](const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
    return out;
}

Result<WriteReport> writeProjectVersion(const model::Project& project, const juce::File& bundle,
                                        const WriteOptions& options)
{
    WriteReport report;
    // 1. PRJ-012: the version on disk goes to Backups/<name>/ first (hard links: the files are never modified), once
    //    per version (a save interrupted earlier may have backed it up already), then the oldest beyond backupsKept
    //    go — before the write, so an interrupted save never leaves more than backupsKept.
    if (options.backupFolder != juce::File() && looksLikeBundle(bundle))
    {
        Result<void> r;
        if (! options.backupFolder.createDirectory())
            r = Result<void>::failure("cannot create " + options.backupFolder.getFullPathName().toStdString(),
                                      errnoText(errno));
        auto all = r ? listBackups(options.backupFolder) : std::vector<juce::File> {};
        const bool already = ! all.empty() && sameVersion(all.back(), bundle);
        if (r && ! already)
        {
            const auto target = uniqueChild(options.backupFolder, timestampName(), kNativeExtension.data());
            AtomicWriter backup(target);
            r = backup.carryOver(bundle, [](const juce::String& rel) { return ! isGenerated(rel); });
            if (r)
                r = backup.commit();
            if (r)
            {
                report.backup = target;
                all.push_back(target);
            }
        }
        if (! r)
            report.warnings.push_back("The previous version could not be backed up: " + r.error().message);
        const auto keepN = static_cast<std::size_t>(std::max(1, options.backupsKept));
        for (std::size_t i = 0; all.size() > keepN && i < all.size() - keepN; ++i)
            if (! all[i].deleteRecursively())
                report.warnings.push_back("Could not remove the old backup " + all[i].getFullPathName().toStdString());
    }
    setFraction(options.progress, 0.2f);

    // 2. PRJ-010/011: the new version, atomically; the old one stays until the new one is complete.
    std::string created;
    if (const auto meta = bundle.getChildFile("meta.json"); meta.existsAsFile())
        created = scanString(readHead(meta, 2048), "created");
    SaveOptions so;
    so.timestamp = options.timestamp.empty() ? juce::Time::getCurrentTime().toISO8601(true).toStdString()
                                             : options.timestamp;
    AtomicWriter writer(bundle);
    if (options.fault)
        writer.setFaultHook(options.fault);
    const auto keep = [](const juce::String& rel) { return isGenerated(rel); };
    if (auto r = writer.carryOver(bundle, keep); ! r)
        return r.error();
    if (options.carryFrom != juce::File() && options.carryFrom != bundle) // PRJ-014: samples/ etc. come along
        if (auto r = writer.carryOver(options.carryFrom, keep); ! r)
            return r.error();
    auto files = NativeCodec::encode(project, so);
    setFraction(options.progress, 0.5f);
    for (auto& f : files)
    {
        if (f.name == "meta.json")
            keepCreated(f.text, created);
        if (auto r = writer.writeFile(juce::String::fromUTF8(f.name.c_str()), f.text.data(), f.text.size()); ! r)
            return r.error();
        report.bytesWritten += f.text.size();
    }
    if (auto r = writer.commit(); ! r)
        return r.error();
    setFraction(options.progress, 0.9f);

    return report;
}

// ---- the store ----

struct ProjectStore::OpenJob
{
    juce::File source, identity;
    Progress* progress = nullptr;
    std::function<void(Result<void>, LoadReport)> done;
    SampleRoots roots;
    std::unique_ptr<model::Project> project;
    std::optional<Result<void>> read;
    LoadReport report;
    bool writeFirst = false; // importForeign: the new native bundle is written on the worker before the swap
    bool written = false;    // … and was (else the import opens unsaved, as an untitled project would)
    std::vector<core::audio::SampleBuffer const*> held; // retained until the document holds them (after the swap)
    int pending = 0;
    bool cancelled() const { return progress != nullptr && progress->cancel.load(std::memory_order_relaxed); }
};

ProjectStore::ProjectStore(core::CommandBus& bus, services::library::Workers& workers, StoreConfig config)
    : bus_(bus), workers_(workers), config_(std::move(config))
{
    changesSub_ = bus_.observe("", [this](const core::ChangeSet&) { ++changes_; });
    file_ = bundleFor(config_.projectsDir, name_);
    external_ = false;
}

ProjectStore::~ProjectStore() { *alive_ = false; }

bool ProjectStore::dirty() const noexcept { return bus_.dirty(); }

SampleRoots ProjectStore::roots() const
{
    SampleRoots r = config_.sampleRoots ? config_.sampleRoots() : SampleRoots {};
    r.bundle = file_;
    return r;
}

bool ProjectStore::isExternal(const juce::File& f) const
{
    return f.getParentDirectory() != config_.projectsDir;
}

bool ProjectStore::isOpen(const juce::File& f) const { return hasFile_ && f == file_; }

void ProjectStore::adopt(const juce::File& file, bool hasFile, bool readOnly)
{
    file_ = file;
    name_ = file.getFileNameWithoutExtension();
    hasFile_ = hasFile;
    readOnly_ = readOnly;
    external_ = isExternal(file);
    if (onIdentity)
        onIdentity(name_, file_);
}

void ProjectStore::stopTransport()
{
    if (bridge_ != nullptr) // PRJ-050: Stop(1) = stop, back to step 1; the engine fades
        bridge_->post(engine::TransportCmd {engine::TransportCmd::Stop, 1.0});
}

std::vector<ProjectInfo> ProjectStore::list(SortBy sort) const
{
    std::vector<ProjectInfo> out;
    const auto scan = [&out](const juce::File& folder, bool external)
    {
        DIR* d = ::opendir(folder.getFullPathName().toRawUTF8());
        if (d == nullptr)
            return;
        while (const dirent* e = ::readdir(d))
        {
            if (e->d_name[0] == '.' || (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN))
                continue;
            if (auto info = readProjectInfo(folder.getChildFile(juce::String::fromUTF8(e->d_name)), external))
                out.push_back(std::move(*info));
        }
        ::closedir(d);
    };
    scan(config_.projectsDir, false);
    if (config_.externalFolders)
        for (const auto& f : config_.externalFolders())
            if (f != config_.projectsDir)
                scan(f, true);
    // The reference projects (hardware feedback: OPEN PROJECT listed none of the unit's own projects, so nothing
    // could be opened): regular *.prj files, no '.' names (AppleDouble / hidden), the file's time.
    if (config_.foreignFolders)
        for (const auto& f : config_.foreignFolders())
        {
            const std::string dir = f.folder.getFullPathName().toStdString();
            DIR* d = ::opendir(dir.c_str());
            if (d == nullptr)
                continue;
            while (const dirent* e = ::readdir(d))
            {
                const juce::String name = juce::String::fromUTF8(e->d_name);
                if (e->d_name[0] == '.' || ! name.endsWithIgnoreCase(".prj"))
                    continue;
                struct stat st {};
                if (::stat((dir + "/" + e->d_name).c_str(), &st) != 0 || ! S_ISREG(st.st_mode))
                    continue;
                ProjectInfo info;
                info.path = f.folder.getChildFile(name);
                info.name = info.path.getFileNameWithoutExtension();
                info.modified = juce::Time(static_cast<juce::int64>(st.st_mtime) * 1000);
                info.external = true;
                info.foreign = true;
                info.location = f.location;
                out.push_back(std::move(info));
            }
            ::closedir(d);
        }
    if (sort == SortBy::Name)
        std::sort(out.begin(), out.end(), [](const ProjectInfo& a, const ProjectInfo& b)
                  { return a.name.compareNatural(b.name) < 0 || (a.name == b.name && a.external < b.external); });
    else
        std::sort(out.begin(), out.end(),
                  [](const ProjectInfo& a, const ProjectInfo& b) { return a.modified > b.modified; });
    return out;
}

void ProjectStore::importForeign(const ProjectInfo& info, Progress& progress,
                                 std::function<void(Result<void>, LoadReport)> done)
{
    juce::String name = juce::String::fromUTF8(safeFileName(info.name.toStdString()).c_str());
    if (validateProjectName(name))
        name = "Imported";
    config_.projectsDir.createDirectory(); // the import's home (a later SAVE writes there)
    juce::File identity = bundleFor(config_.projectsDir, name);
    for (int k = 2; identity.exists() && k < 1000; ++k)
        identity = bundleFor(config_.projectsDir, name + " " + juce::String(k));
    // Saved under the new name before the swap: resume, the OPEN list and SAVE then see a native project like any
    // other (an unsaved import was lost at the next start, or resumed from the previous project's copy).
    startOpen(nativeBundleOf(info.path), identity, progress, std::move(done), true);
}

std::vector<ProjectInfo> ProjectStore::backups(const ProjectInfo& project) const
{
    std::vector<ProjectInfo> out;
    for (const auto& b : listBackups(config_.backupsDir.getChildFile(project.name)))
        if (auto info = readProjectInfo(b, false))
            out.push_back(std::move(*info));
    std::reverse(out.begin(), out.end());
    return out;
}

void ProjectStore::createUntitled()
{
    stopTransport();
    auto old = core::commands::replaceProject(bus_, model::newDefaultProject());
    adopt(bundleFor(config_.projectsDir, kUntitledName), false, false);
    if (onReplaced)
        onReplaced(core::commands::project(bus_), LoadReport {});
}

Result<void> ProjectStore::create(const juce::String& name)
{
    if (busy_)
        return busyError();
    if (const auto bad = validateProjectName(name))
        return Result<void>::failure(std::string(*bad));
    const auto target = bundleFor(config_.projectsDir, name);
    if (target.exists())
        return Result<void>::failure("A project named \"" + name.toStdString() + "\" already exists");
    stopTransport();
    auto old = core::commands::replaceProject(bus_, model::newDefaultProject());
    adopt(target, false, false);
    if (onReplaced)
        onReplaced(core::commands::project(bus_), LoadReport {});
    return {};
}

void ProjectStore::open(const juce::File& bundle, Progress& progress, std::function<void(Result<void>, LoadReport)> done)
{
    const auto b = nativeBundleOf(bundle);
    startOpen(b, b, progress, std::move(done));
}

void ProjectStore::openAs(const juce::File& source, const juce::File& identity, Progress& progress,
                          std::function<void(Result<void>, LoadReport)> done)
{
    startOpen(nativeBundleOf(source), identity, progress, std::move(done));
}

void ProjectStore::startOpen(const juce::File& source, const juce::File& identity, Progress& progress,
                             std::function<void(Result<void>, LoadReport)> done, bool writeFirst)
{
    if (busy_)
    {
        done(busyError(), LoadReport {});
        return;
    }
    busy_ = true;
    progress.fraction.store(0.0f, std::memory_order_relaxed);
    stopTransport();
    auto job = std::make_shared<OpenJob>();
    job->source = source;
    job->identity = identity;
    job->progress = &progress;
    job->done = std::move(done);
    job->writeFirst = writeFirst;
    job->report.foreign = writeFirst;
    job->roots = config_.sampleRoots ? config_.sampleRoots() : SampleRoots {};
    job->roots.bundle = identity.isDirectory() ? identity : source;
    std::weak_ptr<bool> alive = alive_;
    workers_.run(
        [job]
        {
            job->project = std::make_unique<model::Project>();
            ImportReport ir;
            Result<void> r;
            if (job->source.getFileName().endsWithIgnoreCase(kNativeExtension.data()))
            {
                AtomicWriter::recover(job->source); // an interrupted save first (openProjectInto would, after the guard)
                r = checkBundleInputs(job->source);
            }
            if (r)
                r = openProjectInto(job->source, *job->project, ir);
            if (r)
            {
                resolvePool(job->project->sampleRefs, job->roots); // PRJ-040: missing is a state, not an error
                job->report.readOnly = ir.readOnly;
            }
            if (r && job->writeFirst && ! job->cancelled())
            {
                // A failed write does not fail the import: it opens unsaved and the next SAVE asks for a name.
                if (auto w = writeProjectVersion(*job->project, job->identity, WriteOptions {}))
                    job->written = true;
                else
                    ir.warnings.push_back("not saved: " + w.error().message);
            }
            job->report.warnings = std::move(ir.warnings);
            for (auto& u : ir.unmapped)
                job->report.warnings.push_back("not loaded: " + u);
            job->read = std::move(r);
            setFraction(job->progress, 0.3f);
        },
        [this, alive, job]
        {
            if (alive.expired())
                return;
            if (! job->read->ok())
            {
                busy_ = false;
                job->done(*job->read, std::move(job->report));
                return;
            }
            preload(job);
        });
}

void ProjectStore::preload(std::shared_ptr<OpenJob> job)
{
    if (pool_ == nullptr || job->cancelled())
    {
        finishOpen(job);
        return;
    }
    std::vector<model::SampleRef> wanted;
    for (const auto& e : job->project->sampleRefs.entries)
        if (! e.missing)
            wanted.push_back(e);
    job->report.samples = static_cast<int>(wanted.size());
    job->pending = static_cast<int>(wanted.size());
    if (wanted.empty())
    {
        finishOpen(job);
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    const float total = static_cast<float>(wanted.size());
    for (const auto& ref : wanted)
        pool_->request(ref,
                       [this, alive, job, id = ref.id, total](core::Result<services::library::SamplePool::Handle> r,
                                                              const services::library::LoadInfo&)
                       {
                           if (alive.expired())
                               return;
                           if (r)
                           {
                               core::audio::retain(r.value());
                               job->held.push_back(r.value());
                               ++job->report.samplesLoaded;
                           }
                           else
                           {
                               for (auto& e : job->project->sampleRefs.entries)
                                   if (e.id == id)
                                   {
                                       e.missing = true;
                                       job->report.warnings.push_back(e.path + ": " + r.error().message);
                                   }
                           }
                           const int doneCount = job->report.samples - (job->pending - 1);
                           setFraction(job->progress, 0.3f + 0.7f * static_cast<float>(doneCount) / total);
                           if (--job->pending == 0)
                               finishOpen(job);
                       });
}

void ProjectStore::finishOpen(std::shared_ptr<OpenJob> job)
{
    busy_ = false;
    const auto releaseHeld = [this, held = job->held]
    {
        // After the next pump: TrackSampleSync (onReplaced) has taken the document references by then.
        workers_.post(
            [held]
            {
                for (auto* h : held)
                    core::audio::release(h);
            });
    };
    if (job->cancelled()) // PRJ-051: cancellable until the swap; the open project is untouched
    {
        releaseHeld();
        if (job->written) // the import's fresh bundle (importForeign picked a free name): nothing was opened
            job->identity.deleteRecursively();
        job->report.cancelled = true;
        job->done(Result<void>::failure("Loading cancelled"), std::move(job->report));
        return;
    }
    for (const auto& e : job->project->sampleRefs.entries)
        if (e.missing)
            job->report.missing.push_back(e);
    // The swap: one replacement of the whole model (threading.md "Project load while playing").
    std::shared_ptr<model::Project> old = core::commands::replaceProject(bus_, std::move(job->project));
    const bool hasFile = ! job->writeFirst || job->written;
    adopt(job->identity, hasFile, job->report.readOnly);
    if (job->progress != nullptr)
        job->progress->fraction.store(1.0f, std::memory_order_relaxed);
    if (onReplaced)
        onReplaced(core::commands::project(bus_), job->report);
    releaseHeld();
    workers_.run([old] {}); // the previous model is freed off the message thread
    if (config_.resume && ! job->report.readOnly && hasFile)
    {
        const Selection sel = config_.selection ? config_.selection() : Selection {};
        workers_.run([resume = config_.resume, file = job->identity, sel] { (void) resume->markLoaded(file, sel); });
    }
    job->done({}, std::move(job->report));
}

void ProjectStore::save(SaveKind kind, const juce::String& requestedName, const juce::File& dir, Progress& progress,
                        std::function<void(Result<void>)> done)
{
    if (busy_)
        return done(busyError());
    if (! core::commands::hasProject(bus_))
        return done(Result<void>::failure("There is no project to save"));
    const juce::String name = requestedName.isEmpty() ? name_ : requestedName;
    juce::File target;
    if (kind == SaveKind::Save && hasFile_)
    {
        if (readOnly_) // PRJ-052
            return done(Result<void>::failure("This project was saved by a newer version: use SAVE AS"));
        target = file_;
    }
    else
    {
        if (const auto bad = validateProjectName(name))
            return done(Result<void>::failure(std::string(*bad)));
        const juce::File folder = kind == SaveKind::SaveTo ? dir : config_.projectsDir;
        if (kind == SaveKind::SaveTo && ! folder.isDirectory()) // PRJ-061: the volume went away
            return done(Result<void>::failure("Could not save: " + folder.getFullPathName().toStdString() +
                                              " is not available"));
        if (! folder.isDirectory() && ! folder.createDirectory())
            return done(Result<void>::failure("Could not save: cannot create " + folder.getFullPathName().toStdString(),
                                              errnoText(errno)));
        target = bundleFor(folder, name);
        if (target.exists() && ! isOpen(target))
            return done(Result<void>::failure("A project named \"" + name.toStdString() + "\" already exists in " +
                                              folder.getFullPathName().toStdString()));
    }
    busy_ = true;
    progress.fraction.store(0.0f, std::memory_order_relaxed);
    core::commands::flushNotifications(bus_); // pending batches count as "before the snapshot"
    const std::uint64_t changesAtSnapshot = changes_;
    if (! snapshot_)
        snapshot_ = std::make_shared<model::Project>();
    *snapshot_ = core::commands::project(bus_);
    if (captureRuntimeState)
        captureRuntimeState(*snapshot_);

    WriteOptions options;
    options.carryFrom = hasFile_ ? file_ : juce::File();
    if (kind != SaveKind::SaveTo && ! isExternal(target))
        options.backupFolder = config_.backupsDir.getChildFile(target.getFileNameWithoutExtension());
    options.backupsKept = config_.backupsKept;
    options.fault = writeFault_;
    options.progress = &progress;
    const bool exportCopy = kind == SaveKind::SaveTo; // Save to… writes a copy; the open project stays put
    const Selection sel = config_.selection ? config_.selection() : Selection {};
    auto result = std::make_shared<std::optional<Result<WriteReport>>>();
    std::weak_ptr<bool> alive = alive_;
    workers_.run(
        [snapshot = snapshot_, target, options, result, exportCopy, resume = config_.resume, sel]
        {
            *result = writeProjectVersion(*snapshot, target, options);
            if (result->value() && ! exportCopy && resume)
                (void) resume->markLoaded(target, sel);
        },
        [this, alive, result, target, changesAtSnapshot, exportCopy, &progress, done = std::move(done)]
        {
            if (alive.expired())
                return;
            busy_ = false;
            auto& r = result->value();
            if (! r)
            {
                // PRJ-061: name a volume that disappeared.
                if (! target.getParentDirectory().isDirectory())
                    return done(Result<void>::failure("Could not save: the volume " +
                                                          target.getParentDirectory().getFullPathName().toStdString() +
                                                          " was removed during the save",
                                                      r.error().message));
                return done(r.error());
            }
            progress.fraction.store(1.0f, std::memory_order_relaxed);
            if (! exportCopy)
            {
                // Clean only when nothing changed while the worker wrote; else the newer edits stay unsaved.
                if (changes_ == changesAtSnapshot && ! core::commands::notificationsPending(bus_))
                    bus_.checkpoint();
                if (target != file_ || ! hasFile_ || readOnly_)
                    adopt(target, true, false);
                else
                    hasFile_ = true;
                if (onSaved)
                    onSaved(target);
            }
            done({});
        });
}

Result<void> ProjectStore::remove(const ProjectInfo& info)
{
    if (isOpen(info.path))
        return Result<void>::failure("The open project cannot be deleted: open or create another project first");
    if (! looksLikeBundle(info.path))
        return Result<void>::failure("Could not delete: " + info.path.getFullPathName().toStdString() +
                                     " is not a project");
    if (config_.trashDir == juce::File())
    {
        if (! info.path.moveToTrash())
            return Result<void>::failure("Could not move " + info.path.getFullPathName().toStdString() +
                                         " to the trash");
    }
    else
    {
        if (! config_.trashDir.createDirectory())
            return Result<void>::failure("Could not delete: cannot create " +
                                             config_.trashDir.getFullPathName().toStdString(),
                                         errnoText(errno));
        const auto dest = uniqueChild(config_.trashDir, info.name.toStdString() + "-" + timestampName(),
                                      kNativeExtension.data());
        if (! info.path.moveFileTo(dest))
            return Result<void>::failure("Could not delete: cannot move " + info.path.getFullPathName().toStdString() +
                                             " (" + errnoText(errno) + ")",
                                         errnoText(errno));
    }
    if (config_.resume)
        if (const auto rec = config_.resume->read(); rec && rec->project == info.path)
            config_.resume->forget();
    return {};
}

Result<void> ProjectStore::rename(const ProjectInfo& info, const juce::String& newName)
{
    if (const auto bad = validateProjectName(newName))
        return Result<void>::failure(std::string(*bad));
    const bool open = isOpen(info.path);
    if (open && busy_)
        return busyError();
    const auto dest = bundleFor(info.path.getParentDirectory(), newName);
    if (dest.exists())
        return Result<void>::failure("A project named \"" + newName.toStdString() + "\" already exists");
    if (! info.path.moveFileTo(dest))
        return Result<void>::failure("Could not rename " + info.path.getFullPathName().toStdString() + " (" +
                                         errnoText(errno) + ")",
                                     errnoText(errno));
    const auto oldBackups = config_.backupsDir.getChildFile(info.name);
    const auto newBackups = config_.backupsDir.getChildFile(newName);
    if (! info.external && oldBackups.isDirectory() && ! newBackups.exists())
        oldBackups.moveFileTo(newBackups);
    if (open)
    {
        adopt(dest, true, readOnly_);
        if (config_.resume)
        {
            const Selection sel = config_.selection ? config_.selection() : Selection {};
            workers_.run([resume = config_.resume, dest, sel] { (void) resume->markLoaded(dest, sel); });
        }
    }
    return {};
}

Result<ProjectInfo> ProjectStore::duplicate(const ProjectInfo& info)
{
    if (! looksLikeBundle(info.path))
        return Result<ProjectInfo>::failure("Could not duplicate: " + info.path.getFullPathName().toStdString() +
                                            " is not a project");
    const auto folder = info.path.getParentDirectory();
    const auto stem = info.name.substring(0, static_cast<int>(kMaxNameLength) - 8).trimEnd();
    juce::String name = stem + " COPY";
    for (int n = 2; bundleFor(folder, name).exists(); ++n)
        name = stem + " COPY " + juce::String(n);
    AtomicWriter writer(bundleFor(folder, name));
    if (auto r = writer.carryOver(info.path, nullptr); ! r)
        return r.error();
    if (auto r = writer.commit(); ! r)
        return r.error();
    if (auto copy = readProjectInfo(bundleFor(folder, name), info.external))
        return *copy;
    return Result<ProjectInfo>::failure("Could not duplicate " + info.path.getFullPathName().toStdString());
}

Result<void> ProjectStore::restoreBackup(const ProjectInfo& backup, const ProjectInfo& project)
{
    if (isOpen(project.path))
        return Result<void>::failure("Open another project first, or open the backup and save it");
    if (! looksLikeBundle(backup.path))
        return Result<void>::failure("Could not restore: " + backup.path.getFullPathName().toStdString() +
                                     " is not a backup");
    // The current version becomes a backup too (nothing is lost by restoring).
    const auto folder = config_.backupsDir.getChildFile(project.name);
    if (looksLikeBundle(project.path) && folder.createDirectory())
    {
        AtomicWriter keep(uniqueChild(folder, timestampName(), kNativeExtension.data()));
        if (keep.carryOver(project.path, [](const juce::String& rel) { return ! isGenerated(rel); }))
            (void) keep.commit();
    }
    AtomicWriter writer(project.path);
    if (auto r = writer.carryOver(backup.path, [](const juce::String& rel) { return ! isGenerated(rel); }); ! r)
        return r;
    if (auto r = writer.carryOver(project.path, [](const juce::String& rel) { return isGenerated(rel); }); ! r)
        return r;
    return writer.commit();
}
} // namespace services::store
