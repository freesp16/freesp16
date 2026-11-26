// The project lifecycle (spec 02-features/project-management.md PRJ-001…006, 009…014, 050/051,
// 060/061; native-format.md; threading.md "Project load while playing"): new / open / save / save as / save to /
// delete / rename / duplicate on top of the native codec and AtomicWriter, with dirty tracking through the
// CommandBus checkpoint and rotating backups.
//
//   message thread                                   worker (services::library::Workers)
//   save():  flush notifications, copy the model ──▶ back up the version on disk (Backups/<name>/, last 10, hard
//            into a reused heap snapshot (≈ 0.1 ms     links) → write the new version through AtomicWriter (staging →
//            host, bench_project `snapshot`)          fsync → renames → fsync) → rotate → Resume::markLoaded
//   done:    checkpoint() unless the model changed while the worker wrote (then it stays dirty)
//   open():  post TransportCmd{Stop, 1} ──────────▶ recover + read + migrate into a heap Project, resolvePool
//            request every present sample from the pool (progress per sample, cancellable) → one swap: the bus takes
//            the new model (history cleared, clean), onReplaced() rebuilds the engine side (TrackSampleSync::syncAll,
//            parameters, screens) → Resume::markLoaded on a worker
//
// Bundles live in `projectsDir` (<data>/Projects/<name>.sixteen); external ones (Save to…, PRJ-060) anywhere. Names
// follow validateProjectName (PRJ-004). The model is never touched off the message thread; a Project never lives on a
// stack. Message-thread API unless noted.
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/util/Result.h"
#include "engine/EngineApi.h"
#include "services/library/SamplePool.h"
#include "services/library/Workers.h"
#include "services/store/AtomicWriter.h"
#include "services/store/SampleRefs.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace services::store
{
class Resume;
struct Selection;

using Progress = services::library::Progress; // fraction 0–1 + cancel flag, polled by the UI's Progress popup

struct ProjectInfo
{
    juce::String name;          // the bundle's name without ".sixteen"
    juce::File path;            // the bundle folder
    juce::Time modified;        // meta.json "modified" (the file's time when absent)
    int scenesWithContent = 0;  // scenes whose pattern file holds a non-default pattern (from file sizes)
    bool external = false;      // outside projectsDir (USB mark, PRJ-005/060)
    // A reference project file (.prj) in a read-only location (the internal disk, a USB stick): OPEN imports it as a
    // new native project (importForeign); it is never written, renamed or deleted. `location` = the badge text.
    bool foreign = false;
    juce::String location;
};

// A folder of reference projects (.prj files) listed by OPEN as read-only entries.
struct ForeignFolder
{
    juce::File folder;
    juce::String location; // "INTERNAL", "USB"
};
enum class SortBy : std::uint8_t { Name, Modified };
enum class SaveKind : std::uint8_t { Save, SaveAs, SaveTo };

struct LoadReport
{
    std::vector<core::model::SampleRef> missing; // PRJ-040/041: listed, their tracks silent and marked
    std::vector<std::string> warnings;           // ImportReport warnings, recovery actions, sample decode errors
    bool readOnly = false;                       // PRJ-052: a newer format, never saved over
    bool cancelled = false;                      // PRJ-051
    bool foreign = false;                        // importForeign: `warnings` are the reference project's import report
    int samples = 0, samplesLoaded = 0;          // pool entries requested / decoded
};

// PRJ-004: 1–64 characters (code points), none of / \ : * ? " < > |, no leading '.'; also (ours) no control
// characters and no trailing space or '.' (FAT drops them, so the file would not match the name). nullopt = valid,
// otherwise the reason (UI wording). Used live by the name dialog.
std::optional<std::string_view> validateProjectName(std::u32string_view) noexcept;
std::optional<std::string_view> validateProjectName(const juce::String&) noexcept;

inline constexpr int kBackupsKept = 10;                 // PRJ-012
inline constexpr const char* kUntitledName = "Untitled"; // a new project before its first save

struct StoreConfig
{
    juce::File projectsDir;  // <data>/Projects
    juce::File backupsDir;   // <data>/Backups (Backups/<name>/<timestamp>.sixteen)
    juce::File trashDir;     // delete target; empty = the OS trash (host). Device: <data>/System/trash
    std::function<std::vector<juce::File>()> externalFolders; // folders listed as external (mounted volumes)
    std::function<std::vector<ForeignFolder>()> foreignFolders; // reference projects (internal disk, USB), read-only
    std::function<SampleRoots()> sampleRoots;                   // $Lib / $Ext roots (SampleLibrary::roots)
    std::function<Selection()> selection;                       // the UI's selection, for resume (PRJ-023)
    std::shared_ptr<Resume> resume;                              // markLoaded after every save and load
    int backupsKept = kBackupsKept;
};

// What one write of a version did (worker side; also used by the fork-and-kill test directly).
struct WriteOptions
{
    juce::File carryFrom;      // the bundle the project was opened from (SaveAs/SaveTo keep its samples/, PRJ-014)
    juce::File backupFolder;   // where the version on disk is backed up first; empty = no backup
    int backupsKept = kBackupsKept;
    SampleRoots roots;         // Collect & Save (only when the source has $Bundle samples and the target differs)
    std::string timestamp;     // "" = now
    std::function<AtomicWriter::Fault(AtomicWriter::Step)> fault; // tests: fault injection into the project write
    Progress* progress = nullptr;
};
struct WriteReport
{
    std::vector<std::string> warnings; // a failed backup never fails the save; it is reported here
    juce::File backup;                 // the backup made (if any)
    std::size_t bytesWritten = 0;
};
// Worker thread (any thread but audio): backs up the version at `bundle` (when one exists and backupFolder is set),
// writes `project` atomically, rotates the backups. On failure the version on disk is untouched and the error names
// the path and the OS error (PRJ-011).
Result<WriteReport> writeProjectVersion(const core::model::Project&, const juce::File& bundle, const WriteOptions&);
// The bundles in a backup folder, oldest first (leftovers of interrupted writes recovered first).
std::vector<juce::File> listBackups(const juce::File& backupFolder);
// Reads only the bundle's header: the first 2 KB of meta.json and the sizes in patterns/ (no JSON tree, no pool).
std::optional<ProjectInfo> readProjectInfo(const juce::File& bundle, bool external);
juce::File bundleFor(const juce::File& folder, const juce::String& name); // <folder>/<name>.sixteen

// NFR-011 guards in front of the codec's recursive JSON parser (juce::JSON): at most `maxDepth` nested arrays/objects
// (a worker's stack is small; the native format nests ≈ 10 deep) and at most kMaxProjectFileBytes per file.
inline constexpr int kMaxJsonDepth = 256;
inline constexpr std::int64_t kMaxProjectFileBytes = 64ll << 20;
bool jsonNestingWithin(std::string_view text, int maxDepth = kMaxJsonDepth) noexcept;
// Checks every JSON file of a native bundle (meta.json, project.json, patterns/); an error names the file.
Result<void> checkBundleInputs(const juce::File& bundle);

class ProjectStore
{
public:
    ProjectStore(core::CommandBus&, services::library::Workers&, StoreConfig);
    ~ProjectStore(); // pending completions become no-ops; a running worker job finishes on its own copy
    ProjectStore(const ProjectStore&) = delete;
    ProjectStore& operator= (const ProjectStore&) = delete;

    // Optional collaborators (the composition root sets them): the engine (Stop before a load) and the pool (load
    // every sample before the swap). Without a pool the samples load after the swap through TrackSampleSync.
    void setEngine(engine::EngineBridge* bridge) noexcept { bridge_ = bridge; }
    void setPool(services::library::SamplePool* pool) noexcept { pool_ = pool; }

    // ---- browsing (PRJ-005) ----
    std::vector<ProjectInfo> list(SortBy) const; // projectsDir + externalFolders() + foreignFolders(), headers only
    // OPEN of a foreign entry: a reference project imported as a new native project <projectsDir>/<name>[ N].sixteen,
    // written there on the worker before the swap (its samples stay where they are, nothing is written beside the
    // source). A failed write opens it unsaved (a "not saved: …" warning).
    void importForeign(const ProjectInfo&, Progress&, std::function<void(Result<void>, LoadReport)> done);
    std::vector<ProjectInfo> backups(const ProjectInfo&) const; // PRJ-012, newest first

    // ---- lifecycle ----
    // PRJ-002/009: a new default project named `name` (validated; refused when a project of that name exists). Not
    // written until the first save. The transport stops; onReplaced runs.
    Result<void> create(const juce::String& name);
    void createUntitled(); // start-up without resume: "Untitled", saved under a name the user gives later
    // PRJ-050/051: asynchronous; `progress` must outlive the call (until `done`). `done` runs on the message thread.
    void open(const juce::File& bundle, Progress& progress, std::function<void(Result<void>, LoadReport)> done);
    // As open(), but the project is saved to `identity` afterwards (resume from the resume copy when the original is
    // gone).
    void openAs(const juce::File& source, const juce::File& identity, Progress&,
                std::function<void(Result<void>, LoadReport)> done);
    // PRJ-003/060: Save (the current file; an untitled project is saved under `name`), SaveAs (`name` in
    // projectsDir; refused when another project has that name), SaveTo (`name` in `dir`, external). Asynchronous.
    void save(SaveKind, const juce::String& name, const juce::File& dir, Progress&,
              std::function<void(Result<void>)> done);
    Result<void> remove(const ProjectInfo&);                         // PRJ-006: to the trash; not the open project
    Result<void> rename(const ProjectInfo&, const juce::String&);    // extension; backups follow
    Result<ProjectInfo> duplicate(const ProjectInfo&);               // extension: "<name> COPY", "<name> COPY 2", …
    Result<void> restoreBackup(const ProjectInfo& backup, const ProjectInfo& project); // PRJ-012, not the open one

    // ---- state ----
    bool dirty() const noexcept;
    bool busy() const noexcept { return busy_; }  // a save or load runs (a second one is refused)
    bool hasFile() const noexcept { return hasFile_; }  // saved at least once (else Save asks for a name)
    bool readOnly() const noexcept { return readOnly_; } // PRJ-052
    bool external() const noexcept { return external_; }
    const juce::String& name() const noexcept { return name_; }
    const juce::File& file() const noexcept { return file_; } // the bundle Save writes (may not exist yet)
    std::uint64_t changeCount() const noexcept { return changes_; } // observer batches seen (autosave, tests)
    // $Bundle + the configured roots for the open project (the SamplePool resolver; PRJ-040 resolution).
    SampleRoots roots() const;

    // Refuses to replace the active project at the start of a request or when its worker completes.
    std::function<Result<void>()> admitReplacement;

    // Called after create/open swapped the model (the composition root: TrackSampleSync::syncAll, parameter push,
    // screen rebuild, scene/pattern selection from project.settings).
    std::function<void(const core::model::Project&, const LoadReport&)> onReplaced;
    // Called after a successful save (Autosave drops its now-stale copy).
    std::function<void(const juce::File&)> onSaved;
    // Called whenever name() / file() change (create, open, save as, rename): the autosave target follows.
    std::function<void(const juce::String& name, const juce::File& file)> onIdentity;

    // Fault injection for the project write of the next saves (tests).
    void setWriteFault(std::function<AtomicWriter::Fault(AtomicWriter::Step)> f) { writeFault_ = std::move(f); }

private:
    struct OpenJob;
    void startOpen(const juce::File& source, const juce::File& identity, Progress&,
                   std::function<void(Result<void>, LoadReport)> done, bool writeFirst = false);
    void preload(std::shared_ptr<OpenJob>);
    void finishOpen(std::shared_ptr<OpenJob>);
    void stopTransport();
    void adopt(const juce::File& file, bool hasFile, bool readOnly);
    bool isExternal(const juce::File&) const;
    bool isOpen(const juce::File&) const;

    core::CommandBus& bus_;
    services::library::Workers& workers_;
    StoreConfig config_;
    engine::EngineBridge* bridge_ = nullptr;
    services::library::SamplePool* pool_ = nullptr;
    core::Subscription changesSub_;
    std::uint64_t changes_ = 0;
    std::shared_ptr<core::model::Project> snapshot_; // reused by every save (shared with the running job)
    juce::String name_ {kUntitledName};
    juce::File file_;
    bool hasFile_ = false, readOnly_ = false, external_ = false, busy_ = false;
    std::function<AtomicWriter::Fault(AtomicWriter::Step)> writeFault_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace services::store
