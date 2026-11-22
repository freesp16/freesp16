// Lookup over the registered ProjectCodecs (APP_REGISTER_CODEC, services/store/ProjectCodec.h) and the project entry
// points used by the project store: openProject / saveProject.
//
// Order: registration order across files is unspecified (core/util/Registrar.h), so lookups sort by codec id, with
// the native codec first (FMT-001: the native format is the source of truth; others are import/export).
// Worker threads only.
#pragma once

#include "services/store/NativeCodec.h"
#include "services/store/ProjectCodec.h"

#include <string_view>
#include <vector>

namespace services::store
{
std::vector<const ProjectCodec*> codecs();                     // sorted as above
const ProjectCodec* findCodec(std::string_view id);            // by CodecInfo::id
const ProjectCodec* codecForExtension(std::string_view ext);   // ".sixteen", ".prj" (case-insensitive)
const ProjectCodec* codecFor(const juce::File&);               // the first whose canRead() sniff accepts the file

// Opens any project a registered codec reads. A native bundle whose last save was interrupted is recovered first
// (AtomicWriter::recover; never call it while a save of the same bundle runs). The returned Project is ≈ 2.3 MB:
// move it to heap storage at once, or use openProjectInto.
Result<std::unique_ptr<model::Project>> openProject(const juce::File& path);
Result<std::unique_ptr<model::Project>> openProject(const juce::File& path, ImportReport& report);
Result<void> openProjectInto(const juce::File& path, model::Project& out, ImportReport& report);

// Saves in the native format (atomic, FMT-002).
Result<void> saveProject(const model::Project&, const juce::File& bundle, const SaveOptions& = {},
                         SaveReport* report = nullptr);
} // namespace services::store
