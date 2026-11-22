// The reference project import (FMT-040…043, FMT-045; LIB-040; spec 04-file-formats/sp16-interop.md):
// `.prj` files and the resume file `System/data.rsm` (the same container, research storage.md §7) are
// read through a separate ProjectCodec ("sp16-prj", read-only), `.wav.dat` sidecars by readWavDat.
//
//   file ──FileInputStream (read-only, FMT-045)──▶ readContainer ──▶ mapProject ──▶ Project + report
//                                                       │                └─ samples: audioSourceUrl "$Int0/%Tmp/x.wav"
//                                                       └─ round-trip check (writeContainer == the file, FMT-043)
//
// Samples: a URL's volume names a disk root (the folder holding Projects/, Samples/, System/ — `PIONEER DJ SAMPLER/`
// on the unit's disk, `PIONEER DJ SAMPLER/` or `TORAIZ/` on a stick): "$Int0" the internal disk, "$Ext0" a stick
// (ImportOptions::volumes, from setVolumeRoots on the open path). Lookup order: the named volume's roots, then the
// disk the project sits on, then by file name below all of them; "%<alias>" folders are resolved through the map's
// pathAliases. The codec (open path) references the found files in place (a volume the library knows by its "$Ext:"
// path, else absolute); the disk importer (LIB-040) copies them, with their `.wav.dat`, into the library first.
// Missing samples are pool entries marked missing whose path is the expected location (it resolves once the volume
// is mounted; PRJ-040: the missing-media flow relinks them by name).
//
// Report (FMT-042): ImportReport::warnings gets one line per summary item and per aggregated issue (the app logs each
// as "IMPORT <line>"); keys kept in Extras (unknown properties / nodes, fields the model lacks) go to `unmapped`.
// Nothing is ever written next to, or into, the source tree. Worker threads only.
#pragma once

#include "core/model/Project.h"
#include "core/util/Result.h"
#include "services/store/ProjectCodec.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace services::interop::sp16
{
using ::core::Result;

struct WavDat // SampleAdjunctData (research "SampleAdjunctData (.wav.dat) — confirmed")
{
    double bar = 0, bpm = 0;
    int sampleNum = 0;
};
Result<WavDat> readWavDat(const juce::File&);            // ≤ 64 KB, uncompressed tree, root SampleAdjunctData
Result<WavDat> parseWavDat(std::span<const std::byte>);

constexpr std::int64_t kMaxProjectBytes = 64ll << 20;

// "…/PIONEER DJ SAMPLER/Projects/x.prj" → "…/PIONEER DJ SAMPLER"; a TORAIZ folder likewise; else the parent of a
// Projects/ or System/ folder; else the file's folder.
juce::File inferDiskRoot(const juce::File& projectFile);

// One volume of the sample URLs: `root` is its disk root (".../PIONEER DJ SAMPLER"); `modelPrefix` its model path
// when the library knows the volume ("$Ext:INTERNAL/PIONEER DJ SAMPLER", survives another mount point), else empty
// (absolute paths). Several roots may share a volume ("$Ext0": every mounted stick).
struct VolumeRoot
{
    std::string volume; // "$Int0", "$Ext0"
    juce::File root;
    std::string modelPrefix;
};

struct ImportOptions
{
    juce::File diskRoot;             // empty = inferDiskRoot(file)
    std::vector<VolumeRoot> volumes; // empty = every volume resolves to diskRoot
    juce::File copyTo;               // empty = reference samples in place; else copy them below this folder
    std::string copyModelPrefix;     // the model path of copyTo ("$Lib/Samples/Imported/<name>"); empty = absolute
    bool verifyRoundTrip = true;     // FMT-043 tree-level check (one extra deflate of the tree)
};
Result<std::unique_ptr<core::model::Project>> importProjectFile(const juce::File&, const ImportOptions&,
                                                                services::store::ImportReport&);

// The codec's open path (ProjectCodec::read, e.g. ProjectStore::openAs): samples are referenced in place unless a
// library root is set here, then they are copied below <library>/Samples/Imported/<project>/ (model paths "$Lib/…").
// Message thread sets it (composition root / preferences); workers read it.
void setCopyLibrary(const juce::File& libraryRoot); // empty = reference in place
juce::File copyLibrary();
// The volumes the open path and the disk importer resolve sample URLs against (the composition root, from the
// internal disk and the mounted sticks). Message thread sets it; workers read it.
void setVolumeRoots(std::vector<VolumeRoot>);
std::vector<VolumeRoot> volumeRoots();

class Sp16ProjectCodec final : public services::store::ProjectCodec
{
public:
    services::store::CodecInfo info() const override; // {"sp16-prj", "Reference project", {".prj", ".rsm"}, false}
    bool canRead(const juce::File&) const override;   // a file whose first 8 bytes are "vltrgzip"
    Result<std::unique_ptr<core::model::Project>> read(const juce::File&, services::store::ImportReport&) const override;
    Result<void> write(const core::model::Project&, const juce::File&) const override; // not supported (FMT-044: P3)
};

// LIB-040: read-only scan of a disk copy or stick, and import with the samples copied into the library.
class Sp16DiskImporter
{
public:
    explicit Sp16DiskImporter(juce::File libraryRoot = {}) : library_(std::move(libraryRoot)) {}
    // Every visible .prj (and System/data.rsm) below `root`, sorted by path (≤ 6 levels, ≤ 2000 files).
    Result<std::vector<juce::File>> findProjects(const juce::File& root) const;
    // Hidden ".<name>.prj" staging files: a save the unit could not finish (research storage.md §5). Listed so the UI
    // can offer them; never renamed here.
    std::vector<juce::File> findHiddenStaging(const juce::File& root) const;
    // Copies the samples into <library>/Samples/Imported/<project name>/ (model paths "$Lib/…"), then maps. Without a
    // library root the samples are referenced in place.
    Result<std::unique_ptr<core::model::Project>> importProject(const juce::File& prj, const juce::File& diskRoot,
                                                                services::store::ImportReport&) const;

private:
    juce::File library_;
};
} // namespace services::interop::sp16
