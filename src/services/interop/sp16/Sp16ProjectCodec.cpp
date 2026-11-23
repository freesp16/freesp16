#include "services/interop/sp16/Sp16ProjectCodec.h"

#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/ValueTreeCodec.h"
#include "services/store/SampleRefs.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace services::interop::sp16
{
namespace
{
namespace m = ::core::model;
using Kind = ImportLog::Kind;

constexpr const char* kDiskRootNames[] = {"PIONEER DJ SAMPLER", "TORAIZ"}; // research storage.md §1; manual p.41

Result<std::vector<std::byte>> readAll(const juce::File& f, std::int64_t maxBytes)
{
    using R = Result<std::vector<std::byte>>;
    if (! f.existsAsFile())
        return R::failure("No file at " + f.getFullPathName().toStdString());
    const auto size = f.getSize();
    if (size > maxBytes)
        return R::failure(f.getFileName().toStdString() + " is larger than " + std::to_string(maxBytes) + " bytes");
    juce::FileInputStream in(f); // read-only (FMT-045)
    if (! in.openedOk())
        return R::failure("Cannot read " + f.getFullPathName().toStdString() + ": " +
                          in.getStatus().getErrorMessage().toStdString());
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (size > 0 && in.read(data.data(), static_cast<int>(size)) != static_cast<int>(size))
        return R::failure("Cannot read all of " + f.getFullPathName().toStdString());
    return data;
}

bool safeComponent(const juce::String& c)
{
    return c.isNotEmpty() && c != "." && c != ".." && ! c.containsChar('\\') && ! c.containsChar(':');
}

// Resolves audioSourceUrls against the volume roots and the disk root (see the header) and, in copy mode, copies
// into the library.
class SampleFinder
{
public:
    SampleFinder(juce::File root, const ParamIndexMap& map, const ImportOptions& opts, ImportLog& log)
        : root_(std::move(root)), map_(map), opts_(opts), log_(log)
    {
        // Every root once, the project's disk first (the by-name index walks them in this order).
        const auto add = [this](const juce::File& dir, const std::string& model)
        {
            if (dir != juce::File() && std::none_of(roots_.begin(), roots_.end(),
                                                    [&dir](const VolumeRoot& r) { return r.root == dir; }))
                roots_.push_back({{}, dir, model});
        };
        add(root_, {});
        for (const auto& v : opts_.volumes)
            add(v.root, v.modelPrefix);
        for (auto& r : roots_) // the project's disk may itself be a known volume: use its model path
            for (const auto& v : opts_.volumes)
                if (r.modelPrefix.empty() && v.root == r.root)
                    r.modelPrefix = v.modelPrefix;
    }

    int found = 0, byName = 0, copied = 0, datOk = 0, datBad = 0;
    double ms = 0; // time spent in sample lookups, hashing and copies (file system bound)

    m::SampleRef resolve(const std::string& url)
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        auto r = resolveTimed(url);
        ms += juce::Time::getMillisecondCounterHiRes() - t0;
        return r;
    }

private:
    // The model path of a file below one of the roots (the longest root with a model prefix), else absolute.
    std::string modelPathOf(const juce::File& f) const
    {
        const VolumeRoot* best = nullptr;
        for (const auto& r : roots_)
            if (! r.modelPrefix.empty() && f.isAChildOf(r.root) &&
                (best == nullptr || r.root.getFullPathName().length() > best->root.getFullPathName().length()))
                best = &r;
        if (best == nullptr)
            return f.getFullPathName().toStdString();
        return best->modelPrefix + "/" + f.getRelativePathFrom(best->root).replaceCharacter('\\', '/').toStdString();
    }

    m::SampleRef resolveTimed(const std::string& url)
    {
        m::SampleRef ref;
        ref.id = "sp16:" + url;
        ref.path = url;
        ref.missing = true;
        juce::File expected;
        const auto file = locate(url, expected);
        if (! file)
        {
            // Where it should be (the named volume first): resolves once that volume is mounted, and names the
            // file for the missing-media flow; the raw "$Ext0/…" URL never resolved.
            if (expected != juce::File())
            {
                ref.path = modelPathOf(expected);
                ref.displayName = expected.getFileName().toStdString();
            }
            return ref;
        }
        ++found;
        ref.displayName = file->getFileName().toStdString();
        const juce::File dat(file->getFullPathName() + ".dat");
        if (dat.existsAsFile())
        {
            if (readWavDat(dat))
                ++datOk;
            else
            {
                ++datBad;
                log_.issue(Kind::Structure, "unreadable .wav.dat (ignored)", dat.getFileName().toStdString());
            }
        }
        juce::File use = *file;
        std::string modelPath = modelPathOf(*file);
        if (opts_.copyTo != juce::File())
        {
            auto placed = copyIntoLibrary(*file);
            if (! placed)
            {
                log_.issue(Kind::SampleMissing, "copy failed: " + placed.error().message, url);
                return ref;
            }
            use = placed.value().first;
            modelPath = placed.value().second;
        }
        if (opts_.copyTo == juce::File())
        {
            // In place: the pool id stays "sp16:<url>" — hashing every sample here read each file once more before
            // the pool decodes it (over a second for about a hundred samples on a USB FAT volume); the pool keys files
            // by path and the missing-media flow matches non-hash ids by name.
            ref.path = modelPath;
            ref.missing = false;
            return ref;
        }
        auto id = services::store::contentId(use);
        if (! id)
        {
            log_.issue(Kind::SampleMissing, "unreadable: " + id.error().message, url);
            return ref;
        }
        ref.id = id.value();
        ref.path = modelPath;
        ref.missing = false;
        return ref;
    }

    std::optional<juce::File> locate(const std::string& url, juce::File& expected)
    {
        juce::StringArray parts;
        parts.addTokens(juce::String::fromUTF8(url.c_str()), "/", "");
        parts.removeEmptyStrings();
        if (parts.isEmpty())
            return std::nullopt;
        int k = 0;
        std::vector<juce::String> sub; // the volume's sub-folder of its disk root
        std::vector<juce::File> bases; // the named volume's roots first, then the project's disk
        if (parts[0].startsWith("$"))
        {
            const auto volume = parts[0].toStdString();
            bool known = false;
            for (const auto& v : opts_.volumes)
                if (v.volume == volume && v.root != juce::File())
                {
                    bases.push_back(v.root);
                    known = true;
                }
            if (auto v = map_.volumes.find(volume); v != map_.volumes.end())
            {
                juce::StringArray dirs;
                dirs.addTokens(juce::String::fromUTF8(v->second.c_str()), "/", "");
                dirs.removeEmptyStrings();
                for (const auto& d : dirs)
                    if (safeComponent(d))
                        sub.push_back(d);
            }
            else if (! known)
                log_.issue(Kind::Guessed, "volume " + volume + " resolved to the chosen disk root", url);
            ++k;
        }
        else
            log_.issue(Kind::Structure, "sample URL without a volume (looked up by name)", url);
        if (root_ != juce::File() && std::find(bases.begin(), bases.end(), root_) == bases.end())
            bases.push_back(root_);
        std::vector<juce::String> aliases {""};
        if (k < parts.size() && parts[k].startsWith("%"))
        {
            const auto alias = parts[k].toStdString();
            ++k;
            if (auto it = map_.pathAliases.find(alias); it != map_.pathAliases.end())
                aliases.assign(it->second.begin(), it->second.end());
            else
            {
                log_.issue(Kind::Guessed, "path alias " + alias + " unknown (tried Samples/ and the root)", url);
                aliases = {"Samples", ""};
            }
        }
        juce::String rest;
        for (int j = k; j < parts.size(); ++j)
        {
            if (! safeComponent(parts[j]))
            {
                log_.issue(Kind::Structure, "unsafe sample path refused", url);
                return std::nullopt;
            }
            rest += (rest.isEmpty() ? "" : "/") + parts[j];
        }
        if (rest.isEmpty() || bases.empty())
            return std::nullopt;
        for (const auto& root : bases)
            for (const auto& b : aliases)
            {
                juce::File f = root;
                for (const auto& d : sub)
                    f = f.getChildFile(d);
                juce::StringArray dirs;
                dirs.addTokens(b, "/", "");
                dirs.removeEmptyStrings();
                bool ok = true;
                for (const auto& d : dirs)
                {
                    ok = ok && safeComponent(d);
                    f = f.getChildFile(d);
                }
                if (! ok)
                    continue;
                if (expected == juce::File())
                    expected = f.getChildFile(rest);
                if (f.getChildFile(rest).existsAsFile())
                    return f.getChildFile(rest);
            }
        // By file name below every root (bounded), the first match in root order, then path order.
        const auto name = juce::File::createFileWithoutCheckingPath(rest).getFileName();
        buildIndex();
        if (auto it = index_.find(name.toLowerCase().toStdString()); it != index_.end())
        {
            ++byName;
            log_.issue(Kind::Guessed, "sample found by file name only", url);
            return it->second;
        }
        return std::nullopt;
    }

    void buildIndex()
    {
        if (indexed_)
            return;
        indexed_ = true;
        for (const auto& r : roots_)
        {
            if (std::any_of(roots_.begin(), roots_.end(),
                            [&r](const VolumeRoot& o) { return r.root.isAChildOf(o.root); }))
                continue; // walked with the root that holds it
            int files = 0;
            std::vector<juce::File> list;
            walk(r.root, 0, files, list);
            std::sort(list.begin(), list.end(), [](const juce::File& a, const juce::File& b)
                      { return a.getFullPathName() < b.getFullPathName(); });
            for (const auto& f : list)
                index_.emplace(f.getFileName().toLowerCase().toStdString(), f); // keeps the first of a name
        }
    }
    static void walk(const juce::File& dir, int depth, int& files, std::vector<juce::File>& out)
    {
        if (depth > 8 || files > 20000)
            return;
        for (const auto& e : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto f = e.getFile();
            if (f.getFileName().startsWithChar('.'))
                continue;
            if (e.isDirectory())
            {
                if (! f.isSymbolicLink())
                    walk(f, depth + 1, files, out);
            }
            else if (++files <= 20000)
            {
                const auto ext = f.getFileExtension().toLowerCase();
                if (ext == ".wav" || ext == ".aif" || ext == ".aiff")
                    out.push_back(f);
            }
        }
    }

    Result<std::pair<juce::File, std::string>> copyIntoLibrary(const juce::File& src)
    {
        using R = Result<std::pair<juce::File, std::string>>;
        // Keep the layout below the disk root (sanitised per component, FAT-safe), else just the file name.
        juce::StringArray rel;
        const auto holder = std::find_if(roots_.begin(), roots_.end(),
                                         [&src](const VolumeRoot& r) { return src.isAChildOf(r.root); });
        if (holder != roots_.end())
            rel.addTokens(src.getRelativePathFrom(holder->root).replaceCharacter('\\', '/'), "/", "");
        else
            rel.add(src.getFileName());
        rel.removeEmptyStrings();
        juce::File dest = opts_.copyTo;
        std::string model = opts_.copyModelPrefix;
        for (int j = 0; j < rel.size(); ++j)
        {
            const auto safe = services::store::safeFileName(rel[j].toStdString());
            dest = dest.getChildFile(juce::String::fromUTF8(safe.c_str()));
            model += "/" + safe;
        }
        if (dest.existsAsFile())
        {
            if (dest.getSize() == src.getSize())
            {
                const auto a = services::store::contentId(dest), b = services::store::contentId(src);
                if (a && b && a.value() == b.value())
                    return std::make_pair(dest, opts_.copyModelPrefix.empty() ? dest.getFullPathName().toStdString()
                                                                              : model);
            }
            return R::failure(dest.getFullPathName().toStdString() + " exists with other content");
        }
        if (! dest.getParentDirectory().createDirectory())
            return R::failure("cannot create " + dest.getParentDirectory().getFullPathName().toStdString());
        if (! src.copyFileTo(dest)) // reads the source only
            return R::failure("cannot copy to " + dest.getFullPathName().toStdString());
        const juce::File dat(src.getFullPathName() + ".dat");
        if (dat.existsAsFile())
            dat.copyFileTo(juce::File(dest.getFullPathName() + ".dat")); // the library reads it (SidecarStore)
        ++copied;
        return std::make_pair(dest, opts_.copyModelPrefix.empty() ? dest.getFullPathName().toStdString() : model);
    }

    juce::File root_;
    const ParamIndexMap& map_;
    const ImportOptions& opts_;
    ImportLog& log_;
    std::vector<VolumeRoot> roots_; // the disk root and the volume roots, each once (the volume field unused)
    bool indexed_ = false;
    std::map<std::string, juce::File> index_;
};

void collect(const juce::File& dir, int depth, bool hidden, std::vector<juce::File>& out)
{
    if (depth > 6 || out.size() >= 2000 || ! dir.isDirectory())
        return;
    for (const auto& e : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findFilesAndDirectories))
    {
        const auto f = e.getFile();
        const auto name = f.getFileName();
        if (e.isDirectory())
        {
            if (! name.startsWithChar('.') && ! f.isSymbolicLink())
                collect(f, depth + 1, hidden, out);
            continue;
        }
        const bool isHidden = name.startsWithChar('.');
        const bool prj = name.endsWithIgnoreCase(".prj");
        const bool rsm = name.equalsIgnoreCase("data.rsm") && dir.getFileName().equalsIgnoreCase("System");
        if ((prj || rsm) && isHidden == hidden && ! name.startsWith("._")) // "._x" = macOS metadata, not a project
            out.push_back(f);
        if (out.size() >= 2000)
            return;
    }
}
std::mutex& copyMutex()
{
    static std::mutex m;
    return m;
}
juce::String& copyRoot()
{
    static juce::String root;
    return root;
}
std::vector<VolumeRoot>& volumeTable()
{
    static std::vector<VolumeRoot> table; // guarded by copyMutex()
    return table;
}
} // namespace

