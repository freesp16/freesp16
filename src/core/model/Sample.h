// Samples in the model (data-model.md "SamplePool", "SampleMetadata"): tracks reference samples by content id; the
// pool resolves an id to a file (FMT-011 locations) and records missing files as a state, never a dangling reference.
#pragma once

#include "core/model/Extras.h"

#include <string>
#include <vector>

namespace core::model
{
// A reference from a track to a pool entry. Empty id = no sample assigned.
struct SampleKey
{
    std::string id; // content hash (pool key)
    bool empty() const noexcept { return id.empty(); }
    friend bool operator== (const SampleKey&, const SampleKey&) = default;
};

struct SampleRef // data-model.md SamplePool entry
{
    std::string id;          // content hash
    std::string path;        // location: "$Lib/...", "$Bundle/...", "$Ext:<name>/...", or absolute (FMT-011)
    std::string displayName;
    bool missing = false;    // resolved at load; "relink missing files" acts on these
    Extras extras {};
    friend bool operator== (const SampleRef&, const SampleRef&) = default;
};

struct SamplePool
{
    std::vector<SampleRef> entries; // every sample used by any track, once
    Extras extras {};
    const SampleRef* find(const SampleKey& key) const noexcept
    {
        for (const auto& e : entries)
            if (e.id == key.id)
                return &e;
        return nullptr;
    }
    friend bool operator== (const SamplePool&, const SamplePool&) = default;
};

// Sidecar metadata of a sample file (`.wav.dat` in the reference; FMT, LIB). Not part of the Project tree.
struct SampleMetadata
{
    double bars = 0;       // length in bars at native tempo
    double bpm = 0;        // 0 = unknown
    int sampleNumber = 0;  // reference id
    float peak = 0, rms = 0, loudness = 0; // analysis results (services/analysis)
    Extras extras {};
    friend bool operator== (const SampleMetadata&, const SampleMetadata&) = default;
};
} // namespace core::model
