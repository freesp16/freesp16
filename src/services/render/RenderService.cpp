#include "services/render/RenderService.h"

#include "core/params/ParamRegistry.h"
#include "core/util/chain/ChainPlan.h"
#include "services/analysis/Analysis.h"
#include "services/library/SidecarStore.h"
#include "services/render/AudioWriter.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace services::render
{
namespace
{
namespace model = core::model;

constexpr float kTailFloor = 3.1622776601683795e-5f; // −90 dBFS (REN-002)
constexpr double kTailMaxSeconds = 10.0;             // REN-002
constexpr double kTailQuietSeconds = 0.05;           // the tail ends after 50 ms below −90 dBFS
constexpr double kSongMaxSeconds = 3600.0;           // a song render never runs longer than an hour (a guard)

core::ParamId idOf(std::string_view id)
{
    return core::builtinParams().idOf(id).value_or(core::ParamId {});
}

bool hasContent(const model::Pattern& p) noexcept
{
    for (const auto& row : p.steps)
        for (const auto& s : row)
            if (! s.empty())
                return true;
    return false;
}

int patternLength(const model::Scene& s, int pattern) noexcept
{
    return std::clamp(s.patterns[static_cast<std::size_t>(pattern & 15)].length, 1, model::kSteps);
}

// The arrangement's length in seconds at its fixed BPMs ("--" keeps the tempo in force) and sixteenths.
std::pair<double, double> songLength(const model::Project& p, double tempo) noexcept
{
    double seconds = 0, sixteenths = 0;
    for (const auto& row : p.arrangement.sections)
    {
        if (row.fixedBpm && *row.fixedBpm >= 20.0)
            tempo = *row.fixedBpm;
        const double steps = static_cast<double>(std::max(1, row.repeats)) *
                             patternLength(p.scenes[static_cast<std::size_t>(row.scene & 15)], row.pattern);
        sixteenths += steps;
        seconds += steps * 15.0 / tempo;
    }
    return {seconds, sixteenths};
}

void hashInto(const std::vector<float>& l, const std::vector<float>& r, std::uint64_t& h) noexcept
{
    for (std::size_t at = 0; at < l.size(); at += engine::kBlockFrames)
    {
        const std::size_t n = std::min<std::size_t>(engine::kBlockFrames, l.size() - at);
        h = fnv1a(l.data() + at, n * sizeof(float), h);
        h = fnv1a(r.data() + at, n * sizeof(float), h);
    }
}
} // namespace

RenderJob jobFromPreferences(const std::function<float(std::string_view)>& value, int scene, int pattern, int track)
{
    RenderJob j;
    const auto v = [&](std::string_view id, float d) { return value ? value(id) : d; };
    j.source = static_cast<Source>(std::clamp(static_cast<int>(v("render.source", 1) + 0.5f), 0, 3));
    j.scene = scene;
    j.pattern = pattern;
    j.track = track;
    j.repeats = std::max(1, static_cast<int>(v("render.repeats", 1) + 0.5f));
    j.bars = std::max(0.0, std::floor(static_cast<double>(v("render.bars", 0)) + 0.5));
    const int remainder = static_cast<int>(v("render.remainder", 0) + 0.5f); // CUT, WRAP, LEAVE
    j.tails = remainder == 2;
    j.wrapTail = remainder == 1;
    j.includeBusFx = v("render.includeFx", 1) >= 0.5f;
    const int depth = static_cast<int>(v("render.bitDepth", 1) + 0.5f);
    j.bitDepth = depth == 0 ? 16 : (depth == 2 ? 32 : 24);
    j.normalize = v("render.normalize", 0) >= 0.5f;
    j.toTrack = v("render.destination", 0) >= 0.5f;
    j.normalizeDbfs = std::clamp(static_cast<double>(v("render.normalizeTarget", -1.0f)), -6.0, 0.0);
    return j;
}

int firstEmptyTrack(const model::Project& p, int scene) noexcept
{
    const auto& s = p.scenes[static_cast<std::size_t>(scene & 15)];
    for (int t = 0; t < model::kTracks; ++t)
        if (s.tracks[static_cast<std::size_t>(t)].playback.sample.empty())
            return t;
    return -1;
}

juce::File renderFileFor(const juce::File& dir, const std::string& project, const RenderJob& job)
{
    const juce::String base = juce::File::createLegalFileName(juce::String(project.empty() ? "Untitled" : project));
    const juce::String stem = job.source == Source::Arrangement
                                  ? base + "_ARR_"
                                  : base + "_" + juce::String((job.scene & 15) + 1) + "_" +
                                        juce::String((job.pattern & 15) + 1) + "_";
    for (int n = 1;; ++n)
    {
        const auto f = dir.getChildFile(stem + juce::String(n) + ".wav");
        if (! f.exists() && ! f.getSiblingFile(f.getFileName() + ".part").exists())
            return f;
    }
}

Rendered render(const model::Project& source, const RenderJob& job, const SampleSnapshot& samples,
                const ProjectFeeder& feed, double rate, const std::atomic<bool>* cancel, std::atomic<float>* progress)
{
    Rendered out;
    out.sampleRate = rate;
    const int sc = job.scene & 15, pat = job.pattern & 15, track = job.track & 15;
    const int repeats = std::max(1, job.repeats);
    const bool song = job.source == Source::Arrangement;
    const bool direct = job.source == Source::Track && ! job.includeBusFx; // the track alone on OUT 3-4 (8CH)

    // The job's own project (heap: a Project never lives on a stack), adapted to the source.
    auto p = std::make_unique<model::Project>(source);
    auto& scene = p->scenes[static_cast<std::size_t>(sc)];
    if (job.source == Source::Track)
    {
        for (int t = 0; t < model::kTracks; ++t)
        {
            auto& x = scene.tracks[static_cast<std::size_t>(t)];
            if (direct)
                x.routing.output = t == track ? model::OutputAssign::Out34 : model::OutputAssign::Master;
            else
                x.solo = t == track;
        }
        if (direct)
            scene.routing.assignMode = model::AssignMode::EightChannel;
    }
    const double tempo = scene.bpm && *scene.bpm >= 20.0 ? *scene.bpm : p->settings.tempo;
    out.tempo = tempo;

    // The musical length (sixteenths) and the chain of a Scene render.
    core::chain::ChainPlan chain;
    double sixteenths = 0;
    if (job.source == Source::Scene)
    {
        std::vector<int> list;
        for (int q = 0; q < model::kPatterns; ++q)
            if (hasContent(scene.patterns[static_cast<std::size_t>(q)]))
                list.push_back(q);
        if (list.empty())
            list.push_back(pat);
        chain.scene = static_cast<std::uint8_t>(sc);
        for (int r = 0; r < repeats; ++r)
            for (const int q : list)
                if (chain.length < core::chain::kMaxChain)
                {
                    chain.patterns[chain.length++] = static_cast<std::uint8_t>(q);
                    sixteenths += patternLength(scene, q);
                }
    }
    else if (! song)
        sixteenths = static_cast<double>(patternLength(scene, pat)) * repeats;
    double songSeconds = 0;
    if (song)
        std::tie(songSeconds, sixteenths) = songLength(*p, tempo);
    if (job.bars > 0)
        sixteenths = job.bars * 16.0;
    const bool fixedLength = ! song || job.bars > 0;
    std::int64_t nominal = fixedLength ? std::llround(sixteenths * 15.0 / tempo * rate) : 0;
    const double expected = fixedLength ? static_cast<double>(nominal) : songSeconds * rate;

    // The engine, the project, the set-up.
    OfflineEngine e({rate, direct ? 2 : 1});
    core::arrangement::SongStatus status;
    e.begin(*p, samples, feed, song ? &status : nullptr);
    auto& q = e.bridge();
    q.post(engine::ParamChange {idOf("arrangement.loop"), 0, 0.0f, 0});
    q.post(engine::ParamChange {idOf("arrangement.play"), 0, song ? 1.0f : 0.0f, 0});
    if (! song && job.source != Source::Scene) // stopped: the switch is immediate
        q.post(engine::ParamChange {idOf("scene.queue"), 0xFFFF, static_cast<float>(sc * 16 + pat), 0});
    if (job.source == Source::Scene) // the chain's first pattern, then the chain (it plays while the scene is current)
    {
        q.post(engine::ParamChange {idOf("scene.queue"), 0xFFFF, static_cast<float>(sc * 16 + chain.patterns[0]), 0});
        auto* object = new core::chain::ChainPlan(chain);
        if (! q.post(engine::ObjectSwap {{engine::SwapSlot::ProjectConfig, core::chain::kSwapScene,
                                          core::chain::kSwapIndex},
                                         object}))
            core::chain::destroyChainPlan(object);
    }
    q.post(engine::TransportCmd {engine::TransportCmd::SetTempo, tempo});
    out.preroll = e.settle();
    q.post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});

    const int port = direct ? 1 : 0;
    const bool tails = job.tails || job.wrapTail;
    const auto tailMax = static_cast<std::int64_t>(kTailMaxSeconds * rate);
    const auto quiet = static_cast<std::int64_t>(kTailQuietSeconds * rate);
    const std::int64_t songCap = static_cast<std::int64_t>(std::min(kSongMaxSeconds, songSeconds * 1.5 + 10.0) * rate);
    out.left.reserve(static_cast<std::size_t>(expected + (tails ? tailMax : 0) + engine::kBlockFrames));
    out.right.reserve(out.left.capacity());
    std::int64_t done = 0, lastLoud = -1;
    bool played = false, stopped = false, paused = false;
    engine::EngineEvent ev;
    for (;;)
    {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
        {
            out.cancelled = true;
            return out;
        }
        // Stop conditions, checked at the block start.
        if (fixedLength && ! tails && done >= nominal)
            break;
        if (! fixedLength && stopped && ! tails)
            break;
        if (! fixedLength && ! stopped && done >= songCap)
        {
            nominal = done; // a song that never stops (a guard; logged by the caller through the length)
            break;
        }
        if (tails && (fixedLength ? done >= nominal : stopped))
            if (done - nominal >= tailMax || (done - std::max(lastLoud, nominal - 1) > quiet))
                break;
        // LEAVE / WRAP: pause in the block that contains the end (no trigger of the next pass reaches the tail).
        if (fixedLength && tails && ! paused && done + engine::kBlockFrames > nominal)
        {
            q.post(engine::TransportCmd {engine::TransportCmd::Stop, 0.0}); // Stop 0 = pause: voices ring on
            paused = true;
        }
        e.process();
        const float* l = e.out(port, 0);
        const float* r = e.out(port, 1);
        for (int i = 0; i < engine::kBlockFrames; ++i)
        {
            out.left.push_back(l[i]);
            out.right.push_back(r[i]);
            if (std::max(std::fabs(l[i]), std::fabs(r[i])) >= kTailFloor)
                lastLoud = done + i;
        }
        done += engine::kBlockFrames;
        while (e.popEvent(ev))
        {
        }
        if (song)
        {
            const bool playing = e.playhead().playing;
            played = played || playing;
            if (played && ! playing && ! stopped)
            {
                stopped = true;
                if (! fixedLength)
                    nominal = done - engine::kBlockFrames; // the first block the transport did not play
            }
        }
        if (progress != nullptr && expected > 0)
            progress->store(static_cast<float>(std::min(1.0, static_cast<double>(done) / expected)),
                            std::memory_order_relaxed);
    }

    // The length: nominal (CUT), nominal + the tail up to the last sample ≥ −90 dBFS (LEAVE), or the tail wrapped.
    auto length = static_cast<std::size_t>(std::max<std::int64_t>(0, nominal));
    if (tails)
    {
        const auto end = static_cast<std::size_t>(std::max<std::int64_t>(nominal, lastLoud + 1));
        const auto have = std::min(end, out.left.size());
        if (job.wrapTail && length > 0)
        {
            for (std::size_t i = length; i < have; ++i)
            {
                out.left[(i - length) % length] += out.left[i];
                out.right[(i - length) % length] += out.right[i];
            }
        }
        else
            length = have;
    }
    length = std::min(length, out.left.size());
    out.left.resize(length);
    out.right.resize(length);
    out.nominalFrames = std::min<std::int64_t>(nominal, static_cast<std::int64_t>(length));
    out.sixteenths = sixteenths;
    const double seconds = static_cast<double>(std::max<std::int64_t>(nominal, 1)) / rate;
    out.bpm = services::analysis::bpmFromBars(sixteenths / 16.0, seconds);
    std::uint64_t h = 1469598103934665603ull;
    hashInto(out.left, out.right, h);
    out.hash = h;
    if (progress != nullptr)
        progress->store(1.0f, std::memory_order_relaxed);
    return out;
}