void setCopyLibrary(const juce::File& libraryRoot)
{
    std::lock_guard<std::mutex> lock(copyMutex());
    copyRoot() = libraryRoot == juce::File() ? juce::String() : libraryRoot.getFullPathName();
}

juce::File copyLibrary()
{
    std::lock_guard<std::mutex> lock(copyMutex());
    return copyRoot().isEmpty() ? juce::File() : juce::File(copyRoot());
}

void setVolumeRoots(std::vector<VolumeRoot> roots)
{
    std::lock_guard<std::mutex> lock(copyMutex());
    volumeTable() = std::move(roots);
}

std::vector<VolumeRoot> volumeRoots()
{
    std::lock_guard<std::mutex> lock(copyMutex());
    return volumeTable();
}

namespace
{
ImportOptions libraryCopyOptions(const juce::File& prj, const juce::File& library)
{
    ImportOptions opts;
    if (library != juce::File())
    {
        const auto name = services::store::safeFileName(prj.getFileNameWithoutExtension().toStdString());
        opts.copyTo =
            library.getChildFile("Samples").getChildFile("Imported").getChildFile(juce::String::fromUTF8(name.c_str()));
        opts.copyModelPrefix = "$Lib/Samples/Imported/" + name;
    }
    opts.volumes = volumeRoots();
    return opts;
}
} // namespace

