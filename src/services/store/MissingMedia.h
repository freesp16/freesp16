// Missing media (PRJ-040/041; data-model.md "SamplePool": a missing file is a pool state, never a dangling reference).
//
// After a load the pool entries whose file is not found carry SampleRef::missing (ProjectStore::open, resolvePool);
// their tracks stay silent and marked (UID-504) — the project loads and plays. The user can:
//   - relink: search a chosen folder (recursively, on a worker) by file name, confirm each candidate by content hash
//     (the entry id, "sha256:…"), and point the matched entries at the found files — one undo step "RELINK";
//   - leave them empty: the tracks using them are cleared and the entries dropped — one undo step "CLEAR".
// Sample files are only ever read (hashing): never moved, renamed, modified or deleted (PRJ-014).
//
// The edits are Commands through the CommandBus (dirty, undo, bridge): they touch `sampleRefs` and the tracks'
// `playback.sample`, so TrackSampleSync reloads the relinked tracks. Message-thread API.
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "services/library/Workers.h"
#include "services/store/SampleRefs.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace services::store
{
struct RelinkReport
{
    int relinked = 0;
    std::vector<core::model::SampleRef> unmatched;         // still missing
    std::vector<std::string> notes;                         // e.g. "kick.wav: same name, different content"
    std::size_t filesScanned = 0;
    bool cancelled = false;
};

// One pool edit as a Command: the pool entries and some tracks' sample keys after it (the inverse restores both).
class PoolEdit final : public core::Command
{
public:
    struct TrackKey
    {
        std::uint8_t scene, track;
        core::model::SampleKey key;
    };
    // undoable = false: resolved state only (the missing marks), not an edit — no undo step, not dirty.
    PoolEdit(std::string label, std::vector<core::model::SampleRef> entries, std::vector<TrackKey> keys,
             bool undoable = true);

    std::string_view label() const override { return label_; }
    void apply(core::model::Project&) override;
    std::unique_ptr<core::Command> inverse(const core::model::Project& before) const override;
    void collectChanges(core::ChangeSet&) const override;
    std::size_t sizeBytes() const override;
    bool undoable() const override { return undoable_; }

private:
    std::string label_;
    std::vector<core::model::SampleRef> entries_;
    std::vector<TrackKey> keys_;
    bool undoable_ = true;
};

class MissingMedia
{
public:
    static constexpr std::size_t kMaxFilesScanned = 50000; // a relink search stops there

    MissingMedia(core::CommandBus&, services::library::Workers&, std::function<SampleRoots()> roots);
    ~MissingMedia();
    MissingMedia(const MissingMedia&) = delete;
    MissingMedia& operator= (const MissingMedia&) = delete;

    std::vector<core::model::SampleRef> missing() const; // pool entries marked missing, pool order
    // Tracks (scene, track) that use a missing sample: the UI marks them (UID-504).
    std::vector<std::pair<int, int>> silentTracks() const;
    // Re-checks every entry's file (stat only) and updates the marks; returns the number missing. Not an undo step:
    // `missing` is resolved state (the codec never stores it).
    int refresh();

    void relink(const juce::File& folder, std::function<void(RelinkReport)> done);
    void cancel() noexcept; // stops a running search (done gets cancelled = true)
    bool searching() const noexcept { return searching_; }
    void leaveEmpty(const std::vector<std::string>& ids); // one undo step; "" ids ignored

    // The worker side of relink, synchronous (tests, bench): the matches as (entry id, new model path).
    struct Match
    {
        std::string id, path;
        juce::File file;
    };
    static std::vector<Match> search(const std::vector<core::model::SampleRef>& missing, const juce::File& folder,
                                     const SampleRoots& roots, const std::atomic<bool>& cancel, RelinkReport& report);
    // The model path of `file`: "$Lib/…" or "$Ext:<name>/…" when it is under a root, else absolute.
    static std::string modelPathOf(const juce::File& file, const SampleRoots& roots);

private:
    core::CommandBus& bus_;
    services::library::Workers& workers_;
    std::function<SampleRoots()> roots_;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    bool searching_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace services::store
