// Schema versions and the migration chain of the native format (FMT-003, PRJ-052).
//
// A native bundle carries one integer schema version (meta.json `format.version`). On load the whole bundle is parsed
// into one JSON tree (the migration root, below), the chain `vN → vN+1 → … → kSchemaVersion` runs on it, then the tree
// is decoded into the model. Migrations are pure functions on that tree, registered with APP_REGISTER_MIGRATION
// (services/store/ProjectCodec.h) next to the model + codec change that needs them, e.g.:
//
//     void addFadeIn(juce::var& root) { ... }   // root["project"]["scenes"][i]["tracks"][t]["fadeIn"] = 0
//     APP_REGISTER_MIGRATION({1, 2, &addFadeIn, "Track.fadeIn defaults to 0"})
//
// and kSchemaVersion bumped to 2. Fields the writer omits because they hold their default (NativeCodec.h, "sparse
// fields") mean "the default of the version that wrote the file": a migration that changes a default must write the
// old default explicitly where the field is absent.
//
// The migration root (one object):
//     { "meta": <meta.json>, "project": <project.json>, "patterns": [ <patterns/scene-01.json> … <scene-16.json> ] }
// A missing pattern file is a void var in "patterns". The runner updates root.meta.format.version after each step.
// A migration may leave per-project details in root.migrationNotes (an array of strings, appendMigrationNote): the
// runner reports each after the migration's summary and removes the member, so it never reaches the decoder.
//
// Worker threads only (allocates; never the audio thread).
#pragma once

#include "services/store/ProjectCodec.h"

#include <span>
#include <string>
#include <vector>

namespace services::store
{
// The schema version this build writes and the newest it understands. Readers accept every older version (FMT-003);
// a newer file opens read-only (ImportReport::readOnly, PRJ-052).
// 2: MIDI enables default to ENABLE (MigrationV2.cpp)
// 3: project "origin" (import provenance); legacy SP-16 imports unscrambled, envelope k/127 → k/128 (MigrationV3.cpp)
// 4: MIDI NOTE2–4 as semitones from NOTE, 0 = OFF (MigrationV4.cpp)
inline constexpr int kSchemaVersion = 4;

inline constexpr const char* kMigrationNotes = "migrationNotes";
void appendMigrationNote(juce::var& root, const std::string& note);

class MigrationRunner
{
public:
    // `migrations` must outlive the runner. Migrations whose `to` is above `currentVersion` are ignored (they belong
    // to a newer schema than this runner targets).
    MigrationRunner(std::span<const Migration> migrations, int currentVersion);

    // The runner over every APP_REGISTER_MIGRATION of this build, targeting kSchemaVersion.
    static const MigrationRunner& builtin();

    int currentVersion() const noexcept { return current_; }

    // Problems of the chain: a version in [1, current) no migration starts from, two migrations from one version,
    // a migration that does not move forward. Empty = every older version reaches the current one.
    std::vector<std::string> validate() const;

    // Migrates `root` (the migration root above) from `fromVersion` to currentVersion() and returns the version
    // reached. `fromVersion` above currentVersion() is returned unchanged (the caller opens read-only); a gap in the
    // chain is an error. `applied` receives the summary of every migration run, in order.
    Result<int> run(juce::var& root, int fromVersion, std::vector<std::string>* applied = nullptr) const;

private:
    const Migration* step(int from) const;
    std::span<const Migration> migrations_;
    int current_;
};

// root.meta.format.version (0 when absent or not a number).
int schemaVersionOf(const juce::var& root);
void setSchemaVersion(juce::var& root, int version);
} // namespace services::store