Result<WavDat> parseWavDat(std::span<const std::byte> bytes)
{
    Limits limits;
    limits.maxDepth = 4;
    limits.maxChildren = 256;
    limits.maxString = 4096;
    auto tree = decodeTree(bytes, limits);
    if (! tree)
        return tree.error();
    const auto& n = tree.value();
    if (splitSuffix(n.type).first != "SampleAdjunctData")
        return Result<WavDat>::failure("Not a sample sidecar (root " + n.type + ")");
    WavDat w;
    if (const auto* v = n.prop("bar"); v != nullptr && v->isNumber())
        w.bar = v->number();
    if (const auto* v = n.prop("bpm"); v != nullptr && v->isNumber())
        w.bpm = v->number();
    if (const auto* v = n.prop("sampleNum"); v != nullptr && v->t == Var::T::Int)
        w.sampleNum = static_cast<int>(v->i);
    return w;
}

Result<WavDat> readWavDat(const juce::File& f)
{
    auto data = readAll(f, 64 * 1024);
    if (! data)
        return data.error();
    return parseWavDat(data.value());
}

juce::File inferDiskRoot(const juce::File& file)
{
    for (auto d = file.getParentDirectory(); d.getParentDirectory() != d; d = d.getParentDirectory())
        for (const auto* name : kDiskRootNames)
            if (d.getFileName().equalsIgnoreCase(name))
                return d;
    const auto parent = file.getParentDirectory();
    if (parent.getFileName().equalsIgnoreCase("Projects") || parent.getFileName().equalsIgnoreCase("System"))
        return parent.getParentDirectory();
    return parent;
}

