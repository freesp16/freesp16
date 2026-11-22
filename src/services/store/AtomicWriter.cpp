#include "services/store/AtomicWriter.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/vfs.h>
#endif

namespace services::store
{
namespace
{
std::string path(const juce::File& f) { return f.getFullPathName().toStdString(); }

std::string osError(int error) { return error == 0 ? std::string("unknown error") : std::string(std::strerror(error)); }

// fsync a file or folder by path; 0 or errno. Folders may refuse fsync on some file systems (EINVAL, ENOTSUP):
// tolerated, there is nothing more the app can do there.
int fsyncPath(const juce::File& f, bool folder)
{
    int flags = O_RDONLY;
#if defined(O_DIRECTORY)
    if (folder)
        flags |= O_DIRECTORY;
#endif
    const int fd = ::open(path(f).c_str(), flags);
    if (fd < 0)
        return folder && (errno == EACCES || errno == EINVAL) ? 0 : errno;
    int result = 0;
    if (::fsync(fd) != 0)
        result = errno;
    ::close(fd);
    if (folder && (result == EINVAL || result == ENOTSUP || result == EBADF || result == EROFS))
        return 0;
    return result;
}

int renamePath(const juce::File& from, const juce::File& to)
{
    return ::rename(path(from).c_str(), path(to).c_str()) == 0 ? 0 : errno;
}

int writeAll(int fd, const void* data, std::size_t size)
{
    auto p = static_cast<const char*>(data);
    while (size > 0)
    {
        const auto n = ::write(fd, p, size);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return errno;
        }
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return 0;
}

juce::String uniqueSuffix()
{
    juce::Random random;
    random.setSeedRandomly();
    return juce::String(static_cast<int>(::getpid())) + "-" +
           juce::String::toHexString(static_cast<juce::uint32>(random.nextInt())).paddedLeft('0', 8);
}

bool isHex8(const juce::String& s)
{
    return s.length() == 8 && s.containsOnly("0123456789abcdef");
}

// "<base>.tmp-<pid>-<8 hex>" → base, else "".
juce::String tmpBase(const juce::String& name)
{
    const auto at = name.lastIndexOf(".tmp-");
    if (at <= 0)
        return {};
    const auto rest = name.substring(at + 5);
    const auto dash = rest.indexOfChar('-');
    if (dash <= 0 || ! rest.substring(0, dash).containsOnly("0123456789") || ! isHex8(rest.substring(dash + 1)))
        return {};
    return name.substring(0, at);
}

const char* stepName(AtomicWriter::Step s)
{
    switch (s)
    {
        case AtomicWriter::Step::Seal: return "seal";
        case AtomicWriter::Step::MoveOld: return "move-old";
        case AtomicWriter::Step::Install: return "install";
        case AtomicWriter::Step::Clean: return "clean";
    }
    return "?";
}

int writeSmallFileSynced(const juce::File& f, const std::string& text)
{
    const int fd = ::open(path(f).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return errno;
    int error = writeAll(fd, text.data(), text.size());
    if (error == 0 && ::fsync(fd) != 0)
        error = errno;
    ::close(fd);
    return error;
}

// Recovery of one bundle (see AtomicWriter.h). `keep` = a staging folder name never touched (the caller's own).
class Recoverer
{
public:
    Recoverer(const juce::File& bundle, const juce::String& keep)
        : b_(bundle), n_(AtomicWriter::sealedOf(bundle)), k_(AtomicWriter::backupOf(bundle)),
          j_(AtomicWriter::journalOf(bundle)), parent_(bundle.getParentDirectory()), keep_(keep),
          fat_(AtomicWriter::detectMode(parent_) == AtomicWriter::Mode::Fat)
    {
    }

    AtomicWriter::Recovery run()
    {
        juce::String phase, staged;
        if (j_.existsAsFile())
        {
            const auto words = juce::StringArray::fromTokens(j_.loadFileAsString().trim(), " ", "");
            phase = words.size() > 0 ? words[0] : juce::String();
            staged = words.size() > 1 ? words[1] : juce::String();
            // NFR-011: the journal is read from the volume (a USB stick can carry anything): only a
            // staging folder of this bundle, by its own name, may be set aside — never a path elsewhere
            // ("seal ../../x" would have moved x into the project folder) nor the bundle itself.
            if (staged.containsAnyOf("/\\") || tmpBase(staged) != b_.getFileName())
                staged = {};
        }
        // FAT journal: both names of the rename in flight exist → it may have been torn (shared clusters).
        if (phase == "seal" && staged.isNotEmpty() && staged != keep_ && parent_.getChildFile(staged).exists() &&
            n_.exists())
        {
            orphan(parent_.getChildFile(staged));
            orphan(n_);
            r_.needsDiskCheck = true;
        }
        else if (phase == "move-old" && b_.exists() && k_.exists())
        {
            orphan(b_);
            orphan(k_);
            r_.needsDiskCheck = true;
        }
        else if (phase == "install" && n_.exists() && b_.exists())
        {
            orphan(n_);
            r_.needsDiskCheck = true;
        }

        for (const auto& entry : parent_.findChildFiles(juce::File::findFilesAndDirectories, false))
        {
            const auto name = entry.getFileName();
            if (name != keep_ && tmpBase(name) == b_.getFileName())
            {
                entry.deleteRecursively();
                note("removed the incomplete staging " + path(entry));
            }
        }

        if (n_.isDirectory())
        {
            if (b_.exists() && k_.exists()) // impossible with atomic renames; a torn FAT rename without its journal
            {
                orphan(k_);
                r_.needsDiskCheck = true;
            }
            if (b_.exists())
            {
                journal("move-old");
                if (const int e = renamePath(b_, k_))
                    return failed("move " + path(b_) + " aside", e);
                sync();
            }
            journal("install");
            if (const int e = renamePath(n_, b_))
                return failed("install " + path(n_), e);
            sync();
            note("completed the interrupted save of " + path(b_));
        }
        if (k_.exists())
        {
            if (b_.exists())
            {
                journal("clean");
                k_.deleteRecursively();
                note("removed the previous version " + path(k_));
            }
            else if (const int e = renamePath(k_, b_))
                return failed("restore " + path(k_), e);
            else
                note("restored the previous version of " + path(b_));
        }
        if (j_.existsAsFile())
            j_.deleteFile();
        sync();
        return r_;
    }

private:
    void note(std::string s) { r_.actions.push_back(std::move(s)); }
    AtomicWriter::Recovery failed(const std::string& what, int error)
    {
        note("could not " + what + ": " + osError(error));
        return r_;
    }
    void sync() { fsyncPath(parent_, true); }
    void journal(const char* phase)
    {
        if (fat_)
            writeSmallFileSynced(j_, phase);
    }
    void orphan(const juce::File& f)
    {
        for (int i = 1;; ++i)
        {
            const auto target = parent_.getChildFile(b_.getFileName() + ".orphan-" + juce::String(i));
            if (target.exists())
                continue;
            if (const int e = renamePath(f, target))
                note("could not set " + path(f) + " aside: " + osError(e));
            else
                note("kept " + path(f) + " as " + path(target) + " (possibly cross-linked: check the disk)");
            return;
        }
    }

    juce::File b_, n_, k_, j_, parent_;
    juce::String keep_;
    bool fat_;
    AtomicWriter::Recovery r_;
};
} // namespace

AtomicWriter::AtomicWriter(juce::File bundle) : AtomicWriter(std::move(bundle), Mode::Auto) {}

AtomicWriter::AtomicWriter(juce::File bundle, Mode mode) : bundle_(std::move(bundle)), mode_(mode)
{
    const auto parent = bundle_.getParentDirectory();
    if (mode_ == Mode::Auto)
        mode_ = detectMode(parent);
    staging_ = parent.getChildFile(bundle_.getFileName() + ".tmp-" + uniqueSuffix());
    if (! parent.isDirectory())
        initError_ = "The folder " + path(parent) + " does not exist";
    else if (const auto r = staging_.createDirectory(); r.failed())
        initError_ = "Cannot create " + path(staging_) + ": " + r.getErrorMessage().toStdString();
}

AtomicWriter::~AtomicWriter()
{
    if (! done_ && ! crashed_ && staging_.exists())
        staging_.deleteRecursively();
}

juce::File AtomicWriter::sealedOf(const juce::File& b) { return b.getSiblingFile(b.getFileName() + ".new"); }
juce::File AtomicWriter::backupOf(const juce::File& b) { return b.getSiblingFile(b.getFileName() + ".bak"); }
juce::File AtomicWriter::journalOf(const juce::File& b) { return b.getSiblingFile(b.getFileName() + ".commit"); }

juce::File AtomicWriter::stagingFile(const juce::String& relativePath)
{
    auto f = staging_.getChildFile(relativePath);
    f.getParentDirectory().createDirectory();
    return f;
}

Result<void> AtomicWriter::writeFile(const juce::String& relativePath, const void* data, std::size_t size)
{
    if (! initError_.empty())
        return Result<void>::failure(initError_);
    const auto f = stagingFile(relativePath);
    const int fd = ::open(path(f).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("write", f, errno);
    int error = writeAll(fd, data, size);
    if (error == 0 && ::fsync(fd) != 0)
        error = errno;
    if (::close(fd) != 0 && error == 0)
        error = errno;
    if (error != 0)
        return fail("write", f, error);
    synced_.push_back(path(f));
    return {};
}

Result<void> AtomicWriter::carryOver(const juce::File& from, const std::function<bool(const juce::String&)>& skip)
{
    if (! initError_.empty())
        return Result<void>::failure(initError_);
    if (! from.isDirectory())
        return {};
    for (const auto& entry : juce::RangedDirectoryIterator(from, true, "*", juce::File::findFiles))
    {
        const auto& src = entry.getFile();
        if (src.isSymbolicLink())
            continue; // never follow a link out of the bundle
        const auto rel = src.getRelativePathFrom(from);
        if (skip && skip(rel))
            continue;
        const auto dst = stagingFile(rel);
        if (dst.exists())
            continue; // the caller wrote a newer version
        if (::link(path(src).c_str(), path(dst).c_str()) != 0 && ! src.copyFileTo(dst))
            return fail("copy " + path(src) + " to", dst, errno);
    }
    return {};
}

Result<void> AtomicWriter::fail(const std::string& what, const juce::File& p, int error)
{
    return Result<void>::failure("Could not save: cannot " + what + " " + path(p) + " (" + osError(error) + ")",
                                 osError(error));
}

Result<void> AtomicWriter::fsyncTree(const juce::File& folder)
{
    std::vector<juce::File> folders;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFilesAndDirectories))
    {
        const auto& f = entry.getFile();
        if (entry.isDirectory())
        {
            folders.push_back(f);
            continue;
        }
        if (std::find(synced_.begin(), synced_.end(), path(f)) != synced_.end())
            continue;
        if (const int e = fsyncPath(f, false))
            return fail("flush", f, e);
    }
    folders.push_back(folder);
    for (const auto& f : folders)
        if (const int e = fsyncPath(f, true))
            return fail("flush", f, e);
    return {};
}

Result<void> AtomicWriter::writeJournal(Step step)
{
    if (mode_ != Mode::Fat)
        return {};
    std::string text = stepName(step);
    if (step == Step::Seal)
        text += " " + staging_.getFileName().toStdString();
    if (const int e = writeSmallFileSynced(journalOf(bundle_), text))
        return fail("write", journalOf(bundle_), e);
    return {};
}

Result<void> AtomicWriter::commit()
{
    if (done_ || crashed_)
        return Result<void>::failure("Could not save: this writer was already committed");
    if (! initError_.empty())
        return Result<void>::failure("Could not save: " + initError_);
    if (bundle_.existsAsFile())
        return Result<void>::failure("Could not save: " + path(bundle_) + " is a file, not a project folder");

    const auto parent = bundle_.getParentDirectory();
    const auto sealed = sealedOf(bundle_), backup = backupOf(bundle_);
    // An earlier interrupted save of this bundle is finished (or undone) first, so the names below are free.
    Recoverer(bundle_, staging_.getFileName()).run();
    if (sealed.exists() || backup.exists())
        return Result<void>::failure("Could not save: " + path(sealed) + " or " + path(backup) +
                                     " is left from an earlier save and cannot be removed");

    // Runs the fault hook of one step: true = stop now (the result is in `out`).
    Result<void> out;
    const auto injected = [&](Step step, const juce::File& from, const juce::File& to) -> bool
    {
        switch (fault(step))
        {
            case Fault::None: return false;
            case Fault::Torn:
                if (from.isDirectory())
                    from.copyDirectoryTo(to);
                [[fallthrough]];
            case Fault::Crash:
                crashed_ = true;
                out = Result<void>::failure("simulated crash before " + std::string(stepName(step)));
                return true;
            case Fault::Fail:
                out = fail(std::string(stepName(step)), from, EIO);
                return true;
        }
        return false;
    };
    const auto cleanup = [&]
    {
        staging_.deleteRecursively();
        sealed.deleteRecursively();
        journalOf(bundle_).deleteFile();
        fsyncPath(parent, true);
    };

    // 1. seal
    if (auto r = writeJournal(Step::Seal); ! r)
        return cleanup(), r;
    if (auto r = fsyncTree(staging_); ! r)
        return cleanup(), r;
    if (injected(Step::Seal, staging_, sealed))
        return crashed_ ? out : (cleanup(), out);
    if (const int e = renamePath(staging_, sealed))
        return cleanup(), fail("rename", staging_, e);
    fsyncPath(parent, true);

    // 2. move the old bundle aside
    const bool hadOld = bundle_.exists();
    if (hadOld)
    {
        if (auto r = writeJournal(Step::MoveOld); ! r)
            return cleanup(), r;
        if (injected(Step::MoveOld, bundle_, backup))
            return crashed_ ? out : (cleanup(), out);
        if (const int e = renamePath(bundle_, backup))
            return cleanup(), fail("move aside", bundle_, e);
        fsyncPath(parent, true);
    }

    // 3. install
    const auto undoMove = [&]
    {
        if (hadOld && backup.exists() && ! bundle_.exists())
            renamePath(backup, bundle_);
        cleanup();
    };
    if (auto r = writeJournal(Step::Install); ! r)
        return undoMove(), r;
    if (injected(Step::Install, sealed, bundle_))
        return crashed_ ? out : (undoMove(), out);
    if (const int e = renamePath(sealed, bundle_))
        return undoMove(), fail("install", sealed, e);
    fsyncPath(parent, true);
    done_ = true; // the new bundle is in place: from here on nothing is undone

    // 4. clean
    if (hadOld)
    {
        static_cast<void>(writeJournal(Step::Clean)); // without it recovery still sees B + B.bak and deletes B.bak
        const auto cleanFault = fault(Step::Clean);
        if (cleanFault == Fault::Crash || cleanFault == Fault::Torn)
        {
            crashed_ = true;
            return Result<void>::failure("simulated crash before clean");
        }
        if (cleanFault == Fault::None)
            backup.deleteRecursively(); // a failure only leaves B.bak for the next recovery
    }
    journalOf(bundle_).deleteFile();
    fsyncPath(parent, true);
    return {};
}

AtomicWriter::Recovery AtomicWriter::recover(const juce::File& bundle) { return Recoverer(bundle, {}).run(); }

void AtomicWriter::recoverLeftovers(const juce::File& folder)
{
    juce::StringArray bundles;
    for (const auto& entry : folder.findChildFiles(juce::File::findFilesAndDirectories, false))
    {
        const auto name = entry.getFileName();
        if (const auto base = tmpBase(name); base.isNotEmpty())
        {
            if (entry.existsAsFile())
                entry.deleteFile(); // a single-file write that never reached its rename
            else
                bundles.addIfNotAlreadyThere(base);
            continue;
        }
        for (const char* suffix : {".new", ".bak", ".commit"})
            if (name.endsWith(suffix))
            {
                const auto base = name.dropLastCharacters(static_cast<int>(std::strlen(suffix)));
                if (base.endsWithIgnoreCase(kBundleExtension))
                    bundles.addIfNotAlreadyThere(base);
            }
    }
    for (const auto& base : bundles)
        recover(folder.getChildFile(base));
}

Result<void> AtomicWriter::writeFileAtomically(const juce::File& target, const void* data, std::size_t size)
{
    const auto folder = target.getParentDirectory();
    const auto tmp = folder.getChildFile(target.getFileName() + ".tmp-" + uniqueSuffix());
    const auto failure = [&](const std::string& what, const juce::File& f, int e)
    {
        ::unlink(path(tmp).c_str());
        return Result<void>::failure("Could not save: cannot " + what + " " + path(f) + " (" + osError(e) + ")",
                                     osError(e));
    };
    const int fd = ::open(path(tmp).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return failure("write", tmp, errno);
    int error = writeAll(fd, data, size);
    if (error == 0 && ::fsync(fd) != 0)
        error = errno;
    if (::close(fd) != 0 && error == 0)
        error = errno;
    if (error != 0)
        return failure("write", tmp, error);
    if (const int e = renamePath(tmp, target))
        return failure("replace", target, e);
    fsyncPath(folder, true);
    return {};
}

AtomicWriter::Mode AtomicWriter::detectMode(const juce::File& folder)
{
    struct statfs info {};
    if (::statfs(path(folder).c_str(), &info) != 0)
        return Mode::Posix;
#if defined(__APPLE__)
    const std::string_view type(info.f_fstypename);
    return type == "msdos" || type == "exfat" ? Mode::Fat : Mode::Posix;
#else
    constexpr long kMsdos = 0x4d44, kExfat = 0x2011BAB0, kFuseblk = 0x65735546;
    const auto type = static_cast<long>(info.f_type);
    return type == kMsdos || type == kExfat || type == kFuseblk ? Mode::Fat : Mode::Posix;
#endif
}
} // namespace services::store
