#include "app/M5Hosts.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/sampling/SamplingCommands.h"
#include "services/analysis/Analysis.h"
#include "services/library/SidecarStore.h"
#include "services/sampling/WavWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

namespace app
{
namespace
{
namespace rnd = services::render;
namespace lib = services::library;
namespace smp = core::commands::sampling;

// ---- RENDER AUDIO: the batch on the Document's RenderService ------------------------------------------------------
class AppRenderHost final : public ui::m5::RenderHost
{
public:
    AppRenderHost(M5HostEnv env, rnd::RenderService& service) : env_(std::move(env)), service_(service) {}
    ~AppRenderHost() override
    {
        *alive_ = false;
        if (handle_ != 0)
            service_.cancel(handle_);
    }

    bool start(std::vector<rnd::RenderJob> jobs, bool external) override
    {
        status_.error.clear();
        if (status_.busy || service_.busy())
            return refuse("Rendering is busy");
        if (external)
            return refuse("EXTERNAL needs a folder chooser (not available yet)");
        if (jobs.empty())
            return refuse("Choose at least one target");
        if (! core::commands::hasProject(env_.bus))
            return refuse("No project");
        queue_ = std::move(jobs);
        status_ = {};
        status_.busy = true;
        status_.count = static_cast<int>(queue_.size());
        finished_ = status_.finished;
        next();
        return true;
    }
    void cancel() override
    {
        cancelled_ = true;
        if (handle_ != 0)
            service_.cancel(handle_);
    }
    const ui::m5::RenderStatus& status() const override { return status_; }
    std::string_view destinationName(bool external) const override
    {
        return external ? std::string_view("the chosen folder") : std::string_view("Library / RenderedAudio");
    }

private:
    bool refuse(std::string why)
    {
        status_.error = std::move(why);
        return false;
    }
    void next()
    {
        const auto& job = queue_[static_cast<std::size_t>(status_.index)];
        std::weak_ptr<bool> alive = alive_;
        handle_ = service_.start(
            core::commands::project(env_.bus), job,
            [this, alive](float f)
            {
                if (alive.expired())
                    return;
                status_.fraction = (static_cast<float>(status_.index) + f) / static_cast<float>(status_.count);
            },
            [this, alive](core::Result<rnd::RenderResult> r)
            {
                if (alive.expired())
                    return;
                handle_ = 0;
                if (! r)
                {
                    status_.failed = true;
                    status_.error = r.error().message;
                    return finish();
                }
                const auto& x = r.value();
                if (x.cancelled || cancelled_)
                {
                    status_.cancelled = true;
                    return finish();
                }
                ++status_.written;
                status_.lastFile = x.file.getFullPathName().toStdString();
                status_.bars = x.bars;
                status_.bpm = x.bpm;
                log("RENDER UI file %d/%d %s", status_.written, status_.count, status_.lastFile.c_str());
                if (++status_.index < status_.count)
                    return next();
                analyse(x.file);
            });
        if (handle_ == 0 && ! status_.failed)
        {
            status_.failed = true;
            if (status_.error.empty())
                status_.error = "refused";
            finish();
        }
    }
    // REN-020: the last file's peak (the analysis overview of its buffer), then the batch ends.
    void analyse(const juce::File& file)
    {
        std::weak_ptr<bool> alive = alive_;
        env_.pool.requestFile(file, false,
                              [this, alive](core::Result<lib::SamplePool::Handle> r, const lib::LoadInfo&)
                              {
                                  if (alive.expired())
                                      return;
                                  if (r)
                                  {
                                      const auto o = services::analysis::peaks(*r.value());
                                      float peak = 0;
                                      for (std::size_t i = 0; i < o.max.size(); ++i)
                                          peak = std::max({peak, o.max[i], -o.min[i]});
                                      status_.peakDb = peak > 0 ? 20.0 * std::log10(static_cast<double>(peak)) : -120.0;
                                  }
                                  finish();
                              });
    }
    void finish()
    {
        status_.busy = false;
        status_.fraction = 1;
        status_.finished = ++finished_;
        cancelled_ = false;
        queue_.clear();
        log("RENDER UI done files=%d cancelled=%d failed=%d", status_.written, status_.cancelled ? 1 : 0,
            status_.failed ? 1 : 0);
    }
    template <typename... A> void log(const char* fmt, A... args) const
    {
        if (! env_.log)
            return;
        char buf[400];
        std::snprintf(buf, sizeof buf, fmt, args...);
        env_.log(buf);
    }