core::Result<RenderResult> writeRendered(const Rendered& r, const RenderJob& job, const juce::File& file)
{
    RenderResult res;
    res.file = file;
    res.frames = static_cast<std::int64_t>(r.left.size());
    res.seconds = static_cast<double>(res.frames) / r.sampleRate;
    res.bars = r.sixteenths / 16.0;
    res.bpm = r.bpm;
    res.hash = r.hash;
    {
        auto w = AudioFileWriter::create(file, {formatForBitDepth(job.bitDepth), 2, r.sampleRate, isAiffName(file)});
        if (! w)
            return w.error();
        constexpr std::size_t kChunk = 4096;
        for (std::size_t at = 0; at < r.left.size(); at += kChunk)
        {
            const float* ch[2] = {r.left.data() + at, r.right.data() + at};
            if (! w.value()->writePlanar(ch, static_cast<int>(std::min(kChunk, r.left.size() - at))))
                return core::Error {"Cannot write the file", file.getFullPathName().toStdString()};
        }
        if (auto f = w.value()->finish(); ! f)
            return f.error();
    }
    if (job.normalize) // REN-002 → REN-010: the written file, in place
    {
        const juce::File tmp = file.getSiblingFile(file.getFileNameWithoutExtension() + ".normalizing" +
                                                   file.getFileExtension());
        auto n = services::normalize::normalize(file, tmp, job.normalizeDbfs);
        if (! n)
            return n.error();
        if (n.value().changed && ! (file.deleteFile() && tmp.moveFileTo(file)))
            return core::Error {"Cannot write the file", file.getFullPathName().toStdString()};
        res.normalized = n.value();
    }
    core::model::SampleMetadata meta; // REN-003 / REN-021
    meta.bars = res.bars;
    meta.bpm = res.bpm;
    if (auto s = services::library::SidecarStore().write(file, meta); ! s)
        return s.error();
    return res;
}