Result<std::unique_ptr<m::Project>> importProjectFile(const juce::File& file, const ImportOptions& opts,
                                                      services::store::ImportReport& report)
{
    using R = Result<std::unique_ptr<m::Project>>;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    auto bytes = readAll(file, kMaxProjectBytes);
    if (! bytes)
        return bytes.error();
    DecodeInfo info;
    std::size_t inflated = 0;
    const double tRead = juce::Time::getMillisecondCounterHiRes();
    auto tree = readContainer(bytes.value(), {}, &info, &inflated);
    const double tTree = juce::Time::getMillisecondCounterHiRes();
    if (! tree)
        return R::failure(file.getFileName().toStdString() + ": " + tree.error().message);

    ImportLog log;
    std::string mapNote;
    const auto map = activeParamIndexMap(mapNote);
    const juce::File root = opts.diskRoot != juce::File() ? opts.diskRoot : inferDiskRoot(file);
    log.line("file " + file.getFileName().toStdString() + " bytes=" + std::to_string(bytes.value().size()) +
             " inflated=" + std::to_string(inflated) + " nodes=" + std::to_string(info.nodes) + " props=" +
             std::to_string(info.props) + " depth=" + std::to_string(info.maxDepth));
    log.line("disk root " + root.getFullPathName().toStdString() +
             (opts.copyTo != juce::File() ? " (samples copied to " + opts.copyTo.getFullPathName().toStdString() + ")"
                                          : " (samples referenced in place)"));
    log.line(mapNote);

    SampleFinder finder(root, map, opts, log);
    MapOptions mo;
    mo.map = &map;
    mo.samples = [&finder](const std::string& url) { return finder.resolve(url); };
    auto project = std::make_unique<m::Project>();
    MapStats stats;
    if (auto r = mapProject(tree.value(), *project, mo, log, &stats); ! r)
        return R::failure(file.getFileName().toStdString() + ": " + r.error().message);
    const double tMap = juce::Time::getMillisecondCounterHiRes();
    project->origin.file = file.getFileName().toStdString();
    project->origin.contentId = services::store::contentIdOf(bytes.value().data(), bytes.value().size());

    const auto& s = project->settings;
    log.line("version versionNo=" + std::to_string(s.formatVersion) + " minorVersionNo=" +
             std::to_string(s.formatMinorVersion) + " (the numbering is not documented); importer " +
             std::to_string(kImporterVersion));
    std::string list;
    for (const auto& p : stats.patternList)
        list += (list.empty() ? "" : ",") + p;
    log.line("content scenes=" + std::to_string(stats.scenes) + " tracks=" + std::to_string(stats.tracks) +
             " patterns=" + std::to_string(stats.patterns) + " [" + (list.empty() ? "-" : list) + "] triggers=" +
             std::to_string(stats.triggers) + " lockCells=" + std::to_string(stats.lockCells) + " locks=" +
             std::to_string(stats.locks) + " verbatimPatterns=" + std::to_string(stats.verbatimPatterns) +
             " coordinateMismatches=" + std::to_string(stats.coordinateMismatches));
    log.line("samples referenced=" + std::to_string(stats.samples) + " found=" + std::to_string(finder.found) +
             " missing=" + std::to_string(stats.samplesMissing) + " byName=" + std::to_string(finder.byName) +
             " copied=" + std::to_string(finder.copied) + " wavdat=" + std::to_string(finder.datOk) +
             " wavdatBad=" + std::to_string(finder.datBad));
    if (opts.verifyRoundTrip)
    {
        const auto again = writeContainer(tree.value());
        const auto& orig = bytes.value();
        if (again == orig)
            log.line("roundtrip tree identical (" + std::to_string(orig.size()) + " bytes)");
        else
        {
            const auto treeAgain = encodeTree(tree.value());
            auto inner = inflateZlib(std::span(orig).subspan(kContainerMagic.size()), Limits {}.maxInflated);
            const bool treeSame = inner && inner.value() == treeAgain;
            std::size_t at = 0;
            while (at < std::min(again.size(), orig.size()) && again[at] == orig[at])
                ++at;
            log.line("roundtrip tree DIFFERS at byte " + std::to_string(at) + " (rebuilt " +
                     std::to_string(again.size()) + " vs " + std::to_string(orig.size()) + " bytes; inflated tree " +
                     (treeSame ? "identical: only the zlib output differs" : "differs") +
                     "; non-minimal ints " + std::to_string(info.nonMinimalInts) + ")");
        }
    }
    const double tEnd = juce::Time::getMillisecondCounterHiRes();
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "time %.1f ms (read %.1f, inflate+tree %.1f, map %.1f, of which samples %.1f, round-trip check %.1f)",
                  tEnd - t0, tRead - t0, tTree - tRead, tMap - tTree - finder.ms, finder.ms, tEnd - tMap);
    log.line(buf);

    for (auto& line : log.lines())
    {
        const bool kept = line.rfind("unknown-prop ", 0) == 0 || line.rfind("unknown-node ", 0) == 0 ||
                          line.rfind("model-lacks ", 0) == 0;
        (kept ? report.unmapped : report.warnings).push_back(std::move(line));
    }
    return project;
}

