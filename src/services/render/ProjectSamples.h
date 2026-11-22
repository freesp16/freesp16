// The samples of a project file for a render without the app (the render CLI, tests): every track slot's sample is
// decoded through a private SamplePool (2 workers) and returned as a SampleSnapshot — the same buffers the app's
// TrackSampleSync would install (LIB-030: decoded at `sampleRate`, resampled on load). The pool's buffers stay
// alive after the pool is gone only through the snapshot's references, so the caller owns the returned holder.
// Any thread but audio; blocks until every slot is decoded (or `timeoutSeconds`).
#pragma once

#include "core/model/Project.h"
#include "core/util/Result.h"
#include "services/render/OfflineEngine.h"
#include "services/store/SampleRefs.h"

#include <memory>
#include <string>
#include <vector>

namespace services::render
{
class LoadedSamples
{
public:
    LoadedSamples();
    ~LoadedSamples(); // releases the snapshot, then frees the pool's buffers
    LoadedSamples(const LoadedSamples&) = delete;
    LoadedSamples& operator= (const LoadedSamples&) = delete;

    const SampleSnapshot& snapshot() const noexcept;
    const std::vector<std::string>& errors() const noexcept; // per slot: "scene S track T: message"

    // Resolves the project's pool against `roots` (PRJ-040: a missing file is reported, its track stays silent).
    static core::Result<std::unique_ptr<LoadedSamples>> load(core::model::Project&, const services::store::SampleRoots&,
                                                             double sampleRate, double timeoutSeconds = 120.0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace services::render
