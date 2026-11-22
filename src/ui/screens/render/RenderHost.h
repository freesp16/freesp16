// What RENDER AUDIO needs of the offline render (services::render::RenderService; REN-001…005): run a batch of jobs one
// after the other (one file per target), report the batch's progress and result, cancel it. The composition root
// installs the product host (app/M5Hosts.cpp: the Document's RenderService, the live output muted while a job runs, TO
// TRACK through the undoable TrackLoader); tests install a fake. With none installed RENDER reports that rendering is
// not available. Message thread only; status() is read every frame and never allocates.
#pragma once

#include "services/render/RenderService.h"

#include <string>
#include <vector>

namespace ui::m5
{
struct RenderStatus
{
    bool busy = false;
    int index = 0, count = 0;   // the running job (0-based) of the batch
    float fraction = 0;         // of the whole batch, 0–1
    int written = 0;            // files finished in the last batch
    bool cancelled = false;     // the last batch was cancelled
    bool failed = false;        // the last batch stopped on an error (error says why)
    std::uint32_t finished = 0; // +1 when a batch ends (any way)
    std::string error, lastFile;
    double peakDb = -120;       // REN-020 analysis of the last file: its peak (after NORMALIZE)
    double bars = 0, bpm = 0;   // REN-021: the last file's length in bars and its tempo
};

class RenderHost
{
public:
    virtual ~RenderHost() = default;
    // false = refused at once (busy, no project, EXTERNAL without a location); status().error says why.
    virtual bool start(std::vector<services::render::RenderJob> jobs, bool external) = 0;
    virtual void cancel() = 0;
    virtual const RenderStatus& status() const = 0;
    // Where the files go, for the summary ("Library / RenderedAudio", a volume name).
    virtual std::string_view destinationName(bool external) const = 0;

    static RenderHost* current() noexcept;
    static void install(RenderHost*) noexcept;
};
} // namespace ui::m5