    M5HostEnv env_;
    rnd::RenderService& service_;
    std::vector<rnd::RenderJob> queue_;
    ui::m5::RenderStatus status_;
    rnd::JobHandle handle_ = 0;
    std::uint32_t finished_ = 0;
    bool cancelled_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// ---- SAMPLING/EDIT, LIVE SAMPLING: the Document's SamplingSession and the take's document side ----------------------
class AppSamplingHost final : public ui::m5::SamplingHost
{
public:
    AppSamplingHost(M5HostEnv env, services::sampling::SamplingSession& session)
        : env_(std::move(env)), session_(session)
    {
        auto previous = session_.onTake;
        std::weak_ptr<bool> alive = alive_;
        session_.onTake = [this, alive, previous](const services::sampling::TakeInfo& t)
        {
            if (previous)
                previous(t);
            if (! alive.expired() && t.ok && ! t.cancelled)
                assignTake(t);
        };
        auto previousLive = session_.onLiveTake;
        session_.onLiveTake = [this, alive, previousLive](int scene, int track, const core::audio::SampleBuffer* b)
        {
            if (previousLive)
                previousLive(scene, track, b);
            if (alive.expired())
                return;
            liveTrack_ = track;
            liveScene_ = scene;
            liveDirty_ = true;
            ++rev_;
        };
    }
    ~AppSamplingHost() override { *alive_ = false; }

    const core::sampling::CaptureHub* hub() const override { return session_.hub(); }

    bool record(int scene, int track) override
    {
        if (! capture())
            return false;
        // SOURCE (sampling.source): LINE IN L / R record mono (SMP-001).
        const int source = static_cast<int>(std::lround(env_.value ? env_.value("sampling.source") : 0.0f));
        auto r = session_.record(scene, track, source == 1 || source == 2 ? 1 : 2);
        if (! r)
        {
            log("SAMPLING REC refused: %s", r.error().message.c_str());
            return false;
        }
        log("SAMPLING REC track=%d file=%s (screen)", track + 1, r.value().getFullPathName().toRawUTF8());
        return true;
    }
    void stop(int scene, int track) override { session_.stopRecord(scene, track); }
    void drop(int scene, int track) override
    {
        session_.dropRecord(scene, track);
        log("SAMPLING drop track=%d", track + 1);
    }

