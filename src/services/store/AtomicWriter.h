// Crash-safe writes of a bundle folder or a single file (FMT-002, PRJ-010…014).
//
// Bundle protocol, for the bundle B = <parent>/<name> (every name stays in <parent>, nothing is written elsewhere):
//   0. write every file into the staging folder  B.tmp-<pid>-<random>/   (stagingFile / writeFile / carryOver)
//   1. seal:     fsync every staged file and folder, rename staging → B.new, fsync <parent>   (B.new = complete)
//   2. move-old: rename B → B.bak (when B exists), fsync <parent>
//   3. install:  rename B.new → B, fsync <parent>
//   4. clean:    delete B.bak, fsync <parent>
// The old bundle is never deleted before the new one is in place, and at every instant one complete bundle exists
// under B, B.new or B.bak, so recover() (run by commit() itself, by openProject() and by recoverLeftovers() at
// start-up) can always restore B: a staging folder is incomplete (deleted), B.new is complete (rolled forward), B.bak
// is the previous bundle (deleted once B is in place, restored when B is missing).
//
// FAT32 / exFAT variant (USB sticks; Mode::Fat, detected by Mode::Auto): a directory rename there is not atomic
// across a power cut — the new directory entry is written before the old one is removed, so a torn rename leaves two
// names for one cluster chain. The same steps run, but before each one a journal B.commit (a small plain file,
// fsync'ed) names the step in flight, and fsync on a folder may fail (tolerated). recover() reads the journal: when
// both names of the in-flight rename exist, they may share clusters, so neither is ever deleted — the stale name is
// renamed aside to B.orphan-<n> (renaming never frees clusters), the newest complete bundle is installed as B, and the
// report asks for a disk check. Orphans are never deleted by the app.
//
// On failure (not a crash) commit() undoes what it did: the old bundle stays intact, the staging folder is removed and
// the error names the path and the OS error (PRJ-011). Single files (sidecars, PRJ-013) use writeFileAtomically:
// <file>.tmp-<pid>-<random> → fsync → rename over the target → fsync the folder.
//
// Host note: macOS fsync() does not flush the drive cache (F_FULLFSYNC would, at ~10 ms per call); the device's Linux
// fsync() does. Worker threads only.
#pragma once

#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace services::store
{
using ::core::Result;

class AtomicWriter
{
public:
    enum class Mode { Auto, Posix, Fat };
    enum class Step { Seal, MoveOld, Install, Clean };

    explicit AtomicWriter(juce::File bundle);
    AtomicWriter(juce::File bundle, Mode mode);
    ~AtomicWriter(); // an uncommitted (or failed) writer removes its staging folder
    AtomicWriter(const AtomicWriter&) = delete;
    AtomicWriter& operator= (const AtomicWriter&) = delete;

    const juce::File& bundle() const noexcept { return bundle_; }
    const juce::File& stagingFolder() const noexcept { return staging_; }
    Mode mode() const noexcept { return mode_; }

    juce::File stagingFile(const juce::String& relativePath); // write here (parent folders are created)
    // Writes one staged file in one call (the usual way: no second open for the fsync).
    Result<void> writeFile(const juce::String& relativePath, const void* data, std::size_t size);
    // Copies every file of `from` (recursively) into the staging folder unless `skip(relativePath)` says so; hard
    // links where the file system allows it (the files are never modified), copies otherwise. Used to keep a bundle's
    // samples/, previews/ and unknown files across saves (PRJ-014).
    Result<void> carryOver(const juce::File& from, const std::function<bool(const juce::String&)>& skip);

    Result<void> commit(); // fsync + rename protocol above; FAT32-safe variant

    struct Recovery
    {
        std::vector<std::string> actions; // what was done, for the log
        bool needsDiskCheck = false;      // FAT: a torn rename was found (orphans kept)
    };
    static Recovery recover(const juce::File& bundle);
    static void recoverLeftovers(const juce::File& folder); // on start-up: recover() every bundle with leftovers

    // Single-file atomic replace (sidecars, settings, autosave copies).
    static Result<void> writeFileAtomically(const juce::File& target, const void* data, std::size_t size);

    static Mode detectMode(const juce::File& folder); // Fat on msdos/vfat/exfat, else Posix

    // Fault injection (tests): called before each step; Crash stops there as a killed process would (nothing undone,
    // the staging folder stays), Torn (rename steps, Fat mode) leaves both names of the rename (a copy) and then
    // crashes, Fail makes the step fail with an OS-like error (commit() undoes and reports).
    enum class Fault { None, Crash, Torn, Fail };
    void setFaultHook(std::function<Fault(Step)> hook) { faultHook_ = std::move(hook); }

    // Sibling names of a bundle (also used by recovery and tests).
    static juce::File sealedOf(const juce::File& bundle);
    static juce::File backupOf(const juce::File& bundle);
    static juce::File journalOf(const juce::File& bundle);
    // Bundles recoverLeftovers() looks after (B.new / B.bak / B.commit are only touched for these).
    static constexpr const char* kBundleExtension = ".sixteen";

private:
    Result<void> fail(const std::string& what, const juce::File& path, int error);
    Result<void> fsyncTree(const juce::File& folder);
    Result<void> writeJournal(Step);
    Fault fault(Step step) const { return faultHook_ ? faultHook_(step) : Fault::None; }

    juce::File bundle_, staging_;
    Mode mode_;
    bool done_ = false, crashed_ = false;
    std::string initError_;
    std::vector<std::string> synced_; // staged files already fsync'ed by writeFile
    std::function<Fault(Step)> faultHook_;
};
} // namespace services::store