// ---------------------------------------------------------------------------------------------- RenderService

struct RenderService::Job
{
    JobHandle handle = 0;
    std::unique_ptr<model::Project> project;
    SampleSnapshot samples;
    RenderJob spec;
    juce::File file;
    int assignScene = -1, assignTrack = -1;
    std::atomic<bool> cancel {false};
    std::atomic<float> fraction {0.0f};
    bool abandoned = false; // message thread: stop() / the destructor — the completion does nothing
    std::optional<core::Result<RenderResult>> result;
    ProgressFn progress;
    DoneFn done;
};

RenderService::RenderService(Env env) : env_(std::move(env)) {}

RenderService::~RenderService()
{
    // No post here: the live engine may be gone already (the composition root calls stop() before it destroys it).
    if (job_)
    {
        job_->abandoned = true;
        job_->cancel.store(true);
    }
    // worker_ (declared after env_) joins its running job; its pending completion is dropped.
}

void RenderService::stop()
{
    if (! job_)
        return;
    job_->abandoned = true;
    job_->cancel.store(true);
    job_.reset();
    setLiveMute(false);
}

void RenderService::setLiveMute(bool on)
{
    if (env_.live != nullptr)
        env_.live->post(engine::ParamChange {idOf("render.muteLive"), 0, on ? 1.0f : 0.0f, 0});
}