    bool unsaved(int scene, int track) const override
    {
        const auto it = takes_.find(key(scene, track));
        return it != takes_.end() && ! it->second.saved && core::commands::hasProject(env_.bus) &&
               smp::isRecordedTemp(core::commands::project(env_.bus), scene, track);
    }
    std::string proposedName(int scene, int track) const override
    {
        // <name>_<bpm>bpm_<bars>bar_NN (UID-780): the replaced sample's name (else REC), the take's tempo, the
        // first free number in Recordings/.
        std::string base = "REC";
        double bars = 0, bpm = 0;
        if (const auto it = takes_.find(key(scene, track)); it != takes_.end())
        {
            if (! it->second.replacedName.empty())
                base = it->second.replacedName;
            bars = it->second.bars;
            bpm = it->second.bpm;
        }
        if (const auto cut = base.find("_"); cut != std::string::npos && base != "REC")
            base = base.substr(0, cut);
        char stem[160];
        if (bpm > 0 && bars > 0)
            std::snprintf(stem, sizeof stem, "%s_%.0fbpm_%.0fbar", base.c_str(), bpm, bars);
        else
            std::snprintf(stem, sizeof stem, "%s", base.c_str());
        for (int n = 1; n < 100; ++n)
        {
            char name[180];
            std::snprintf(name, sizeof name, "%s_%02d", stem, n);
            if (! env_.recordings.getChildFile(juce::String(name) + ".wav").exists())
                return name;
        }
        return std::string(stem) + "_99";
    }
    void saveSelection(int scene, int track, std::int64_t start, std::int64_t end, const std::string& name,
                       bool replace, ui::m5::HostDone done) override
    {
        const auto file = env_.recordings.getChildFile(juce::File::createLegalFileName(juce::String(name)) + ".wav");
        double bars = 0, bpm = 0;
        tempo(scene, track, bars, bpm);
        derive(scene, track, start, end, file, {}, bars, bpm,
               [this, scene, track, replace, file, done](bool ok, std::string message)
               {
                   if (ok)
                   {
                       auto it = takes_.find(key(scene, track));
                       if (it != takes_.end())
                           it->second.saved = true;
                       if (replace)
                           env_.loadToTrack(scene, track, file, [this, scene, track](bool loaded)
                                            {
                                                if (loaded)
                                                    takes_.erase(key(scene, track)); // SAVE & REPLACE: the file is the sample
                                                ++rev_;
                                            });
                       ++rev_;
                   }
                   if (done)
                       done(ok, ok ? file.getFullPathName().toStdString() : message);
               });
    }
    void discard(int scene, int track) override
    {
        const auto it = takes_.find(key(scene, track));
        if (it == takes_.end() || ! core::commands::hasProject(env_.bus))
            return;
        env_.bus.execute(smp::discardTake(scene, track, it->second.replaced)); // SMP-010: the old sample comes back
        takes_.erase(it);
        ++rev_;
    }
    void process(int scene, int track, ui::m5::TakeProcess kind, std::int64_t start, std::int64_t end,
                 ui::m5::HostDone done) override
    {
        static constexpr const char* kSuffix[] = {"norm", "fadein", "fadeout"};
        const auto stem = proposedName(scene, track) + "_" + kSuffix[static_cast<int>(kind)];
        const auto file = env_.recordings.getChildFile(juce::File::createLegalFileName(juce::String(stem)) + ".wav");
        double bars = 0, bpm = 0;
        tempo(scene, track, bars, bpm);
        derive(scene, track, start, end, file, kind, bars, bpm,
               [this, scene, track, file, done](bool ok, std::string message)
               {
                   if (! ok)
                   {
                       if (done)
                           done(false, message);
                       return;
                   }
                   env_.loadToTrack(scene, track, file, [this, done, file](bool loaded)
                                    {
                                        ++rev_;
                                        if (done)
                                            done(loaded, loaded ? "Applied: " + file.getFileName().toStdString()
                                                                : std::string("load failed"));
                                    });
               });
    }
    bool tempo(int scene, int track, double& bars, double& bpm) const override
    {
        if (const auto it = takes_.find(key(scene, track)); it != takes_.end())
        {
            bars = it->second.bars;
            bpm = it->second.bpm;
            return bars > 0;
        }
        if (const auto f = fileOf(scene, track))
        {
            const auto meta = lib::SidecarStore().read(*f);
            bars = meta.bars;
            bpm = meta.bpm;
            return bars > 0 || bpm > 0;
        }
        return false;
    }
    void setTempo(int scene, int track, double bars, double bpm) override
    {
        // REN-021: the take's sidecar holds both (the library and the stretcher read it; [contract] the engine's
        // sample BPM, stretchBank().setSampleTempo, has no EngineApi path yet).
        if (auto it = takes_.find(key(scene, track)); it != takes_.end())
        {
            it->second.bars = bars;
            it->second.bpm = bpm;
        }
        if (const auto f = fileOf(scene, track))
        {
            auto meta = lib::SidecarStore().read(*f);
            meta.bars = bars;
            meta.bpm = bpm;
            if (auto r = lib::SidecarStore().write(*f, meta); ! r)
                log("SAMPLING TEMPO sidecar failed: %s", r.error().message.c_str());
        }
        log("SAMPLING TEMPO track=%d bars=%.3f bpm=%.3f", track + 1, bars, bpm);
        ++rev_;
    }

