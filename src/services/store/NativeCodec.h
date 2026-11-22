// The native project format v1 (spec 04-file-formats/native-format.md; FMT-001…003, 010…014, 020).
//
//   <name>.sixteen/
//   ├─ meta.json            { "format": { "name": "sixteen-project", "version": 1 }, "app": { "name", "version" },
//   │                         "created", "modified", "samplePool": { "entries": [ { id, location, path, name } ] } }
//   ├─ project.json         { "format": {…}, "settings": {…}, "scenes": [16 × { name, colour, bpm, tracks: [16],
//   │                         sendFx, masterFx, routing }], "arrangement", "ribbon", "midi" } — everything but steps
//   ├─ patterns/scene-NN.json   { "scene": NN, "patterns": [ { "index", "length", "mutes", "solos",
//   │                         "steps": [ { "track", "step", "trigger", "velocity", "type", "locks", "lockChange" } ] } ] }
//   ├─ samples/             Collect & Save copies (FMT-012), carried over unchanged by every save (PRJ-014)
//   └─ previews/ …          any other file of the bundle is carried over unchanged
//
// Our decisions for v1 (the spec gives a sketch only):
//   - Sparse (FMT-020): only patterns that differ from a default pattern and only non-default steps are written; every
//     entity omits members equal to its header default (NativeCodecSchema.h) — a default project is a few KB.
//   - Addressing in pattern files is 1-based (scene, index, track, step), as the spec's sketch; every other number is
//     the model's value. Locks are keyed by slot name ("pitch", "fx1.p2"; kLockSlotNames), lockChange only when not
//     Revert. Step-level opaque data lives in Pattern::extras as "native.step[s].track[t].<key>" (0-based) and
//     is written inside that step's object.
//   - FX slots (FMT-014): the current type's settings at the top, the other remembered types in "perType" keyed by
//     the stored type name; FX params stay 0–1 step fractions (FMT-013). Engineering units elsewhere are the model's.
//   - Samples (FMT-011): "location" is "$Lib", "$Bundle", "$Ext:<name>" or "absolute" (SampleRefs.h); `missing` is
//     not stored (resolved after load: resolvePool).
//   - Normalisation a round trip applies (read(write(p)) == p holds for models that already satisfy it, which
//     validate() requires): lock values of unmasked slots are 0 / Revert, Pattern::locks is in step-major order
//     with no unreferenced entries, a scene BPM of 0 reads as absent, extras order (NativeCodecSchema.h).
//   - Newer schema than kSchemaVersion: decoded best effort, ImportReport::readOnly set (PRJ-052); the caller must not
//     save over it. Unknown keys are kept in extras and written back.
//   - Text is UTF-8 JSON; doubles in shortest round-trip form; names ≤ 64 characters, plain files only (FAT32).
//
// Worker threads only. A Project is ≈ 2.3 MB: never on a thread stack — read into heap storage (readInto).
#pragma once

#include "services/store/Migrations.h"
#include "services/store/ProjectCodec.h"
#include "services/store/SampleRefs.h"

#include <string>
#include <string_view>
#include <vector>

namespace services::store
{
inline constexpr std::string_view kNativeFormatName = "sixteen-project";
inline constexpr std::string_view kNativeExtension = ".sixteen";

struct SaveOptions
{
    bool collectSamples = false; // FMT-012 Collect & Save: copy every referenced sample into samples/
    SampleRoots roots {};        // where $Lib / $Ext:<name> are (collect only)
    bool writeDefaults = false;  // write every member (debugging, the defaults fixture); files stay readable
    std::string timestamp {};    // ISO 8601 for meta.json created/modified; "" = now (tests pin it)
    std::string appName {}, appVersion {}; // meta.json "app"; "" = this build's identity (fixtures pin it)
};

struct SaveReport
{
    std::vector<std::string> warnings;    // e.g. a sample Collect & Save could not find
    model::SamplePool pool;               // the pool as written (Collect & Save rewrites locations to $Bundle)
    std::size_t bytesWritten = 0;
};

class NativeCodec final : public ProjectCodec
{
public:
    CodecInfo info() const override;
    bool canRead(const juce::File&) const override;
    Result<std::unique_ptr<model::Project>> read(const juce::File&, ImportReport&) const override;
    Result<void> write(const model::Project&, const juce::File&) const override; // atomic via AtomicWriter

    // Reads into caller-provided (heap) storage: no 2 MB temporaries. `out` is reset first.
    static Result<void> readInto(const juce::File& bundle, model::Project& out, ImportReport& report);
    static Result<void> save(const model::Project&, const juce::File& bundle, const SaveOptions& = {},
                             SaveReport* report = nullptr);

    // The encoded files without touching the disk (tests, benchmarks): name ("meta.json", "project.json",
    // "patterns/scene-01.json" …) and text, in write order.
    struct EncodedFile
    {
        std::string name, text;
    };
    static std::vector<EncodedFile> encode(const model::Project&, const SaveOptions& = {});
    // The migration root (Migrations.h) of a bundle on disk, parsed but not migrated or decoded.
    static Result<juce::var> load(const juce::File& bundle);
    // Decodes a migration root at kSchemaVersion into `out` (reset first).
    static void decode(const juce::var& root, model::Project& out, ImportReport& report);
};

// The bundle a path names: the folder itself, or the folder of a meta.json / project.json inside it.
juce::File nativeBundleOf(const juce::File&);
} // namespace services::store
