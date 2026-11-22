// E8 — export a project for the reference (FMT-044, LIB-041; spec 04-file-formats/sp16-interop.md, library-and-
// storage.md LIB-041). Behind the feature flag `ext.interop.export` (off by default).
//
//   Project ──buildTree──▶ ProjectData tree (the observed 1.6x schema: tools/prj-dump/prj-synth.py "observed") ──▶
//   writeContainer ("vltrgzip" + zlib 1) ──▶ <folder>/PIONEER DJ SAMPLER/Projects/<name>.prj
//                └──▶ samples: <folder>/PIONEER DJ SAMPLER/Samples/… as 44.1 kHz WAV ≤ 64 s + `.wav.dat` sidecars
//
// The tree is the exact inverse of the importer's mapping (services/interop/sp16/Sp16Import.cpp): every model field
// the importer reads is written under the name and in the encoding the importer reads first (observed names, packed
// hi/lo quantise and retrigger units, −1 "none" sentinels, int MIDI flags, "FxType_" effect names, "MT" stretch,
// `$…/%Smp/` sample URLs); step matrices through the importer's encodePattern + encodeSteps (the lock-ordinal map,
// ParamIndexMap); the tree through encodeTree / writeContainer. What the import kept (Extras "sp16.<Node>.<prop>",
// "sp16.node.<type>", cell overrides, kept value2) is restored where the model still holds what the import made of it,
// so generated → import → export → import is equal at model level (tests/unit/ext/export). What the format cannot hold
// is dropped and listed in the ExportReport (FMT-044): arrangement rows (OQ-02), track names, effect type memory,
// Hold/Glide lock changes, locks on slots the ordinal map does not export, extension data (native.* / ext.* extras),
// sample audio beyond 64 s.
//
// Safety (FMT-044): the export always goes to a NEW folder (refused when it exists), is assembled in "<folder>.partial"
// and renamed at the end, never overwrites a file, and refuses a target on an instrument disk (a folder below one
// named "PIONEER DJ SAMPLER" / "TORAIZ", or next to a "PIONEER DJ SAMPLER" holding Projects/ or System/). Loading a
// computer-written file on a unit is untested (research): at the user's own risk. Never copies to hardware.
// Worker threads only.
#pragma once

#include "core/model/Project.h"
#include "core/util/Result.h"
#include "services/interop/sp16/Sp16Mapping.h"
#include "services/interop/sp16/ValueTreeCodec.h"
#include "services/store/ProjectCodec.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace services::interop::sp16
{
inline constexpr double kExportSampleRate = 44100.0; // LIB-041: the rate the reference reads (M p.3)
inline constexpr double kExportMaxSeconds = 64.0;    // FMT-044

struct ExportSample
{
    std::string id;          // SampleKey id (pool entry)
    std::string url;         // audioSourceUrl written into PlaybackData
    std::string relPath;     // below the disk root ("Samples/Kit/kick.wav")
    core::model::SampleRef ref;
};

struct ExportOptions
{
    std::string name;                     // project name ("" = the folder's name)
    const ParamIndexMap* map = nullptr;   // null = the active map (user file or built-in)
    // Where a pool entry's audio is (null = SampleRef::path when it is an absolute path of an existing file).
    std::function<juce::File(const core::model::SampleRef&)> locate;
    bool writeSamples = true;
};

class ExportReport
{
public:
    void drop(const std::string& what, const std::string& where); // aggregated by `what`
    std::vector<std::string> lines() const;                         // "dropped <what> x<n> (first: <where>)", stats

    juce::File projectFile, diskRoot;
    int scenes = 0, patterns = 0, samples = 0, samplesCopied = 0, samplesConverted = 0, samplesTruncated = 0,
        samplesMissing = 0;
    struct Dropped
    {
        std::string what, first;
        int count = 0;
    };
    std::vector<Dropped> dropped;

private:
    std::map<std::string, std::size_t> index_;
};

// Pure: the reference tree of `project` and the samples it references (no file access).
Node buildExportTree(const core::model::Project&, const ExportOptions&, ExportReport&, std::vector<ExportSample>&);

// True when `folder` lies on (or is) an instrument disk layout (see the header comment).
bool isInstrumentDisk(const juce::File& folder);

// Writes the export into the new folder `folder` (must not exist; its parent must).
Result<void> exportProject(const core::model::Project&, const juce::File& folder, const ExportOptions&, ExportReport&);

// The writing codec of E8 (APP_REGISTER_CODEC): never sniffs or reads; write() exports into the new folder `folder`
// when `ext.interop.export` is on (refused otherwise).
class Sp16ExportCodec final : public services::store::ProjectCodec
{
public:
    services::store::CodecInfo info() const override; // {"sp16-prj-export", "Reference project (export)", {".prj"}, true}
    bool canRead(const juce::File&) const override { return false; }
    Result<std::unique_ptr<core::model::Project>> read(const juce::File&, services::store::ImportReport&) const override;
    Result<void> write(const core::model::Project&, const juce::File& folder) const override;
};
} // namespace services::interop::sp16
