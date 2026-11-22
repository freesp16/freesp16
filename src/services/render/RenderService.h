// Offline render (spec 02-features/render-and-processing.md REN-001…005).
//
//   message thread                                     render worker (one job at a time)
//   start(project, job, progress, done):
//     deep copy of the project (≈ 2 MB, samples ────▶  OfflineEngine: the project's values + samples, settle,
//     shared by reference: SampleSnapshot), live        Play, 64-frame blocks as fast as possible (render()),
//     output muted (render.muteLive, a 64-sample        tail, WAV/AIFF via AudioFileWriter, normalize (REN-010),
//     fade in the live OutputRouter, REN-005)           sidecar (bars, bpm; REN-003)
//   pump(): progress(fraction), then done(result) ◀──  completion
//     the live output restored on done or cancel; REN-004 render to track through Env::assignToTrack (the
//     composition root's undoable TrackLoader::load)
//
// Sources (REN-001; tempo = the scene's BPM, else the project tempo):
//   Pattern      scene/pattern, its length × repeats (render.bars > 0: that many bars instead)
//   Track        the same with only `track` audible: includeBusFx → the track soloed on Main (its send and the
//                master FX included); otherwise the track alone on OUT 3-4 of 8CH mode (post insert FX, post fader,
//                no send/master FX — the router's direct-output feed, FX-012)
//   Scene        the scene's patterns that have content (a pattern unlike a default one), in order, `repeats` times,
//                as a pattern chain (SCN-030 ChainPlan); none → the job's pattern
//   Arrangement  the song in song mode (stop at the end), until the transport stops (block-exact)
// Tails (REN-002): REMAINDER CUT = the nominal length; LEAVE (`tails`) = the transport pauses at the block that
// contains the end and the output runs on until the peak of the last 50 ms is below −90 dBFS or 10 s passed,
// trimmed after the last sample ≥ −90 dBFS; WRAP (`wrapTail`, ours) = that tail added onto the start, length
// nominal. Files: `<project>_<scene>_<pattern>_<n>.wav` (1-based scene / pattern; n = the first free number from 1;
// Arrangement: `<project>_ARR_<n>.wav`) in Env::outputDir (RenderedAudio/), sidecar `bars` = rendered
// sixteenths / 16, `bpm` = 240 · bars / nominal seconds.
//
// render() is the synchronous core (the worker, the render CLI, tests and the benches call it). Deterministic: the
// same project, samples and job give a bit-identical output (RenderResult::hash, FNV-1a 64 of the float output).
#pragma once

#include "core/model/Project.h"
#include "core/util/Result.h"
#include "services/library/Workers.h"
#include "services/normalize/Normalizer.h"
#include "services/render/OfflineEngine.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace services::render
{
enum class Source : std::uint8_t { Track, Pattern, Scene, Arrangement };

struct RenderJob
{
    Source source = Source::Pattern;
    int scene = 0, pattern = 0, track = 0, repeats = 1; // 0-based scene / pattern / track
    bool includeBusFx = true, tails = false;
    int bitDepth = 24; // 16 / 24 / 32 (= 32-bit float)
    bool normalize = false;
    bool toTrack = false;
    int targetTrack = -1; // REN-004: -1 = the first empty track of the scene (none: the source track for Track)
    // Our additions to the spec's request:
    double bars = 0;              // > 0: the length in bars instead of the source's own
    bool wrapTail = false;        // REMAINDER WRAP
    double normalizeDbfs = -1.0;  // REN-011 target
};

// The job from the render.* preferences (core/params/catalogue/render.cpp): value(id) returns a preference's value.
RenderJob jobFromPreferences(const std::function<float(std::string_view id)>& value, int scene, int pattern,
                             int track);

struct Rendered // render()'s output, before the file
{
    std::vector<float> left, right;
    double sampleRate = 44100;
    std::int64_t nominalFrames = 0; // the length without the tail
    double sixteenths = 0;          // the rendered musical length (bars = sixteenths / 16)
    double bpm = 0;                 // REN-021 from bars and the nominal length
    double tempo = 120;             // the tempo the render started with
    int preroll = 0;
    std::uint64_t hash = 0;         // FNV-1a 64 over the float blocks (L then R per block), the nominal + tail output
    bool cancelled = false;
};

struct RenderResult
{
    juce::File file;
    std::int64_t frames = 0;
    double seconds = 0, bars = 0, bpm = 0;
    std::uint64_t hash = 0;
    std::optional<services::normalize::NormalizeReport> normalized;
    int assignedScene = -1, assignedTrack = -1; // REN-004: where Env::assignToTrack put it
    bool cancelled = false;
};

using JobHandle = std::uint32_t;
using ProgressFn = std::function<void(float fraction)>;
using DoneFn = std::function<void(core::Result<RenderResult>)>;

// The synchronous render. `cancel` / `progress` (may be null) are polled / written once per block.
Rendered render(const core::model::Project&, const RenderJob&, const SampleSnapshot&, const ProjectFeeder&,
                double sampleRate, const std::atomic<bool>* cancel = nullptr, std::atomic<float>* progress = nullptr);

// Writes `r` (REN-002 bit depth) to `file`, then normalizes it in place when job.normalize (REN-010), then the
// sidecar (REN-003). Worker thread.
core::Result<RenderResult> writeRendered(const Rendered&, const RenderJob&, const juce::File& file);

// REN-003: `<project>_<scene>_<pattern>_<n>.wav` in `dir`, n = the first free number from 1.
juce::File renderFileFor(const juce::File& dir, const std::string& project, const RenderJob&);
// REN-004: the first track of `scene` without a sample, -1 = none.
int firstEmptyTrack(const core::model::Project&, int scene) noexcept;

class RenderService
{
public:
    struct Env
    {
        ProjectFeeder feed;                                  // the composition root's project forwarding
        std::function<SampleSnapshot()> samples;             // message thread: the document's buffers now
        engine::EngineBridge* live = nullptr;                // muted during a job (render.muteLive); null = none
        double sampleRate = 44100;
        juce::File outputDir;                                // RenderedAudio/
        std::function<std::string()> projectName;            // for the file name
        std::function<void(int scene, int track, const juce::File&)> assignToTrack; // REN-004 (undoable load)
    };

    explicit RenderService(Env);
    ~RenderService(); // cancels a running job and waits for it; its done is not called; posts nothing
    RenderService(const RenderService&) = delete;
    RenderService& operator= (const RenderService&) = delete;

    // Message thread. One job at a time: a start while busy fails at once (done with an error, handle 0).
    JobHandle start(const core::model::Project&, const RenderJob&, ProgressFn, DoneFn);
    void cancel(JobHandle); // the job stops at its next block; done gets a result with cancelled = true; live restored
    // Message thread, before the live engine goes (shutdown): cancels the running job without calling its done, and
    // restores the live output at once.
    void stop();
    // Message thread, once per UI frame: progress of the running job, then the completion.
    void pump();
    bool busy() const noexcept { return job_ != nullptr; }
    float progress() const noexcept;

private:
    struct Job;
    void setLiveMute(bool on);

    Env env_;
    services::library::Workers worker_ {1};
    std::shared_ptr<Job> job_;
    JobHandle next_ = 1;
};
} // namespace services::render