float RenderService::progress() const noexcept
{
    return job_ ? job_->fraction.load(std::memory_order_relaxed) : 0.0f;
}

JobHandle RenderService::start(const model::Project& p, const RenderJob& spec, ProgressFn progress, DoneFn done)
{
    if (job_)
    {
        if (done)
            done(core::Error {"A render is already running", "busy"});
        return 0;
    }
    auto job = std::make_shared<Job>();
    job->handle = next_++;
    job->project = std::make_unique<model::Project>(p); // REN-001: the copy (message thread)
    if (env_.samples)
        job->samples = env_.samples();
    job->spec = spec;
    job->spec.repeats = std::max(1, spec.repeats);
    job->progress = std::move(progress);
    job->done = std::move(done);
    job->file = renderFileFor(env_.outputDir, env_.projectName ? env_.projectName() : std::string {}, spec);
    if (spec.toTrack) // REN-004: an empty track, else (Track) the source track
    {
        job->assignScene = spec.scene & 15;
        job->assignTrack = spec.targetTrack >= 0 ? (spec.targetTrack & 15) : firstEmptyTrack(p, spec.scene);
        if (job->assignTrack < 0 && spec.source == Source::Track)
            job->assignTrack = spec.track & 15;
        if (job->assignTrack < 0)
        {
            if (job->done)
                job->done(core::Error {"No empty track for the render", "render to track"});
            return 0;
        }
    }
    job_ = job;
    setLiveMute(true); // REN-005
    const auto feed = env_.feed;
    const double rate = env_.sampleRate;
    worker_.run(
        [job, feed, rate]
        {
            auto r = render(*job->project, job->spec, job->samples, feed, rate, &job->cancel, &job->fraction);
            job->samples = {}; // the references go as soon as the audio is rendered
            if (r.cancelled)
            {
                RenderResult c;
                c.cancelled = true;
                job->result = c;
                return;
            }
            job->result = writeRendered(r, job->spec, job->file);
        },
        [this, job]
        {
            if (job->abandoned)
                return;
            if (job_ == job)
                job_.reset();
            setLiveMute(false); // REN-005: restored on done or cancel
            auto result = job->result ? std::move(*job->result) : core::Result<RenderResult>(core::Error {"Render failed"});
            if (result && ! result.value().cancelled && job->assignTrack >= 0)
            {
                result.value().assignedScene = job->assignScene;
                result.value().assignedTrack = job->assignTrack;
                if (env_.assignToTrack)
                    env_.assignToTrack(job->assignScene, job->assignTrack, result.value().file);
            }
            if (job->progress)
                job->progress(1.0f);
            if (job->done)
                job->done(std::move(result));
        });
    return job->handle;
}

void RenderService::cancel(JobHandle h)
{
    if (job_ && job_->handle == h)
        job_->cancel.store(true);
}

void RenderService::pump()
{
    if (job_ && job_->progress)
        job_->progress(job_->fraction.load(std::memory_order_relaxed));
    worker_.pump();
}
} // namespace services::render