    bool liveArm(int scene, int track) override
    {
        if (! capture())
            return false;
        session_.liveArm(scene, track);
        log("LIVE ARM track=%d (screen)", track + 1);
        return true;
    }
    void liveDisarm(int scene, int track) override { session_.liveDisarm(scene, track); }
    void setNudge(std::int32_t frames) override { session_.setNudge(frames); }
    bool liveUnsaved() const override { return liveTrack_ >= 0 && liveDirty_; }
    int liveScene() const override { return liveScene_; }
    int liveTrack() const override { return liveTrack_; }
    void saveLive(int, ui::m5::HostDone done) override
    {
        // SAVE TO LIBRARY = <library>/Recordings (SMP-025): "LIVE NN.wav", the first free number.
        juce::File target;
        for (int n = 1; n < 1000; ++n)
        {
            target = env_.recordings.getChildFile(juce::String::formatted("LIVE %03d.wav", n));
            if (! target.exists())
                break;
        }
        std::weak_ptr<bool> alive = alive_;
        session_.saveLive(target, [this, alive, done](core::Result<juce::File> r)
                          {
                              if (alive.expired())
                                  return;
                              if (r)
                              {
                                  liveDirty_ = false;
                                  ++rev_;
                                  log("LIVE SAVE ok file=%s (screen)", r.value().getFullPathName().toRawUTF8());
                              }
                              if (done)
                                  done(static_cast<bool>(r), r ? r.value().getFullPathName().toStdString()
                                                               : r.error().message);
                          });
    }
    void unloadLive(int scene, int track) override
    {
        if (core::commands::hasProject(env_.bus))
            env_.bus.execute(smp::unloadTrack(scene, track)); // SMP-025: undoable; the bus hook clears the engine slot
        liveTrack_ = -1;
        liveDirty_ = false;
        ++rev_;
    }
    std::uint32_t revision() const override { return rev_; }

private:
    struct Take
    {
        smp::ReplacedTrack replaced;
        juce::File file;
        std::string replacedName;
        double bars = 0, bpm = 0;
        bool saved = false;
    };
    static int key(int scene, int track) noexcept { return (scene & 15) * 16 + (track & 15); }

    bool capture()
    {
        const double bpm = env_.bridge.playhead().bpm;
        if (session_.ensureCapture(bpm > 0 ? bpm : 120.0))
            return true;
        log("%s", "SAMPLING REC refused: no memory for the capture ring");
        return false;
    }

    // SMP-010: the finished take replaces the track's sample as RECORDED TEMP. (one undo step).
    void assignTake(const services::sampling::TakeInfo& t)
    {
        std::weak_ptr<bool> alive = alive_;
        env_.pool.requestFile(t.file, true,
                              [this, alive, t](core::Result<lib::SamplePool::Handle> r, const lib::LoadInfo& info)
                              {
                                  if (alive.expired() || ! r || ! core::commands::hasProject(env_.bus))
                                      return;
                                  const auto& p = core::commands::project(env_.bus);
                                  Take take;
                                  take.replaced = smp::replacedOf(p, t.scene, t.track);
                                  take.file = t.file;
                                  if (take.replaced.ref)
                                      take.replacedName = take.replaced.ref->displayName;
                                  take.bpm = t.bpm;
                                  take.bars = t.bars > 0 ? t.bars
                                                         : services::analysis::barsFromBpm(
                                                               t.bpm, static_cast<double>(t.frames) / t.sampleRate);
                                  core::model::SampleRef ref;
                                  ref.id = info.contentId;
                                  ref.path = t.file.getFullPathName().toStdString();
                                  ref.displayName = t.file.getFileNameWithoutExtension().toStdString();
                                  core::commands::library::LoadDefaults d;
                                  d.frames = r.value()->frames;
                                  d.loop = t.bars > 0;
                                  env_.bus.execute(smp::assignTake(t.scene, t.track, smp::takeRef(ref), d));
                                  takes_[key(t.scene, t.track)] = std::move(take);
                                  ++rev_;
                                  log("SAMPLING ASSIGN scene=%d track=%d (RECORDED TEMP.)", t.scene + 1, t.track + 1);
                              });
    }

    std::optional<juce::File> fileOf(int scene, int track) const
    {
        if (const auto it = takes_.find(key(scene, track)); it != takes_.end())
            return it->second.file;
        if (! core::commands::hasProject(env_.bus) || ! env_.resolve)
            return std::nullopt;
        const auto& p = core::commands::project(env_.bus);
        const auto& tr = p.scenes[static_cast<std::size_t>(scene & 15)].tracks[static_cast<std::size_t>(track & 15)];
        const auto* ref = tr.playback.sample.empty() ? nullptr : p.sampleRefs.find(tr.playback.sample);
        return ref != nullptr ? env_.resolve(ref->path) : std::nullopt;
    }