services::store::CodecInfo Sp16ProjectCodec::info() const
{
    static constexpr std::string_view kExtensions[] = {".prj", ".rsm"};
    return {"sp16-prj", "Reference project", kExtensions, false};
}

bool Sp16ProjectCodec::canRead(const juce::File& f) const
{
    if (! f.existsAsFile() || f.getSize() < 8)
        return false;
    juce::FileInputStream in(f);
    char magic[8] {};
    return in.openedOk() && in.read(magic, 8) == 8 && hasContainerMagic(asBytes(std::string_view(magic, 8)));
}

Result<std::unique_ptr<m::Project>> Sp16ProjectCodec::read(const juce::File& f,
                                                           services::store::ImportReport& report) const
{
    return importProjectFile(f, libraryCopyOptions(f, copyLibrary()), report);
}

Result<void> Sp16ProjectCodec::write(const m::Project&, const juce::File&) const
{
    return Result<void>::failure("Writing reference projects is not supported");
}

Result<std::vector<juce::File>> Sp16DiskImporter::findProjects(const juce::File& root) const
{
    if (! root.isDirectory())
        return Result<std::vector<juce::File>>::failure("No folder at " + root.getFullPathName().toStdString());
    std::vector<juce::File> out;
    collect(root, 0, false, out);
    std::sort(out.begin(), out.end(),
              [](const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); });
    return out;
}

std::vector<juce::File> Sp16DiskImporter::findHiddenStaging(const juce::File& root) const
{
    std::vector<juce::File> out;
    collect(root, 0, true, out);
    std::sort(out.begin(), out.end(),
              [](const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); });
    return out;
}

Result<std::unique_ptr<m::Project>> Sp16DiskImporter::importProject(const juce::File& prj, const juce::File& diskRoot,
                                                                    services::store::ImportReport& report) const
{
    auto opts = libraryCopyOptions(prj, library_);
    opts.diskRoot = diskRoot;
    return importProjectFile(prj, opts, report);
}
} // namespace services::interop::sp16

APP_REGISTER_CODEC(::services::interop::sp16::Sp16ProjectCodec)
