// Sample sidecar metadata (LIB-023; data-model.md "SampleMetadata"): read the reference `.wav.dat` (a JUCE binary
// ValueTree, root `SampleAdjunctData`, fields `bar`, `bpm`, `sampleNum` — research docs/project-format.md
// "SampleAdjunctData"; `bars`/`sampleNumber` accepted too) and the app's own `<file>.json`; write only the `.json`,
// atomically (services::store::AtomicWriter, PRJ-013). `.wav.dat` files are never written.
//
// The ValueTree reader is an own bounded parser (≤ 64 KB file, ≤ 8 levels, ≤ 256 properties/children per node, no
// allocation from an untrusted count): JUCE's ValueTree::readFromData follows an array count from the stream, so a
// damaged file could make it loop billions of times (fuzzed: lib.fuzz).
//
// `.json` wins over `.dat` when both exist. Worker threads (listing) or the message thread (small files).
#pragma once

#include "core/model/Sample.h"
#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <cstddef>
#include <optional>

namespace services::library
{
class SidecarStore
{
public:
    static constexpr std::size_t kMaxSidecarBytes = 64 * 1024;

    core::model::SampleMetadata read(const juce::File& audio) const;
    // True when a sidecar of either kind exists and parsed (`out` filled).
    bool tryRead(const juce::File& audio, core::model::SampleMetadata& out) const;
    core::Result<void> write(const juce::File& audio, const core::model::SampleMetadata&) const;

    static juce::File jsonFor(const juce::File& audio);      // kick.wav → kick.wav.json
    static juce::File referenceFor(const juce::File& audio); // kick.wav → kick.wav.dat

    // Parsers (also fed by the fuzz test): nothing on any input that is not one of these files.
    static std::optional<core::model::SampleMetadata> parseReference(const void* data, std::size_t size);
    static std::optional<core::model::SampleMetadata> parseJson(const juce::String& text);
    static juce::String toJson(const core::model::SampleMetadata&);
};
} // namespace services::library