    // Writes [start, end) of the track's buffer to `file` (24-bit WAV + sidecar) on a worker, optionally processed:
    // NORMALIZE (REN-010: peak to −1.0 dBFS), FADE IN / OUT (10 ms linear at the edge).
    void derive(int scene, int track, std::int64_t start, std::int64_t end, const juce::File& file,
                std::optional<ui::m5::TakeProcess> kind, double bars, double bpm, ui::m5::HostDone done)
    {
        const auto* b = env_.current ? env_.current(scene, track) : nullptr;
        if (b == nullptr || b->frames <= 0)
        {
            if (done)
                done(false, "no sample on the track");
            return;
        }
        start = std::clamp<std::int64_t>(start, 0, b->frames - 1);
        end = std::clamp<std::int64_t>(end, start + 1, b->frames);
        core::audio::retain(b);
        auto result = std::make_shared<std::string>();
        const double seconds = static_cast<double>(end - start) / b->sampleRate;
        if (bars <= 0 && bpm > 0)
            bars = services::analysis::barsFromBpm(bpm, seconds);
        env_.workers.run(
            [b, start, end, file, kind, bars, bpm, result]
            {
                const auto n = static_cast<std::size_t>(end - start);
                std::vector<float> l(b->ch[0] + start, b->ch[0] + end);
                std::vector<float> r(b->channels > 1 ? b->ch[1] + start : b->ch[0] + start,
                                     b->channels > 1 ? b->ch[1] + end : b->ch[0] + end);
                if (kind == ui::m5::TakeProcess::Normalize)
                {
                    float peak = 0;
                    for (std::size_t i = 0; i < n; ++i)
                        peak = std::max({peak, std::fabs(l[i]), std::fabs(r[i])});
                    if (peak > 0)
                    {
                        const float g = std::pow(10.0f, -1.0f / 20.0f) / peak;
                        for (std::size_t i = 0; i < n; ++i)
                        {
                            l[i] *= g;
                            r[i] *= g;
                        }
                    }
                }
                else if (kind)
                {
                    const auto fade = std::min<std::size_t>(n, static_cast<std::size_t>(b->sampleRate * 0.010));
                    for (std::size_t i = 0; i < fade; ++i)
                    {
                        const float g = static_cast<float>(i) / static_cast<float>(fade);
                        const std::size_t k = *kind == ui::m5::TakeProcess::FadeIn ? i : n - 1 - i;
                        l[k] *= g;
                        r[k] *= g;
                    }
                }
                services::sampling::WavWriter w;
                auto ok = w.open(file, b->sampleRate, b->channels > 1 ? 2 : 1);
                if (ok)
                    ok = w.write(l.data(), r.data(), static_cast<std::int64_t>(n));
                if (ok)
                    ok = w.finish();
                if (! ok)
                {
                    *result = ok.error().message;
                    return;
                }
                core::model::SampleMetadata meta;
                meta.bars = bars;
                meta.bpm = bpm;
                (void)lib::SidecarStore().write(file, meta);
            },
            [this, b, result, done, file, alive = std::weak_ptr<bool>(alive_)]
            {
                core::audio::release(b);
                if (alive.expired())
                    return;
                log("SAMPLING SAVE %s %s", file.getFullPathName().toRawUTF8(), result->empty() ? "ok" : result->c_str());
                if (done)
                    done(result->empty(), *result);
            });
    }

    template <typename... A> void log(const char* fmt, A... args) const
    {
        if (! env_.log)
            return;
        char buf[400];
        std::snprintf(buf, sizeof buf, fmt, args...);
        env_.log(buf);
    }

    M5HostEnv env_;
    services::sampling::SamplingSession& session_;
    std::map<int, Take> takes_;
    int liveTrack_ = -1, liveScene_ = 0;
    bool liveDirty_ = false;
    std::uint32_t rev_ = 1;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace

std::unique_ptr<ui::m5::RenderHost> makeRenderHost(M5HostEnv env, services::render::RenderService& service)
{
    return std::make_unique<AppRenderHost>(std::move(env), service);
}

std::unique_ptr<ui::m5::SamplingHost> makeSamplingHost(M5HostEnv env, services::sampling::SamplingSession& session)
{
    return std::make_unique<AppSamplingHost>(std::move(env), session);
}
} // namespace app
