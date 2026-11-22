#include "app/Document.h"

#include "ProjectIdentity.h"

#include "app/LibraryAdapters.h"
#include "app/M5Hosts.h"
#include "app/ProjectFeed.h"
#include "app/QueuedBridge.h"
#include "app/ext/Extensions.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/arrangement/ArrangementCommands.h"
#include "core/commands/arrangement/ArrangementPlanBuilder.h"
#include "core/commands/ribbon/RibbonCommands.h"
#include "core/commands/scene/SceneCommands.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/commands/slice/SliceCommands.h"
#include "core/edit/PatternChainBuilder.h"
#include "core/edit/SharedEditor.h"
#include "core/edit/WorkerPool.h"
#include "core/params/catalogue/fx.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "io/controllers/profiles/ControllerHub.h"
#include "io/controllers/StripSource.h"
#include "io/midi/MidiSettings.h"
#include "services/interop/sp16/Sp16Mapping.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "services/library/PreviewService.h"
#include "services/library/SampleLibrary.h"
#include "services/library/SamplePool.h"
#include "services/library/SidecarStore.h"
#include "services/library/TrackLoader.h"
#include "services/library/TrackSampleSync.h"
#include "services/library/Workers.h"
#include "services/analysis/Analysis.h"
#include "services/normalize/Normalizer.h"
#include "services/render/RenderService.h"
#include "services/sampling/SamplingSession.h"
#include "services/store/ProjectStoreSession.h"
#include "services/settings/Preferences.h"
#include "services/settings/ProjectSettings.h"
#include "io/controllers/PanelPads.h"
#include "ui/input/pads/PadController.h"
#include "ui/input/pads/PadLamps.h"
#include "ui/input/panel/PanelKeys.h"
#include "ui/input/scenes/ScenePatternKeys.h"
#include "ui/input/scenes/SceneSync.h"
#include "ui/input/steps/PatternSync.h"
#include "ui/input/steps/StepController.h"
#include "ui/screens/effects/ModelFxSlots.h"
#include "ui/screens/project/StoreProjectSource.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/screens/setslice/SliceSync.h"
#include "ui/screens/touchstrip/StripLive.h"
#include "ui/screens/utility/PrefAccess.h"
#include "ui/screens/utility/UtilityEnv.h"
#include "ui/shell/ModelParamAccess.h"
#include "ui/shell/Root.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <thread>
#if defined(__linux__)
#include <malloc.h> // malloc_trim
#endif

namespace app
{
namespace
{
namespace lib = services::library;
namespace store = services::store;

// TRK-041: the pad controller's one selected track is the shell's (UiState::selectedTrack, 1–16).
class ShellSelection final : public ui::input::pads::Selection
{
public:
    ui::ShellApi* shell = nullptr;
    int selected() const override { return shell != nullptr ? (shell->state().selectedTrack - 1) & 15 : 0; }
    void select(int track) override
    {
        if (shell != nullptr)
            shell->state().selectedTrack = (track & 15) + 1;
    }
};

// The step controller paints its step lamps into a LampSink; this one writes them into the frame the
// shell's LampModel computed, before io::apply (levels: 0 off, ≥ 200 on, else dim — the PARAM level 40 shows
// dim).
class FrameLampSink final : public io::LampSink
{
public:
    explicit FrameLampSink(io::LampFrame& f) : frame_(f) {}
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override
    {
        frame_[c] = {level == 0     ? io::LampMode::Off
                     : level >= 200 ? io::LampMode::On
                     : level > io::kFaintLevel ? io::LampMode::Dim
                                               : io::LampMode::Faint,
                     rgb, 0};
    }
    void flush() override {}

private:
    io::LampFrame& frame_;
};

// SEQ-023: the shell's ParamAccess. Every edit the shell makes (knob strip turns, drags, popups)
// passes through; one of a lockable track parameter of the selected track while REC is armed and the transport runs
// also records a lock on the playing step, in the pass's undo group (StepController::recordEncoder decides).
class RecordingAccess final : public ui::ParamAccess
{
public:
    RecordingAccess(ui::ParamAccess& inner, ui::input::steps::StepController& steps, engine::EngineBridge& bridge,
                    const ShellSelection& selection)
        : inner_(inner), steps_(steps), bridge_(bridge), selection_(selection)
    {
    }
    float get(const core::ParamSpec& s, std::uint16_t scope) const override { return inner_.get(s, scope); }
    void set(const core::ParamSpec& s, std::uint16_t scope, float v) override
    {
        inner_.set(s, scope, v);
        if (! s.lockable || s.scope != core::ParamScope::Track || selection_.shell == nullptr)
            return;
        const auto& st = selection_.shell->state();
        if (scope != st.trackScope() || st.stepHeld > 0)
            return;
        steps_.recordEncoder(s, s.clamp(v), bridge_.playhead().step);
    }
    bool lockOf(const core::ParamSpec& s, std::uint16_t scope, int step, float& v) const override
    {
        return inner_.lockOf(s, scope, step, v);
    }
    void setLock(const core::ParamSpec& s, std::uint16_t scope, int step, float v) override
    {
        inner_.setLock(s, scope, step, v);
    }

private:
    ui::ParamAccess& inner_;
    ui::input::steps::StepController& steps_;
    engine::EngineBridge& bridge_;
    const ShellSelection& selection_;
};

// The PROJECT screens save through the installed ProjectSource; this decorator logs every save's result
// ("SAVED <name> ok|FAILED <error>"), so a save made on the panel (PROJECT › SAVE) leaves the same trace as `cmd save-as`.
class LoggingProjectSource final : public ui::m2::ProjectSource
{
public:
    LoggingProjectSource(ui::m2::ProjectSource& inner, std::function<void(const std::string&)> log)
        : inner_(inner), log_(std::move(log))
    {
    }
    std::vector<ui::m2::ProjectEntry> list(ui::m2::ProjectSort s) override { return inner_.list(s); }
    const std::string& currentName() const override { return inner_.currentName(); }
    bool dirty() const override { return inner_.dirty(); }
    bool hasFile() const override { return inner_.hasFile(); }
    bool exists(std::string_view name) override { return inner_.exists(name); }
    void create(const std::string& name, std::function<void(bool, std::string)> done) override
    {
        inner_.create(name, std::move(done));
    }
    void open(const ui::m2::ProjectEntry& e, std::function<void(float)> progress,
              std::function<void(bool, std::string, std::vector<std::string>)> done) override
    {
        inner_.open(e, std::move(progress), std::move(done));
    }
    void save(ui::m2::SaveTarget t, const std::string& name, std::function<void(float)> progress,
              std::function<void(bool, std::string)> done) override
    {
        inner_.save(t, name, std::move(progress),
                    [log = log_, name, done = std::move(done)](bool ok, std::string error)
                    {
                        if (log)
                            log("SAVED " + name + (ok ? " ok " : " FAILED ") + error);
                        if (done)
                            done(ok, std::move(error));
                    });
    }
    bool remove(const ui::m2::ProjectEntry& e, std::string& error) override { return inner_.remove(e, error); }
    bool rename(const ui::m2::ProjectEntry& e, const std::string& name, std::string& error) override
    {
        return inner_.rename(e, name, error);
    }
    bool duplicate(const ui::m2::ProjectEntry& e, std::string& error) override { return inner_.duplicate(e, error); }
    void cancel() override { inner_.cancel(); }
    void poll() override { inner_.poll(); }

private:
    ui::m2::ProjectSource& inner_;
    std::function<void(const std::string&)> log_;
};

// UTILITY › AUTOSAVE (settings.autosave: OFF / 1 min / 5 min, PRJ-030). 1 min is PRJ-030 itself (60 s while
// stopped, 5 min while playing); 5 min autosaves every 5 min, stopped or playing. OFF disables autosave (elsewhere).
double autosaveIdleMs(float choice) noexcept
{
    return choice >= 1.5f ? store::Autosave::kPlayingIntervalMs : store::Autosave::kIdleIntervalMs;
}
} // namespace

struct Document::Impl
{
    Impl(engine::EngineBridge& b, DocumentConfig c)
        : queued(b), bridge(queued), config(std::move(c)), pool(workers, [this](const std::string& p) { return resolve(p); },
                                                 lib::SamplePool::Config{.engineRate = config.sampleRate}),
          sync(pool, bridge), preview(pool, workers, bridge), loader(library, pool, bus),
          browse(library, loader, preview, bus), views(pool, sync, bus), fxSlots(bus, &bridge), fxMeters(bridge),
          values(bus, &bridge), pads(bus, bridge, selection), sceneSync(core::builtinParams(), bridge), sceneKeys(bus),
          steps(bus, &bridge, &values), lockValues(values, steps), recording(lockValues, steps, bridge, selection),
          strip(bridge, core::builtinParams())
    {
        strip.log = [this](const std::string& line) { log("%s", line.c_str()); };
        const juce::File dataDir(config.dataDir);
        loadPreferences(dataDir); // before the session reads RESUME / AUTOSAVE
        if (auto r = library.setRoot(dataDir); ! r)
            log("LIBRARY root %s: %s", config.dataDir.c_str(), r.error().message.c_str());
        // On the device the internal disk's stock Samples tree and every stick are read-only locations;
        // the data directory's own mount (the launcher's bind mount) is not listed.
        library.setLocationProvider(config.device ? std::shared_ptr<lib::LocationProvider>(
                                                        std::make_shared<lib::ProcMountsProvider>(
                                                            juce::File("/proc/mounts"), config.internalDisk,
                                                            std::vector<std::string> {config.dataDir}))
                                                  : std::make_shared<lib::HostVolumesProvider>());
        library.setInUse([this](const juce::File& f) { return pool.uses(f); });
        // The offline render service (its own Engine on its own worker; REN-001…005). The live engine is
        // muted while a job runs; render to track loads the file with the undoable LOAD (TrackLoader, REN-004).
        renderer.emplace(services::render::RenderService::Env {
            .feed = feed::feeder(true),
            .samples = [this] { return services::render::SampleSnapshot::of(sync); },
            .live = &bridge,
            .sampleRate = config.sampleRate,
            .outputDir = dataDir.getChildFile("RenderedAudio"),
            .projectName = [this] { return session ? session->store().name().toStdString() : std::string {}; },
            .assignToTrack = [this](int scene, int track, const juce::File& file)
            {
                loader.load(scene, track, file, [this, scene, track](core::Result<void> r, const lib::LoadInfo&)
                            { log("RENDER JOB assigned scene=%d track=%d %s", scene + 1, track + 1,
                                  r ? "ok" : r.error().message.c_str()); });
            }});
        pool.onReplaced = [this](lib::SamplePool::Handle from, lib::SamplePool::Handle to) { sync.replace(from, to); };
        sync.onError = [this](int s, int t, const core::Error& e)
        { log("SAMPLE scene %d track %d: %s", s + 1, t + 1, e.message.c_str()); };

        // Every edit reaches the engine (EDT-U4): the track fields, the table parameters, the FX slots, the samples.
        core::commands::setBridge(bus,
                                  [this](const core::ChangeSet& changes, const core::model::Project& p)
                                  {
                                      ui::input::pads::forwardTrackChanges(changes, p, bridge);
                                      ui::forwardModelChanges(changes, p, bridge);
                                      ui::fx::forwardFxChanges(changes, p, bridge);
                                      ui::input::steps::forwardPatternChanges(changes, p, bridge);
                                      sceneSync.onChange(changes, p);
                                      strip.forwardChanges(changes, p);
                                      forwardArrangement(changes, p, bridge);
                                      sync.onChange(changes, p);
                                      slices.onChange(changes, p);
                                      services::settings::forwardSettingsChanges(changes, p, bridge);
                                      if (extensions)
                                          extensions->onChange(changes, p); // nothing while ext.* is off
                                  });
        sync.onInstalled = [this](int s, int t, lib::TrackSampleSync::Handle sample) // re-slice it
        {
            if (core::commands::hasProject(bus))
                slices.onInstalled(s, t, sample != nullptr ? sample->frames : 0, core::commands::project(bus));
        };
        ui::slice::sliceLive().buffer = [this](int s, int t) { return sync.current(s, t); }; // SLC-006
        pads.slicePad = [this](int track, int pad) // SLICE pad lamps (which pads hold a slice)
        {
            if (! core::commands::hasProject(bus))
                return pad;
            const auto& p = core::commands::project(bus);
            const int s = p.settings.currentScene & 15;
            return ui::slice::padSlice(p, s, track, pad, framesOf(sync, s, track));
        };

        const auto& params = core::builtinParams();
        auto pref = [&](std::string_view id) {
            const auto* spec = params.find(id);
            return spec == nullptr || values.get(*spec, 0) >= 0.5f; // the preferences, else the defaults
        };
        session.emplace(
            bus, workers, &bridge, &pool,
            store::SessionConfig{.dataDir = dataDir,
                                 .sampleRoots = [this] { return library.roots(); },
                                 .externalFolders =
                                     [this]
                                 {
                                     std::vector<juce::File> out;
                                     for (const auto& l : library.locations())
                                         if (l.removable)
                                             out.push_back(l.root);
                                     return out;
                                 },
                                 // The reference projects OPEN lists read-only and imports (hardware feedback):
                                 // the internal disk's and every stick's PIONEER DJ SAMPLER/Projects (or a stick's
                                 // TORAIZ/Projects). The same disks are the volumes the import resolves sample URLs
                                 // against ("$Int0" the internal disk, "$Ext0" each stick), published here for its
                                 // worker; a volume the library knows keeps "$Ext:" paths (another mount point).
                                 .foreignFolders =
                                     [this]
                                 {
                                     namespace sp16 = services::interop::sp16;
                                     std::vector<store::ForeignFolder> out;
                                     std::vector<sp16::VolumeRoot> volumes;
                                     const auto volume = [&](const char* name, const juce::File& root)
                                     {
                                         const auto alias = library.aliasPathOf(root);
                                         volumes.push_back({name, root,
                                                            alias && alias->startsWith("$Ext:")
                                                                ? alias->toStdString() : std::string()});
                                     };
                                     const juce::File mount(juce::String::fromUTF8(config.internalDisk.c_str()));
                                     if (const auto r = lib::InternalDisk::stockRoot(mount); r.isDirectory())
                                         volume("$Int0", r);
                                     if (const auto f = lib::InternalDisk::projectsFolder(mount); f.isDirectory())
                                         out.push_back({f, "INTERNAL"});
                                     for (const auto& l : library.locations())
                                     {
                                         if (! l.removable)
                                             continue;
                                         juce::File stick = l.root; // a stick's location is its TORAIZ/Samples
                                         if (stick.getFileName().equalsIgnoreCase("Samples")
                                             && stick.getParentDirectory().getFileName().equalsIgnoreCase("TORAIZ"))
                                             stick = stick.getParentDirectory().getParentDirectory();
                                         for (const char* folder : {lib::InternalDisk::kStockFolder, "TORAIZ"})
                                         {
                                             const auto root = stick.getChildFile(folder);
                                             const auto same = [&root](const sp16::VolumeRoot& v)
                                             { return v.root == root; };
                                             if (! root.isDirectory()
                                                 || std::any_of(volumes.begin(), volumes.end(), same))
                                                 continue;
                                             volume("$Ext0", root);
                                             if (const auto f = root.getChildFile("Projects"); f.isDirectory()
                                                 && std::none_of(out.begin(), out.end(),
                                                                 [&f](const store::ForeignFolder& x) { return x.folder == f; }))
                                                 out.push_back({f, "USB"});
                                         }
                                     }
                                     sp16::setVolumeRoots(std::move(volumes));
                                     return out;
                                 },
                                 .selection = [this] { return currentSelection(); },
                                 .useOsTrash = ! config.device,
                                 .resumeEnabled = pref("project.resume"),
                                 .autosaveEnabled = pref("project.autosave") &&
                                                    prefs.get("settings.autosave") >= 0.5f});
        session->setAutosaveIdleInterval(autosaveIdleMs(prefs.get("settings.autosave")));
        watchPreferences(); // RESUME / AUTOSAVE / library locations follow their preferences
        // Our choice: the user's copy of the reference lock-ordinal map (OQ-01) lives in the data folder's System/.
        services::interop::sp16::setUserMapFile(dataDir.getChildFile("System/interop/sp16-param-index-map.json"));
        session->onProjectReplaced = [this](const core::model::Project& p, const store::LoadReport& report)
        {
            sync.syncAll(p);
            slices.syncAll(p); // the samples still loading post theirs when installed
            if (chain)
                postChain(core::edit::PatternChainBuilder::clear()); // a chain belongs to the project it was built in
            forwardAll(p, bridge); // every scene (resident scenes), patterns, then the scene/pattern switch
            postArrangement(p, bridge, &core::arrangement::liveSongStatus()); // the song and song mode
            if (extensions)
                extensions->onProjectReplaced(p); // the extensions' values (only those whose flag is on)
            if (report.foreign) // OPEN of a reference project: its import report, as `cmd import-sp16` logs it
                for (const auto& w : report.warnings)
                    log("IMPORT %s", w.c_str());
            log("PROJECT %s samples=%d loaded=%d missing=%zu%s", session->store().name().toRawUTF8(), report.samples,
                report.samplesLoaded, report.missing.size(), report.readOnly ? " read-only" : "");
        };

        // Pastes are prepared on the workers (EDT-004); chains built on the PATTERN keys go to the engine.
        core::edit::setSharedWorkers(&editWorkers);
        core::edit::sharedEditor(bus);
        sceneKeys.setChainSink([this](const core::chain::ChainPlan& plan) { postChain(plan); });

        ui::m1::BrowseSource::install(&browse);
        views.install();
        // PROJECT over the session's store, SEQUENCE / SCENE MANAGER over the app's bus.
        ui::m2::installProjectSession(*session); // the screens' session gets the bus once it holds a project (startup)
        projectLog.emplace(ui::m2::ProjectSource::current(), [this](const std::string& line) { log("%s", line.c_str()); });
        ui::m2::ProjectSource::install(&*projectLog); // every save logs SAVED (panel and `cmd save-as`)
        ui::fx::environment().slots = &fxSlots;
        fxMeters.install(ui::fx::environment());

        // Takes go to <library>/Recordings (SMP-007); the ring is allocated on first use.
        sampling.emplace(bridge, workers,
                         services::sampling::SessionConfig {
                             dataDir.getChildFile("Recordings"), config.sampleRate,
                             [](std::string_view id) { return core::builtinParams().idOf(id).value_or(core::ParamId {}); }});
        sampling->log = [this](const std::string& line) { log("%s", line.c_str()); };
        sampling->onTake = [this](const services::sampling::TakeInfo& t)
        {
            if (t.cancelled)
                log("%s", "SAMPLING TAKE cancelled");
            else if (! t.ok)
                log("SAMPLING TAKE failed: %s", t.error.c_str());
            else
                log("SAMPLING TAKE ok file=%s frames=%lld channels=%d rate=%.0f bpm=%.1f bars=%d peak=%.2f dBFS%s",
                    t.file.getFullPathName().toRawUTF8(), static_cast<long long>(t.frames), t.channels, t.sampleRate,
                    static_cast<double>(t.bpm), t.bars,
                    t.peak > 0.0f ? 20.0 * std::log10(static_cast<double>(t.peak)) : -999.0,
                    t.lowDisk ? " low-disk" : "");
        };

        // The render and sampling screens' hosts (RENDER AUDIO over the render service; SAMPLING/EDIT and LIVE
        // SAMPLING over the sampling session and the take's document side), installed for the screens.
        M5HostEnv env {bus, pool, workers, bridge, [this](const std::string& line) { log("%s", line.c_str()); },
                       [this](int s, int t, const juce::File& f, std::function<void(bool)> done)
                       {
                           loader.load(s, t, f, [done](core::Result<void> r, const lib::LoadInfo&)
                                       {
                                           if (done)
                                               done(static_cast<bool>(r));
                                       });
                       },
                       dataDir.getChildFile("Recordings"),
                       [this](std::string_view id)
                       {
                           const auto* spec = core::builtinParams().find(id);
                           return spec != nullptr ? values.get(*spec, 0) : 0.0f;
                       },
                       [this](const std::string& path) { return this->resolve(path); },
                       [this](int s, int t) { return sync.current(s, t); }};
        renderHost = makeRenderHost(env, *renderer);
        samplingHost = makeSamplingHost(env, *sampling);
        ui::m5::RenderHost::install(renderHost.get());
        ui::m5::SamplingHost::install(samplingHost.get());
        // The P3 extensions' glue (app/ext/Extensions.h); inert while every ext.* flag is off.
        extensions.emplace(app::ext::ExtEnv {
            .bus = &bus,
            .bridge = &bridge,
            .log = [this](const std::string& line) { log("%s", line.c_str()); },
            .setRemote = [this](const core::ParamSpec& spec, std::uint16_t scope, float v) { values.setRemote(spec, scope, v); },
            .value = [this](const core::ParamSpec& spec, std::uint16_t scope) { return values.get(spec, scope); },
            .selectedTrack = [this] { return selection.selected(); },
            .recArmed = [this] { return selection.shell != nullptr && selection.shell->state().recArmed; },
            .padMode = [this] { return static_cast<int>(pads.effectiveMode()); },
            .resolveSample = [this](const std::string& path) { return resolve(path); },
            .preference = [this](std::string_view key) { return prefs.get(key); }});
    }

    ~Impl()
    {
        ui::m5::RenderHost::install(nullptr);
        ui::m5::SamplingHost::install(nullptr);
        samplingHost.reset();
        renderHost.reset();
        ui::utility::utilityEnv() = {}; // its providers refer to this document
        services::settings::install(nullptr);
        ui::slice::sliceLive().buffer = nullptr;
        ui::fx::environment().slots = nullptr;
        ui::fx::environment().meters = nullptr;
        ui::fx::environment().metersUser = nullptr;
        ui::m1::BrowseSource::install(nullptr);
        ui::m2::ProjectSource::install(nullptr);
        ui::m2::Session::instance().install(nullptr);
        core::commands::setBridge(bus, {});
        workers.drain(); // completions refer to the members below
        core::edit::setSharedWorkers(nullptr);
        core::edit::resetSharedEditor();
    }

    // SCN-030: the chain to the engine (ObjectSwap ProjectConfig 0/0; length 0 clears) and kept for the offline render.
    void postChain(const core::chain::ChainPlan& plan)
    {
        chain = plan;
        if (plan.length == 0)
            chain.reset();
        auto* object = new core::chain::ChainPlan(plan);
        if (! bridge.post(engine::ObjectSwap {{engine::SwapSlot::ProjectConfig, core::chain::kSwapScene,
                                               core::chain::kSwapIndex},
                                              object}))
            core::chain::destroyChainPlan(object);
        log("EDIT CHAIN scene=%d patterns=%s", plan.scene + 1, chainText(plan).c_str());
    }
    static std::string chainText(const core::chain::ChainPlan& plan)
    {
        std::string s;
        for (int i = 0; i < plan.length; ++i)
            s += (i ? "," : "") + std::to_string(plan.patterns[static_cast<std::size_t>(i)] + 1);
        return s.empty() ? "-" : s;
    }

    // The arrangement plan (ObjectSwap SceneConfig 0/15, rebuilt after every row or pattern-length edit) and
    // song mode (arrangement.play) to the engine. `status` = where the engine publishes the song position (the live
    // engine's; null for an offline render).
    static void postPlan(const core::model::Project& p, engine::EngineBridge& to, core::arrangement::SongStatus* status)
    {
        feed::postPlan(p, to, status); // app/ProjectFeed.h (shared with the offline render)
    }
    static void postPlay(const core::model::Project& p, engine::EngineBridge& to) { feed::postPlay(p, to); }
    static void postArrangement(const core::model::Project& p, engine::EngineBridge& to,
                                core::arrangement::SongStatus* status)
    {
        postPlan(p, to, status);
        postPlay(p, to);
    }
    static void forwardArrangement(const core::ChangeSet& changes, const core::model::Project& p,
                                   engine::EngineBridge& to)
    {
        if (core::commands::arrangement::affectsPlan(changes))
            postPlan(p, to, &core::arrangement::liveSongStatus());
        if (core::commands::arrangement::affectsPlay(changes))
            postPlay(p, to);
    }
    void arrangementCommand(const std::string& cmd, std::istringstream& in);
    void importReference(std::string args); // `cmd import-sp16 <file> [copy]`
    lib::Progress importProgress;
    void logArrangement();

    // Every value the engine needs for project `p` (app/ProjectFeed.h: the 16 scenes' track tables and FX, the 256
    // patterns, the sequencer and ribbon values, the scene switch, the routing). ≈ 15,000 messages: the QueuedBridge
    // hands them on as the engine's FIFO drains.
    static void forwardAll(const core::model::Project& p, engine::EngineBridge& to) { feed::forwardAll(p, to); }

    template <typename... A> void log(const char* fmt, A... args) const
    {
        if (! config.log)
            return;
        char buf[600];
        if constexpr (sizeof...(A) == 0)
            std::snprintf(buf, sizeof buf, "%s", fmt); // a plain message is text, never a format
        else
            std::snprintf(buf, sizeof buf, fmt, args...);
        config.log(buf);
    }

    std::optional<juce::File> resolve(const std::string& path) const
    {
        if (session)
            return session->resolveSample(path);
        return library.resolve(juce::String(path));
    }

    store::Selection currentSelection() const
    {
        store::Selection s;
        if (selection.shell != nullptr)
        {
            const auto& st = selection.shell->state();
            s.scene = (st.scene - 1) & 15;
            s.pattern = (st.pattern - 1) & 15;
            s.track = (st.selectedTrack - 1) & 15;
            const auto id = selection.shell->current().id;
            s.screen = id == "home" ? std::string() : std::string(id);
        }
        return s;
    }

    void loadKit()
    {
        juce::File folder = juce::File::isAbsolutePath(config.devKit)
                                ? juce::File(config.devKit)
                                : juce::File(config.dataDir).getChildFile(config.devKit);
        if (! folder.isDirectory())
        {
            log("KIT FAILED no folder %s", folder.getFullPathName().toRawUTF8());
            return;
        }
        log("KIT loading %s", folder.getFullPathName().toRawUTF8());
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        kitScenes = std::clamp(config.devKitScenes, 1, 16);
        for (int scene = 0; scene < kitScenes; ++scene)
            loader.loadFolder(scene, folder,
                              [this, t0, scene](core::Result<void> r)
                              {
                                  if (! r)
                                      log("KIT FAILED %s", r.error().message.c_str());
                                  else if (scene == kitScenes - 1)
                                  {
                                      kitPending = true;
                                      kitStartMs = t0;
                                  }
                              });
    }

    void checkKit(double nowMs)
    {
        if (! kitPending || ! core::commands::hasProject(bus))
            return;
        int installed = 0;
        for (int scene = 0; scene < kitScenes; ++scene)
            for (int t = 0; t < 16; ++t)
                installed += sync.current(scene, t) != nullptr && ! sync.loading(scene, t) ? 1 : 0;
        if (installed < 16 * kitScenes || queued.pending() != 0)
            return;
        installed /= kitScenes;
        kitPending = false;
        log("KIT ready installed=%d tracks=1-16 scenes=1-%d in %.0f ms resident=%lld kB", installed, kitScenes,
            juce::Time::getMillisecondCounterHiRes() - kitStartMs, static_cast<long long>(pool.residentBytes() / 1024));
        (void)nowMs;
    }

    QueuedBridge queued; // every Document post goes through it (a project open posts more than the FIFO holds)
    engine::EngineBridge& bridge;
    DocumentConfig config;

    void syncSequencer(double nowMs);
    void command(const std::string& text);
    void logState();
    void render(const std::string& file, double seconds, bool withChain = false, bool song = false);
    lib::Workers workers{2};
    core::edit::WorkerPoolAdapter<lib::Workers> editWorkers{workers}; // the SceneEditor's (EDT-004)
    std::optional<core::chain::ChainPlan> chain; // the chain last posted (SCN-030)
    core::commands::arrangement::RowClipboard rowClipboard; // ARR-012 (the ARRANGER screen's)
    core::CommandBus bus;
    lib::SampleLibrary library{workers};
    lib::SamplePool pool;
    std::optional<store::ProjectSession> session;
    lib::TrackSampleSync sync;
    lib::PreviewService preview;
    lib::TrackLoader loader;
    LibraryBrowseSource browse;
    SampleViewsAdapter views;
    ui::fx::ModelFxSlots fxSlots;
    ui::fx::EngineFxMeters fxMeters;
    ui::ModelParamAccess values;
    ShellSelection selection;
    ui::input::pads::PadController pads;
    // The scene sync, the SCENE / PATTERN keys, the step keys and their lock access (the
    // shell's hold + turn writes step locks through lockValues).
    ui::input::scenes::SceneSync sceneSync;
    ui::input::scenes::ScenePatternKeys sceneKeys;
    ui::input::steps::StepController steps;
    ui::input::steps::StepLockAccess lockValues;
    RecordingAccess recording; // the shell's ParamAccess: lockValues + SEQ-023 encoder recording
    // SHIFT + SCENE, SHIFT + lit SCALE, ext.undo / ext.redo, ext.clear + held step (NFR-040); the pad
    // controller's lamps (pad modes, mode keys) over the shell's lamp frame.
    ui::input::panel::PanelKeys panelKeys {bus, pads, steps};
    ui::input::pads::PadLamps padLamps;
    std::optional<LoggingProjectSource> projectLog;
    io::StripRouter strip; // MODE / HOLD, the strip and pad pressure → engine, the strip lamps
    // The preferences (<data dir>/preferences.json, saved on a worker ≤ 1 s after a change) and the
    // shell's ParamAccess that saves every preference edit (ui/screens/utility/PrefAccess.h).
    services::settings::Preferences prefs {core::builtinParams(),
                                           [this](std::function<void()> job) { workers.run(std::move(job)); }};
    ui::utility::PreferenceAccess prefAccess {recording, prefs};
    std::vector<core::Subscription> prefWatch;
    void loadPreferences(const juce::File& dataDir);
    void watchPreferences();
    void applyLibraryLocations();
    void applyMidiInputs();
    void applyPadPrefs(); // PAD CURVE / THRESHOLD / SENSITIVITY / MODE SELECT RESPONSE
    io::PanelPadPrefs padPrefs {};
    bool padPrefsApplied = false, padModeHold = false;
    void prefsCommand(const std::string& cmd, std::istringstream& in);
    // The slice tables of the samples the tracks play (ObjectSwap SliceMap per edit / new sample).
    ui::slice::SliceSync slices {bridge, [this](int s, int t) { return framesOf(sync, s, t); }};
    static std::int64_t framesOf(const lib::TrackSampleSync& from, int s, int t)
    {
        const auto* b = from.current(s, t);
        return b != nullptr ? b->frames : 0;
    }
    bool slicePad(const io::ControlEvent& e);   // SLICE pad mode presses (SLC-003/005), UI-403
    // ANALOG FILTER: ACTIVE toggles afs.active; the absolute DRIVE / CUTOFF / RESONANCE / HP CUTOFF controls
    // (0–1023, the catalogue's units) set afs.drive / lpCutoff / resonance / hpCutoff.
    bool afsControl(const io::ControlEvent& e)
    {
        const char* id = nullptr;
        switch (e.control)
        {
        case io::Control::AfsActive:
            if (e.kind == io::ControlEvent::Kind::Press)
                if (const auto* spec = core::builtinParams().find("afs.active"))
                    values.set(*spec, 0, values.get(*spec, 0) >= 0.5f ? 0.0f : 1.0f);
            return e.kind == io::ControlEvent::Kind::Press || e.kind == io::ControlEvent::Kind::Release;
        case io::Control::AfsDrive: id = "afs.drive"; break;
        case io::Control::AfsCutoff: id = "afs.lpCutoff"; break;
        case io::Control::AfsResonance: id = "afs.resonance"; break;
        case io::Control::AfsHpCutoff: id = "afs.hpCutoff"; break;
        case io::Control::MainVolume: id = "routing.mainKnob"; break; // PHONES + OUT3-8 level, MAIN gate (F113)
        default: return false;
        }
        if (e.kind != io::ControlEvent::Kind::Value)
            return false;
        if (const auto* spec = core::builtinParams().find(id))
            values.set(*spec, 0, spec->clamp(static_cast<float>(e.value)));
        return true;
    }
    mutable double overAtMs = -1e12; // the last output clip (OVER lamp)
    // The bars (1–4) of the pattern on the step keys: the playing one, else the selected one (UI-155).
    int patternBars() const
    {
        const auto& p = core::commands::project(bus);
        const auto& ph = bridge.playhead();
        const bool live = ph.blockIndex > 0;
        const int scene = live ? ph.scene & 15 : p.settings.currentScene & 15;
        const int pattern = live ? ph.pattern & 15 : p.settings.currentPattern & 15;
        const int length = p.scenes[static_cast<std::size_t>(scene)].patterns[static_cast<std::size_t>(pattern)].length;
        return std::clamp((length + 15) / 16, 1, 4);
    }
    void sliceStepEntry(int step1to16);         // SLC-005: step entry writes the last-played slice
    void sliceCommand(std::istringstream& in);  // `cmd slice count N | move S FRAME | add FRAME | delete S | area A | state`
    // Incoming CCs the engine applied (io::midi::setUiEventSink, drained by the "midi" control source
    // before frame()) → the model, non-undoable and coalesced to the last value per parameter and scope each frame.
    std::vector<core::midi::UiEvent> midiCcs;
    std::uint64_t midiModelWrites = 0;
    void queueMidiCc(const core::midi::UiEvent& e)
    {
        if (e.kind != core::midi::UiEvent::Kind::Param)
            return;
        for (auto& q : midiCcs)
            if (q.param == e.param && q.scope == e.scope)
            {
                q.value = e.value;
                return;
            }
        midiCcs.push_back(e);
    }
    void flushMidiCcs()
    {
        if (midiCcs.empty())
            return;
        const auto& reg = core::builtinParams();
        if (core::commands::hasProject(bus))
            for (const auto& e : midiCcs)
                if (e.param < reg.size())
                {
                    const auto& spec = reg[core::ParamId {e.param}];
                    values.setRemote(spec, e.scope, e.value);
                    if (++midiModelWrites <= 16) // the first ones (diagnostics; a CC stream is not logged)
                        log("MIDI MODEL %.*s scope=%d value=%g", static_cast<int>(spec.id.size()), spec.id.data(),
                            static_cast<int>(e.scope), static_cast<double>(e.value));
                }
        midiCcs.clear();
    }
    // The offline render service (destroyed before the bridge, the sync and the loader it refers to;
    // after `sampling`).
    void renderCommand(const std::string& cmd, std::istringstream& in);
    std::optional<services::render::RenderService> renderer;
    services::render::JobHandle renderJob = 0;
    int renderProgressLogged = -1;
    // The capture hub, the Recorder thread, live takes (last member: destroyed first, before workers).
    std::optional<services::sampling::SamplingSession> sampling;
    // The render and sampling screens' hosts (destroyed before the services they refer to).
    std::unique_ptr<ui::m5::RenderHost> renderHost;
    std::unique_ptr<ui::m5::SamplingHost> samplingHost;
    std::optional<app::ext::Extensions> extensions; // constructed last, destroyed first
    void samplingCommand(const std::string& cmd, std::istringstream& in);
    bool shift = false;
    int recordedHits = 0;
    std::uint64_t lastPoll = ~std::uint64_t {0};
    double lastPollMs = -1e12;
    double lastTrimMs = 0;     // the last heap trim (Document::frame)
    bool kitPending = false;
    double frameMs = 0; // the last frame()'s time (lamps)
    int kitScenes = 1;
    double kitStartMs = 0;
};

Document::Document(engine::EngineBridge& bridge, DocumentConfig config)
    : impl_(std::make_unique<Impl>(bridge, std::move(config)))
{
}

Document::~Document()
{
    // The session goes before the pool and the library it points to (members are destroyed in reverse order, and
    // the session is declared after the pool only because it needs the pool's address).
    ui::m2::ProjectSource::install(nullptr); // its adapter refers to the session's store
    impl_->session.reset();
}

ui::ParamAccess& Document::values() noexcept
{
    return impl_->prefAccess; // ModelParamAccess + step locks (SEQ-022) + SEQ-023 recording + preferences (SET-010)
}

std::function<void(ui::UiState&, double)> Document::summaries()
{
    return [this](ui::UiState& st, double)
    {
        auto& d = *impl_;
        if (! core::commands::hasProject(d.bus))
            return;
        const auto& p = core::commands::project(d.bus);
        ui::fillTrackSummaries(
            st, p,
            [](const void* sync, int t)
            {
                const auto& s = *static_cast<const lib::TrackSampleSync*>(sync);
                return s.loading(0, t);
            },
            &d.sync);
        st.scene = (p.settings.currentScene & 15) + 1;
        st.pattern = (p.settings.currentPattern & 15) + 1;
        st.dirty = d.bus.dirty();
        for (int t = 0; t < 16; ++t) // the tiles' UNSAVED TAKE badge (a take played as RECORDED TEMP.)
            st.tracks[static_cast<std::size_t>(t)].unsavedTake =
                d.samplingHost != nullptr && d.samplingHost->unsaved(p.settings.currentScene & 15, t);
        auto& t1 = st.tracks[0];
        if (d.config.testTone && ! t1.hasContent && p.scenes[0].tracks[0].mode == core::model::TrackMode::Sample)
        {
            t1.name = "Test tone 440 Hz"; // the engine's fallback tone sounds while track 1 has no sample
            t1.hasContent = true;
        }
    };
}

void Document::attach(ui::Root& root)
{
    impl_->selection.shell = root.shell();
    impl_->panelKeys.attach(root.shell());
    impl_->panelKeys.log = [this](const std::string& line) { impl_->log("%s", line.c_str()); };
    if (auto* shell = root.shell()) // THEME (UID-051)
        shell->state().dark = impl_->prefs.get("ui.theme") < 0.5f;
}

void Document::startup()
{
    impl_->midiCcs.reserve(128);
    io::midi::setUiEventSink([this](const core::midi::UiEvent& e) { // incoming MIDI (+ MIDI learn's CCs)
        if (! (impl_->extensions && impl_->extensions->onMidiUi(e)))
            impl_->queueMidiCc(e);
    });
    impl_->session->startup(
        [this](const store::StartupResult& r)
        {
            auto& d = *impl_;
            ui::m2::Session::instance().install(&d.bus); // SEQUENCE / SCENE MANAGER edit the app's document
            if (r.message.isNotEmpty())
                d.log("PROJECT note: %s", r.message.replace("\n", " ").toRawUTF8());
            d.log("PROJECT %s %s", r.resumed ? "resumed" : "new", d.session->store().name().toRawUTF8());
            if (r.resumed && d.selection.shell != nullptr) // PRJ-023
                d.selection.shell->state().selectedTrack = (r.selection.track & 15) + 1;
            if (! d.config.devKit.empty())
                d.loadKit();
        });
}

bool Document::handle(const io::ControlEvent& e)
{
    auto& d = *impl_;
    using Kind = io::ControlEvent::Kind;
    const bool press = e.kind == Kind::Press, edge = press || e.kind == Kind::Release;
    if (e.control == io::Control::Shift && edge)
        d.shift = press;
    if (e.control == io::Control::ExtErase && edge)
        d.steps.setErase(press); // SEQ-046
    // EDT-005: held COPY / PASTE + step keys; on SCENE MANAGER the keys are its COPY / PASTE buttons,
    // so the step gesture (which copies the shown bar on a bare COPY) stays out of the clipboard.
    const bool sceneManager = d.selection.shell != nullptr && d.selection.shell->current().id == "sceneManager";
    if (e.control == io::Control::ExtCopy && edge && (! sceneManager || ! press))
        d.steps.setCopy(press);
    if (e.control == io::Control::ExtPaste && edge && (! sceneManager || ! press))
        d.steps.setPaste(press);
    d.strip.handle(e); // never consumes
    if (d.extensions)
        d.extensions->handle(e); // held pads for recorded strip moves (never consumes)
    if (d.afsControl(e)) // the panel's ANALOG FILTER controls
        return true;
    if (d.panelKeys.handle(e, d.shift)) // before the SCENE / PATTERN keys and the pad modes
        return true;
    if (core::commands::hasProject(d.bus) && d.sceneKeys.handle(e, d.shift))
        return true; // SCENE / PATTERN modes: the step and BAR keys are theirs while a mode is on
    const bool sequenceScreen = d.selection.shell != nullptr && d.selection.shell->current().id == "track.sequence";
    if (! sequenceScreen) // the SEQUENCE screen's step keys select steps (UI-246); elsewhere they enter triggers
    {
        if (io::isStep(e.control) && edge && core::commands::hasProject(d.bus))
        {
            d.steps.onStepKey(io::indexOf(e.control), press, e.timeMs); // SEQ-040 / SEQ-022 (the shell sees it too)
            if (! press)
                d.sliceStepEntry(io::indexOf(e.control)); // SLC-005
        }
        if (press && e.control >= io::Control::Bar1 && e.control <= io::Control::Bar4 && core::commands::hasProject(d.bus))
        {
            const int bar0 = static_cast<int>(e.control) - static_cast<int>(io::Control::Bar1);
            if (bar0 < d.patternBars()) // a BAR key past the pattern length is dark and does nothing (UI-155)
                ui::input::steps::setShownBar(bar0);
        }
    }
    if (d.slicePad(e)) // SLICE pad mode (the pads play slices; they do not select tracks)
        return true;
    d.pads.handle(e);
    // In MUTE and SCALE pad mode a pad does not select track n (PAD-004: SHIFT + pad selects in every mode
    // but MUTE; PAD-006, PAD-010): keep the press from the shell, whose default pad handling would select it — in SCALE
    // mode the next pads would then play that track's sample. The selected track's tile flashes for a scale hit.
    using PadMode = ui::input::pads::PadMode;
    const auto mode = d.pads.effectiveMode(); // MUTE while the MUTE key is held (solo)
    if (io::isPad(e.control) && edge && (mode == PadMode::Mute || (mode == PadMode::Scale && ! d.shift))
        && d.selection.shell != nullptr && core::commands::hasProject(d.bus))
    {
        if (press)
        {
            const int n = io::indexOf(e.control);
            const auto& p = core::commands::project(d.bus);
            const auto& tracks = p.scenes[static_cast<std::size_t>(p.settings.currentScene & 15)].tracks;
            if (mode == PadMode::Scale)
            {
                d.selection.shell->state().tracks[static_cast<std::size_t>(d.selection.selected())].lastHitMs = e.timeMs;
                d.log("INPUT pad%d scale track=%d", n, d.selection.selected() + 1);
            }
            else
            {
                int muted = 0, soloed = 0;
                for (const auto& t : tracks)
                    muted += t.mute ? 1 : 0, soloed += t.solo ? 1 : 0;
                d.log("INPUT pad%d %s mute=%d solo=%d muted=%d soloed=%d", n,
                      d.shift ? "mute-others" : d.pads.soloHeld() ? "solo" : "mute",
                      tracks[static_cast<std::size_t>(n - 1)].mute ? 1 : 0,
                      tracks[static_cast<std::size_t>(n - 1)].solo ? 1 : 0, muted, soloed);
            }
        }
        return true;
    }
    return false;
}

void Document::frame(double nowMs)
{
    auto& d = *impl_;
    d.queued.flush(); // what the engine's FIFO could not take last frame (a project open)
    d.flushMidiCcs(); // this frame's incoming CCs → the model (non-undoable)
    d.workers.pump();
    if (d.renderer)
        d.renderer->pump(); // progress and completion of the offline render
    if (d.sampling)
        d.sampling->frame(); // live takes → the track (worker build, swap, take ready)
    d.session->frame(nowMs);
    d.prefs.tick(nowMs); // the debounced preferences save
    d.pool.collect();
    d.preview.tick();
    d.pads.sync();
    d.pads.tick(nowMs); // PAD MODE SELECT RESPONSE OVER 1sec (PAD-007)
    d.syncSequencer(nowMs);
    d.frameMs = nowMs;
    auto& live = ui::m4::stripLive(); // TOUCH STRIP SETTING's StripPreview
    live.mode = static_cast<int>(d.strip.mode());
    live.contact = d.strip.contact();
    live.position = d.strip.position();
    if (d.extensions) // flags, MIDI learn's overlay, recorded strip moves
        d.extensions->frame(nowMs, d.bridge.playhead(), live.mode, live.contact, live.position);
    core::commands::flushNotifications(d.bus);
    d.checkKit(nowMs);
    if (nowMs - d.lastPollMs >= 1000.0) // removable volumes (LIB-002), stale waveform conversions
    {
        d.lastPollMs = nowMs;
        d.library.pollLocations();
        d.views.collect();
    }
    // NFR-014 (our choice): glibc keeps the pages of freed heap memory (an offline render's engine, a replaced
    // project and its undo history, decode buffers) inside its arena; a later allocation that does not fit a hole
    // grows the heap, so RSS climbed ~2 MB every few project opens with the bytes in use flat (tools/soak/soak-run.py
    // --heap-stats). Once a minute a worker returns the free pages to the system: RSS stayed flat over 16 opens and
    // ~35 MB lower, with no audio-thread effect (the audio thread never allocates; madvise and page faults share the
    // mmap lock in read mode).
#if defined(__GLIBC__)
    if (nowMs - d.lastTrimMs >= 60000.0)
    {
        d.lastTrimMs = nowMs;
        d.workers.run([] { malloc_trim(0); });
    }
#endif
}

void Document::command(const std::string& text)
{
    impl_->command(text);
}

void Document::lamps(io::LampFrame& frame) const
{
    auto& d = *impl_;
    // The step keys and SCENE / PATTERN keys. While a SCENE / PATTERN mode is on its keys own the step
    // lamps (slots, pending blink, length mode); otherwise the step controller's view of the shown bar (UI-153).
    if (d.selection.shell != nullptr && core::commands::hasProject(d.bus))
    {
        const auto& ph = d.bridge.playhead();
        if (d.sceneKeys.ownsStepKeys())
            d.sceneKeys.lamps(frame, ph, d.frameMs);
        else
        {
            FrameLampSink sink(frame);
            d.steps.paintStepLamps(sink, ph);
        }
        // UI-155: the BAR keys (lit inside the pattern length, the shown bar brighter) and the playback bar indicators
        // above them (the playing bar, pulsing on the beat). Hardware feedback: nothing painted them, so the four
        // lamps stayed dark outside pattern-length mode (which paints its own BAR lamps).
        if (d.sceneKeys.mode() != ui::input::scenes::KeyMode::Length)
            io::barLamps(d.patternBars(), ui::input::steps::shownBar() + 1, ph.step / 16 + 1,
                         ph.blockIndex > 0 && ph.playing, ph.ppq - std::floor(ph.ppq), frame);
    }
    // The pad controller's pad-mode lamps and mode keys; its paint is limited to 60 Hz and PadLamps
    // keeps the last paint, so a skipped paint shows the previous state, not the shell's pads.
    d.pads.paintLamps(d.padLamps, d.frameMs, d.bridge.meters());
    d.padLamps.overlay(frame, d.pads.effectiveMode());
    d.strip.lamps(frame);
    // UI-156/158: ANALOG FILTER ACTIVE (red) from afs.active; OVER (amber) while the master clip latch holds (our
    // choice: the engine publishes no AFS overload — MeterFeed is frozen — so OVER shows the output clip that a driven
    // filter causes, latched 2 s like the meters).
    if (const auto* active = core::builtinParams().find("afs.active"))
        frame[io::Control::AfsActive] = {d.values.get(*active, 0) >= 0.5f ? io::LampMode::On : io::LampMode::Off,
                                         io::lampcolour::red, 0};
    const auto& m = d.bridge.meters();
    if (m.masterPeak[0] >= 1.0f || m.masterPeak[1] >= 1.0f)
        d.overAtMs = d.frameMs;
    frame[io::Control::AfsOver] = {d.frameMs - d.overAtMs < 2000.0 ? io::LampMode::On : io::LampMode::Off,
                                   io::lampcolour::amber, 0};
}

void Document::shutdown()
{
    io::midi::setUiEventSink(nullptr);
    if (impl_->renderer) // a running render is cancelled, the live output unmuted (before the engine goes)
        impl_->renderer->stop();
    impl_->preview.stop();
    impl_->session->shutdown();
    impl_->prefs.flush(); // nothing unsaved at quit
    impl_->log("PREFS flushed writes=%d%s", impl_->prefs.writes(),
               impl_->prefs.lastError().empty() ? "" : (" error=" + impl_->prefs.lastError()).c_str());
}
} // namespace app

namespace app
{
// The engine's events of the last UI frame (the shell drained them) → the recorder and the scene keys; the
// playhead and the transport's REC / playing state → the step controller (one recording pass = one undo step).
void Document::Impl::syncSequencer(double nowMs)
{
    if (selection.shell == nullptr || ! core::commands::hasProject(bus))
        return;
    const auto& ph = bridge.playhead();
    const auto& feeds = selection.shell->feeds();
    const bool fresh = feeds.frames() != lastPoll; // the shell polls once per drawn frame: each batch once
    lastPoll = feeds.frames();
    for (const auto& e : fresh ? feeds.events() : std::span<const engine::EngineEvent> {})
    {
        if (e.kind == engine::EngineEvent::Kind::RecordedHit)
            ++recordedHits;
        steps.onEngineEvent(e);    // RecordedHit / ErasedStep → recordHit / clearStep in the pass's group
        if (e.kind == engine::EngineEvent::Kind::RecordedHit) // SLC-005: the hit's slice lock
            if (const auto slice = ui::slice::sliceLive().takeRecorded(e.track, nowMs))
            {
                bus.execute(core::commands::sequencer::setLock(steps.scene(), steps.pattern(), e.track, e.step, 22,
                                                               static_cast<float>(*slice)));
                log("SLICE recorded track=%d step=%d slice=%d", e.track + 1, e.step + 1, *slice + 1);
            }
        sceneKeys.onEngineEvent(e); // SwitchDone → the model's current scene / pattern
        if (e.kind == engine::EngineEvent::Kind::SwitchDone)
            log("M2 SWITCH scene=%d pattern=%d frame=%d", e.value / 16 + 1, e.value % 16 + 1, e.step);
    }
    sceneKeys.observe(ph);
    const auto& p = core::commands::project(bus);
    const auto& st = selection.shell->state();
    steps.setContext(p.settings.currentScene, p.settings.currentPattern, st.selectedTrack - 1);
    steps.setStepKeysOwned(sceneKeys.ownsStepKeys());
    if (const auto* spec = core::builtinParams().find("seq.triggerType"))
    {
        const int t = static_cast<int>(std::lround(values.get(*spec, 0)));
        steps.setEntryType(t == 1 ? core::model::TriggerType::Half
                                  : (t == 2 ? core::model::TriggerType::LockOnly : core::model::TriggerType::Full));
    }
    // REC armed = the transport controller's (UiState::recArmed, published after the model each frame); the
    // metronome is not forced on for recording passes (SEQ-045 names no mode; the pre-count clicks anyway, SEQ-044).
    steps.setTransport(ph.playing, st.recArmed);
    // MOTION REC: the selected track's switch gates knob recording (TRACK MENU › MOTION REC, TRACK SETTING).
    static const auto* motionRec = core::builtinParams().find("track.motionRec");
    const bool motion = motionRec != nullptr &&
                        values.get(*motionRec, static_cast<std::uint16_t>((p.settings.currentScene & 15) << 4 |
                                                                          ((st.selectedTrack - 1) & 15))) >= 0.5f;
    steps.setMotionRec(motion);
    steps.tick(nowMs);
}

// A pad in SLICE pad mode plays the selected track's slice (the engine routes it: a slice-number lock);
// here the message thread notes which slice it was (SET SLICE's SLICE No., step entry, the recorded hit's lock) and
// keeps the press from the shell, whose default pad handling would select track n (the engine would then play that
// track's slices). SHIFT + pad still selects (PAD-004). UI-403: SHIFT + the lit SLICE key opens SET SLICE and keeps
// the mode on.
bool Document::Impl::slicePad(const io::ControlEvent& e)
{
    using Kind = io::ControlEvent::Kind;
    if (e.kind == Kind::Press && e.control == io::Control::PadModeSlice && shift
        && pads.mode() == ui::input::pads::PadMode::Slice && selection.shell != nullptr)
    {
        if (selection.shell->current().id != "track.playback.setSlice")
        {
            selection.shell->open("track.playback");
            selection.shell->open("track.playback.setSlice");
        }
        return true;
    }
    if (! io::isPad(e.control) || (e.kind != Kind::Press && e.kind != Kind::Release) || shift
        || pads.effectiveMode() != ui::input::pads::PadMode::Slice) // MUTE held: solo, not slices
        return false;
    pads.handle(e); // the controller posts releases and the presses of indirect sources
    if (e.kind == Kind::Press && core::commands::hasProject(bus) && selection.shell != nullptr)
    {
        const auto& p = core::commands::project(bus);
        const int s = p.settings.currentScene & 15, t = selection.selected();
        const int slice = ui::slice::padSlice(p, s, t, io::indexOf(e.control) - 1, framesOf(sync, s, t));
        if (slice >= 0)
        {
            // Only a hit the engine can record waits for its RecordedHit (REC armed and the transport running).
            const bool recordable = selection.shell->state().recArmed && bridge.playhead().playing;
            ui::slice::sliceLive().onHit(t, slice, e.timeMs, recordable);
            selection.shell->state().tracks[static_cast<std::size_t>(t)].lastHitMs = e.timeMs; // the tile flashes
        }
        log("INPUT pad%d slice=%d", io::indexOf(e.control), slice + 1);
    }
    return true;
}

// `cmd slice …` (host smoke): the SET SLICE edits of the selected track in the current scene, through
// core/commands/slice (undoable, posted to the engine by SliceSync); slices are 1-based. `state` logs
// "SLICE scene=S track=T count=N area=A frames=F points=p1,p2,…,end edited=E locks=step:slice,…".
void Document::Impl::sliceCommand(std::istringstream& in)
{
    namespace sc = core::commands::slice;
    std::string what;
    in >> what;
    const auto& p = core::commands::project(bus);
    const int s = p.settings.currentScene & 15, t = selection.selected();
    const std::int64_t frames = framesOf(sync, s, t);
    std::unique_ptr<core::Command> c;
    long long a = 0;
    if (what == "count" && (in >> a))
        c = sc::setCount(p, s, t, static_cast<int>(a));
    else if (what == "move")
    {
        long long frame = 0;
        if (in >> a >> frame)
            c = sc::movePoint(p, s, t, static_cast<int>(a) - 1, frame, frames);
    }
    else if (what == "add" && (in >> a))
        c = sc::addPoint(p, s, t, a, frames);
    else if (what == "delete" && (in >> a))
        c = sc::deletePoint(p, s, t, static_cast<int>(a) - 1, frames);
    else if (what == "area" && (in >> a))
        c = sc::setArea(p, s, t, static_cast<int>(a) - 1, frames);
    else if (what != "state")
    {
        log("CMD slice: usage count N | move S FRAME | add FRAME | delete S | area A | state");
        return;
    }
    if (what != "state")
    {
        if (c == nullptr)
        {
            log("CMD slice %s: nothing to change", what.c_str());
            return;
        }
        bus.execute(std::move(c));
        core::commands::flushNotifications(bus);
    }
    const auto& now = sc::settings(core::commands::project(bus), s, t);
    const auto pts = core::slice::slicePoints(now, frames);
    std::string list;
    for (int k = 0; k < pts.count; ++k)
        list += std::to_string(pts.start(k)) + ",";
    list += std::to_string(pts.frames);
    // The slice-number locks (slot 22) of the track in the current pattern: step:slice, 1-based.
    std::string locks;
    const auto& pat = core::commands::project(bus).scenes[static_cast<std::size_t>(s)]
                          .patterns[static_cast<std::size_t>(p.settings.currentPattern & 15)];
    for (int step = 0; step < core::model::kSteps; ++step)
    {
        float v = 0;
        if (core::commands::sequencer::lockOf(pat, step, t, 22, v))
            locks += (locks.empty() ? "" : ",") + std::to_string(step + 1) + ":" + std::to_string(std::lround(v) + 1);
    }
    log("SLICE scene=%d track=%d count=%d area=%d frames=%lld points=%s edited=%d locks=%s", s + 1, t + 1, pts.count,
        core::slice::clampArea(now.windowOffset, pts.count) + 1, static_cast<long long>(frames), list.c_str(),
        core::slice::editedPoints(now), locks.empty() ? "-" : locks.c_str());
}

// SLC-005: a step entered in SLICE pad mode gets the slice-number lock of the selected track's last-played slice.
void Document::Impl::sliceStepEntry(int step1to16)
{
    if (pads.mode() != ui::input::pads::PadMode::Slice || ! core::commands::hasProject(bus) || step1to16 < 1
        || step1to16 > 16)
        return;
    const int t = selection.selected();
    const int slice = ui::slice::sliceLive().lastSlice[static_cast<std::size_t>(t)];
    if (slice < 0)
        return;
    const auto& p = core::commands::project(bus);
    const int sc = steps.scene(), pat = steps.pattern(), step = steps.patternStep(step1to16);
    const auto& cell = p.scenes[static_cast<std::size_t>(sc & 15)].patterns[static_cast<std::size_t>(pat & 15)]
                           .steps[static_cast<std::size_t>(step & 63)][static_cast<std::size_t>(t)];
    float current = -1.0f;
    if (cell.trigger == core::model::Step::kEmpty
        || (core::commands::sequencer::lockOf(p.scenes[static_cast<std::size_t>(sc & 15)].patterns[static_cast<std::size_t>(pat & 15)],
                                    step, t, 22, current) && current == static_cast<float>(slice)))
        return; // the release cleared the step (or it already plays this slice)
    bus.execute(core::commands::sequencer::setLock(sc, pat, t, step, 22, static_cast<float>(slice)));
}

void Document::Impl::command(const std::string& text)
{
    std::istringstream in(text);
    std::string cmd;
    in >> cmd;
    if (cmd.rfind("pref", 0) == 0) // prefs | pref <key> <value> | prefs-flush | prefs-export/-import
    {
        prefsCommand(cmd, in);
        return;
    }
    if (! core::commands::hasProject(bus))
    {
        log("CMD %s: no project yet", text.c_str());
        return;
    }
    const auto& params = core::builtinParams();
    if (cmd == "scene-bpm")
    {
        int scene = 0;
        double bpm = 0;
        in >> scene >> bpm;
        bus.execute(core::commands::scene::setSceneBpm((scene - 1) & 15, bpm > 0 ? std::optional<double>(bpm)
                                                                                 : std::nullopt));
    }
    else if (cmd == "trigger-type")
    {
        std::string t;
        in >> t;
        if (const auto* spec = params.find("seq.triggerType"))
            values.set(*spec, 0, t == "half" ? 1.0f : (t == "param" ? 2.0f : 0.0f));
    }
    else if (cmd == "lock")
    {
        int step = 0;
        std::string id;
        float v = 0;
        in >> step >> id >> v;
        if (const auto* spec = params.find(id))
            steps.setLock(*spec, step, v);
        else
            log("CMD lock: unknown parameter %s", id.c_str());
    }
    else if (cmd == "slice") // the selected track's slices (SET SLICE's commands, one undo step each)
        sliceCommand(in);
    else if (cmd == "save-as")
    {
        std::string name;
        in >> name;
        ui::m2::ProjectSource::current().save(ui::m2::SaveTarget::SaveAs, name, [](float) {},
                                              [this](bool ok, std::string)
                                              {
                                                  if (ok) // SAVED is logged by projectLog; then the state as saved
                                                      logState();
                                              });
    }
    else if (cmd == "state")
        logState();
    else if (cmd == "edit-copy" || cmd == "edit-paste")
    {
        // Diagnostics (the SCENE MANAGER's actions without its taps): scene S | pattern S P |
        // track S T [seq] | sequence S P T, 1-based, through the session's SceneEditor (pastes on the workers).
        std::string kind, extra;
        int a = 1, b = 1, c = 1;
        in >> kind >> a;
        namespace ed = core::edit;
        ed::EditRef ref = ed::sceneRef(a - 1);
        if (kind == "pattern" && (in >> b))
            ref = ed::patternRef(a - 1, b - 1);
        else if (kind == "track" && (in >> b))
            ref = ed::trackRef(a - 1, b - 1, (in >> extra) && extra == "seq");
        else if (kind == "sequence" && (in >> b >> c))
            ref = ed::sequenceRef(a - 1, b - 1, c - 1);
        else if (kind != "scene")
        {
            log("CMD %s: unknown kind %s", cmd.c_str(), kind.c_str());
            return;
        }
        auto& editor = ed::sharedEditor(bus);
        const std::string args = text.size() > cmd.size() ? text.substr(cmd.size() + 1) : std::string();
        if (cmd == "edit-copy")
        {
            editor.copy(ref);
            log("EDIT copied %s", args.c_str());
            return;
        }
        editor.paste(ref, [this, what = args](ed::Result r) {
            log("EDIT pasted %s %s%s", what.c_str(), r.ok() ? "ok" : "FAILED ", r.ok() ? "" : r.error().message.c_str());
        });
    }
    else if (cmd == "open") // diagnostics: open a screen by id (`cmd open track.lfo`), logs "UI OPEN id ok|refused"
    {
        std::string id;
        in >> id;
        const bool ok = selection.shell != nullptr && selection.shell->open(id);
        log("UI OPEN %s %s", id.c_str(), ok ? "ok" : "refused");
    }
    else if (cmd == "ribbon") // diagnostics: the strip's message-thread state and the project's items
    {
        const auto& r = core::commands::project(bus).ribbon;
        std::string items;
        for (int u = 0; u < core::commands::ribbon::kSets; ++u)
            for (int i = 0; i < core::commands::ribbon::itemCount(r, u); ++i)
            {
                const auto& it = r.user[static_cast<std::size_t>(u)][static_cast<std::size_t>(i)];
                char buf[96];
                std::snprintf(buf, sizeof buf, "%sU%d.%d=%s:%.3f-%.3f", items.empty() ? "" : ",", u + 1, i + 1,
                              it.target.c_str(), it.bottom, it.top);
                items += buf;
            }
        log("RIBBON STATE mode=%s hold=%d contact=%d position=%d leds=0x%02x pitch=%d..%d items=%s posted=%llu "
            "refused=%llu",
            core::ribbon::modeName(strip.mode()), strip.hold() ? 1 : 0, strip.contact() ? 1 : 0, strip.position(),
            strip.ledMask(), r.pitchBottomCents, r.pitchTopCents, items.empty() ? "-" : items.c_str(),
            static_cast<unsigned long long>(strip.posted()), static_cast<unsigned long long>(strip.refused()));
    }
    else if (cmd == "ribbon-assign" || cmd == "ribbon-range" || cmd == "ribbon-delete" || cmd == "ribbon-pitch")
    {
        // TOUCH STRIP SETTING's edits without its screen: ribbon-assign U S <param id> |
        // ribbon-range U S <bottom 0-1> <top 0-1> | ribbon-delete U S | ribbon-pitch <bottom> <top> (cents); 1-based.
        namespace rb = core::commands::ribbon;
        const auto& p = core::commands::project(bus);
        std::unique_ptr<core::Command> c;
        if (cmd == "ribbon-pitch")
        {
            int lo = 0, hi = 0;
            in >> lo >> hi;
            c = rb::setPitchRange(p, lo, hi);
        }
        else
        {
            int u = 0, slot = 0;
            in >> u >> slot;
            if (cmd == "ribbon-assign")
            {
                std::string id;
                in >> id;
                c = rb::assignItem(p, u - 1, slot - 1, id);
            }
            else if (cmd == "ribbon-range")
            {
                double lo = 0, hi = 1;
                in >> lo >> hi;
                c = rb::setItemRange(p, u - 1, slot - 1, lo, hi);
            }
            else
                c = rb::deleteItem(p, u - 1, slot - 1);
        }
        if (c)
            bus.execute(std::move(c));
        else
            log("CMD %s: nothing to change", text.c_str());
    }
    else if (cmd == "param") // diagnostics: `cmd param <id> <scope> <value>` posts a ParamChange as is
    {                        // (not saved in the project; e.g. routing.outAssign 0 2 = scene 1 track 1 → OUT3-4)
        std::string id;
        int scope = 0;
        float v = 0;
        in >> id >> scope >> v;
        const auto pid = params.idOf(id);
        const bool ok = pid && bridge.post(engine::ParamChange {*pid, static_cast<std::uint16_t>(scope), v, 0});
        log("PARAM %s scope=%d value=%g %s", id.c_str(), scope, static_cast<double>(v), ok ? "posted" : "REFUSED");
    }
    else if (cmd == "import-sp16")
        importReference(text.size() > cmd.size() ? text.substr(cmd.size() + 1) : std::string());
    else if (cmd == "render")
    {
        std::string file, mode;
        double seconds = 10;
        in >> file >> seconds >> mode;
        render(file, seconds, mode == "chain", mode == "song");
    }
    else if (cmd == "render-job" || cmd == "render-cancel" || cmd == "normalize" || cmd == "analyze")
        renderCommand(cmd, in); // diagnostics (the RENDER AUDIO screen's job without its screen)
    else if (cmd.rfind("arr-", 0) == 0 || cmd == "undo" || cmd == "redo" || cmd == "pattern-length")
        arrangementCommand(cmd, in); // diagnostics (the ARRANGER's actions without its screen)
    else if (cmd.rfind("sample-", 0) == 0 || cmd.rfind("live-", 0) == 0)
        samplingCommand(cmd, in); // diagnostics (the SAMPLING / LIVE SAMPLING actions without screens)
    else if (cmd.rfind("ext-", 0) == 0 && extensions && extensions->command(cmd, in))
        ; // extension diagnostics (`cmd ext-flag / ext-state / ext-toggle / ext-learn / ext-unlearn / ext-export`)
    else
        log("CMD unknown: %s", text.c_str());
}

// A reference project (.prj / System/data.rsm) through the sp16-prj codec, as a new native project.
// The path is the rest of the line (it may hold spaces: "PIONEER DJ SAMPLER"); a trailing " copy" copies the samples.
void Document::Impl::importReference(std::string args)
{
    while (! args.empty() && (args.back() == ' ' || args.back() == '\r'))
        args.pop_back();
    bool copy = false;
    if (args.size() > 5 && args.compare(args.size() - 5, 5, " copy") == 0)
    {
        copy = true;
        args.resize(args.size() - 5);
    }
    if (args.rfind("internal:", 0) == 0) // the internal disk's Projects folder
    {
        const juce::File mount(juce::String::fromUTF8(config.internalDisk.c_str()));
        const auto projects = lib::InternalDisk::projects(mount);
        const std::string what = args.substr(9);
        if (what.empty())
        {
            log("IMPORT unit projects=%zu folder=%s", projects.size(),
                lib::InternalDisk::projectsFolder(mount).getFullPathName().toRawUTF8());
            for (std::size_t i = 0; i < projects.size(); ++i)
                log("IMPORT unit project %zu %s %lld", i + 1, projects[i].getFileName().toRawUTF8(),
                    static_cast<long long>(projects[i].getSize()));
            return;
        }
        juce::File chosen;
        if (what.find_first_not_of("0123456789") == std::string::npos) // an index ('#' would start a script comment)
        {
            const long n = std::atol(what.c_str());
            if (n >= 1 && static_cast<std::size_t>(n) <= projects.size())
                chosen = projects[static_cast<std::size_t>(n - 1)];
        }
        else
            chosen = lib::InternalDisk::projectsFolder(mount).getChildFile(juce::String::fromUTF8(what.c_str()));
        if (chosen == juce::File() || ! chosen.existsAsFile())
        {
            log("IMPORT FAILED no unit project '%s' (%zu in %s)", what.c_str(), projects.size(),
                lib::InternalDisk::projectsFolder(mount).getFullPathName().toRawUTF8());
            return;
        }
        args = chosen.getFullPathName().toStdString();
    }
    const juce::File file(juce::String::fromUTF8(args.c_str()));
    if (args.empty() || ! juce::File::isAbsolutePath(juce::String::fromUTF8(args.c_str())) || ! file.existsAsFile())
    {
        log("IMPORT FAILED no file at '%s' (an absolute path is needed)", args.c_str());
        return;
    }
    juce::String name = file.getFileNameWithoutExtension();
    if (file.getFileName().equalsIgnoreCase("data.rsm"))
        name = "Resumed";
    name = juce::String::fromUTF8(store::safeFileName(name.toStdString()).c_str());
    if (store::validateProjectName(name))
        name = "Imported";
    juce::File identity = store::bundleFor(session->projectsDir(), name);
    for (int k = 2; identity.exists() && k < 1000; ++k)
        identity = store::bundleFor(session->projectsDir(), name + " " + juce::String(k));
    services::interop::sp16::setCopyLibrary(copy ? library.root() : juce::File());
    log("IMPORT begin file=%s as=%s samples=%s", file.getFullPathName().toRawUTF8(),
        identity.getFileNameWithoutExtension().toRawUTF8(), copy ? "copy" : "in-place");
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    session->store().openAs(file, identity, importProgress,
                            [this, t0, name = identity.getFileNameWithoutExtension()](core::Result<void> r,
                                                                                         store::LoadReport report)
                            {
                                services::interop::sp16::setCopyLibrary(juce::File());
                                for (const auto& w : report.warnings)
                                    log("IMPORT %s", w.c_str());
                                log("IMPORT done %s project=%s samples=%d loaded=%d missing=%zu ms=%.0f%s%s",
                                    r ? "ok" : "FAILED", name.toRawUTF8(), report.samples, report.samplesLoaded,
                                    report.missing.size(), juce::Time::getMillisecondCounterHiRes() - t0,
                                    r ? "" : " error=", r ? "" : r.error().message.c_str());
                                if (r)
                                    logState();
                            });
}

// One line per pattern with content: what the resume check compares (and what the render's onsets are derived from).
void Document::Impl::logState()
{
    const auto& p = core::commands::project(bus);
    log("M2 STATE project=%s scene=%d pattern=%d tempo=%.1f recorded=%d", session->store().name().toRawUTF8(),
        p.settings.currentScene + 1, p.settings.currentPattern + 1, p.settings.tempo, recordedHits);
    for (int s = 0; s < 16; ++s)
    {
        const auto& scene = p.scenes[static_cast<std::size_t>(s)];
        for (int q = 0; q < 16; ++q)
        {
            const auto& pat = scene.patterns[static_cast<std::size_t>(q)];
            int full = 0, half = 0, param = 0;
            std::string cells;
            for (int st = 0; st < pat.length; ++st)
                for (int t = 0; t < 16; ++t)
                {
                    const auto& c = pat.steps[static_cast<std::size_t>(st)][static_cast<std::size_t>(t)];
                    if (c.trigger == core::model::Step::kEmpty)
                        continue;
                    const char k = c.triggerType == core::model::TriggerType::Full   ? 'F'
                                   : c.triggerType == core::model::TriggerType::Half ? 'H'
                                                                                     : 'P';
                    (k == 'F' ? full : (k == 'H' ? half : param))++;
                    char buf[24];
                    std::snprintf(buf, sizeof buf, "%s%c%d.%d%s", cells.empty() ? "" : ",", k, st + 1, t + 1,
                                  c.lockIndex != core::model::Step::kNoLocks ? "L" : "");
                    cells += buf;
                }
            if (full + half + param == 0)
                continue;
            log("M2 PATTERN scene=%d pattern=%d length=%d bpm=%.1f full=%d half=%d param=%d locks=%zu cells=%s", s + 1,
                q + 1, pat.length, scene.bpm ? *scene.bpm : 0.0, full, half, param, pat.locks.size(), cells.c_str());
        }
    }
}

// Diagnostics: the ARRANGER's row actions without its screen (rows, scenes and patterns 1-based; BPM 0 = "--"):
//   arr-append S P REPEAT BPM | arr-insert ROW S P REPEAT BPM | arr-insert-current ROW | arr-edit ROW S P REPEAT BPM |
//   arr-field ROW scene|pattern|repeat|bpm VALUE | arr-delete ROW | arr-move FROM TO | arr-dup ROW | arr-copy ROW |
//   arr-paste ROW | arr-clear | arr-play 0|1 | arr-start ROW | arr-state; undo | redo; pattern-length S P STEPS.
// ROW = rows + 1 is END OF ARRANGEMENT. Each logs "ARR <cmd> ok" or "ARR <cmd> refused" (nothing to change / invalid).
// Render diagnostics (the RENDER AUDIO screen's actions without the screen):
//   render-job <track|pattern|scene|arrangement> [scene=S] [pattern=P] [track=T] [repeats=N] [bars=B] [bits=16|24|32]
//              [remainder=cut|wrap|leave] [fx=0|1] [normalize=0|1] [target=dBFS] [totrack=0|1] [into=T]
//              defaults = the render.* preferences and the current scene / pattern / selected track (1-based values);
//              logs "RENDER JOB start …", "RENDER JOB progress=…" at each 25 %, "RENDER JOB done …"
//   render-cancel                         cancels the running job
//   normalize <in> <out> [dBFS]           REN-010…013 on a file, logs "NORMALIZE …"
//   analyze <file>                        REN-020/021/024 on a file (decoded through the pool), logs "ANALYZE …"
void Document::Impl::renderCommand(const std::string& cmd, std::istringstream& in)
{
    namespace rnd = services::render;
    if (cmd == "render-cancel")
    {
        log("RENDER JOB cancel id=%u %s", renderJob, renderer && renderer->busy() ? "ok" : "idle");
        if (renderer)
            renderer->cancel(renderJob);
        return;
    }
    if (cmd == "normalize")
    {
        std::string a, b;
        double target = -1.0;
        in >> a >> b >> target;
        const auto r = services::normalize::normalize(juce::File(a), juce::File(b), target);
        if (! r)
            log("NORMALIZE %s REFUSED %s", a.c_str(), r.error().message.c_str());
        else
            log("NORMALIZE %s changed=%d silence=%d atTarget=%d peak=%.0f after=%.0f ceiling=%lld gain=%.9f", a.c_str(),
                r.value().changed ? 1 : 0, r.value().silence ? 1 : 0, r.value().atTarget ? 1 : 0, r.value().peakBefore,
                r.value().peakAfter, static_cast<long long>(r.value().ceiling), r.value().gain);
        return;
    }
    if (cmd == "analyze")
    {
        std::string a;
        in >> a;
        const juce::File file(a);
        pool.requestFile(file, false,
                         [this, file](core::Result<lib::SamplePool::Handle> r, const lib::LoadInfo&)
                         {
                             if (! r)
                             {
                                 log("ANALYZE %s FAILED %s", file.getFullPathName().toRawUTF8(), r.error().message.c_str());
                                 return;
                             }
                             const auto& b = *r.value();
                             const auto meta = lib::SidecarStore().read(file);
                             const auto o = services::analysis::peaks(b);
                             const double seconds = static_cast<double>(b.frames) / b.sampleRate;
                             const bool loop = services::analysis::classify(b, meta) == services::analysis::SampleClass::Loop;
                             log("ANALYZE %s frames=%lld buckets=%zu class=%s bars=%.3f bpm=%.3f", file.getFullPathName().toRawUTF8(),
                                 static_cast<long long>(b.frames), o.min.size(), loop ? "loop" : "oneshot", meta.bars,
                                 meta.bpm > 0 ? meta.bpm : services::analysis::bpmFromBars(meta.bars, seconds));
                         });
        return;
    }
    // render-job
    if (! core::commands::hasProject(bus) || ! renderer)
        return log("RENDER JOB refused: %s", "no project");
    const auto& p = core::commands::project(bus);
    const auto& params = core::builtinParams();
    std::string src, kv;
    in >> src;
    const int selected = selection.shell != nullptr ? (selection.shell->state().selectedTrack - 1) & 15 : 0;
    auto job = rnd::jobFromPreferences(
        [&](std::string_view id)
        {
            const auto* spec = params.find(id);
            return spec != nullptr ? values.get(*spec, 0) : 0.0f;
        },
        p.settings.currentScene & 15, p.settings.currentPattern & 15, selected);
    job.source = src == "track" ? rnd::Source::Track
                                : (src == "scene" ? rnd::Source::Scene
                                                  : (src == "arrangement" ? rnd::Source::Arrangement : rnd::Source::Pattern));
    while (in >> kv)
    {
        const auto eq = kv.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        const double x = std::atof(v.c_str());
        if (k == "scene") job.scene = static_cast<int>(x) - 1;
        else if (k == "pattern") job.pattern = static_cast<int>(x) - 1;
        else if (k == "track") job.track = static_cast<int>(x) - 1;
        else if (k == "repeats") job.repeats = static_cast<int>(x);
        else if (k == "bars") job.bars = x;
        else if (k == "bits") job.bitDepth = static_cast<int>(x);
        else if (k == "remainder") { job.tails = v == "leave"; job.wrapTail = v == "wrap"; }
        else if (k == "fx") job.includeBusFx = x >= 0.5;
        else if (k == "normalize") job.normalize = x >= 0.5;
        else if (k == "target") job.normalizeDbfs = x;
        else if (k == "totrack") job.toTrack = x >= 0.5;
        else if (k == "into") job.targetTrack = static_cast<int>(x) - 1;
    }
    renderProgressLogged = -1;
    renderJob = renderer->start(
        p, job,
        [this](float f)
        {
            const int quarter = static_cast<int>(f * 4.0f);
            if (quarter > renderProgressLogged)
            {
                renderProgressLogged = quarter;
                log("RENDER JOB progress=%d", quarter * 25);
            }
        },
        [this](core::Result<rnd::RenderResult> r)
        {
            if (! r)
                return log("RENDER JOB failed: %s", r.error().message.c_str());
            const auto& x = r.value();
            if (x.cancelled)
                return log("RENDER JOB done cancelled=%d", 1);
            log("RENDER JOB done cancelled=0 file=%s frames=%lld bars=%.3f bpm=%.3f hash=%016llx normalized=%d "
                "into=%d",
                x.file.getFullPathName().toRawUTF8(), static_cast<long long>(x.frames), x.bars, x.bpm,
                static_cast<unsigned long long>(x.hash), x.normalized && x.normalized->changed ? 1 : 0,
                x.assignedTrack + 1);
        });
    if (renderJob == 0)
        return; // refused (its done line says why)
    log("RENDER JOB start id=%u source=%s scene=%d pattern=%d track=%d repeats=%d bits=%d tails=%d wrap=%d fx=%d",
        renderJob, src.c_str(), job.scene + 1, job.pattern + 1, job.track + 1, job.repeats, job.bitDepth,
        job.tails ? 1 : 0, job.wrapTail ? 1 : 0, job.includeBusFx ? 1 : 0);
}

void Document::Impl::arrangementCommand(const std::string& cmd, std::istringstream& in)
{
    namespace ar = core::commands::arrangement;
    const auto& p = core::commands::project(bus);
    auto section = [&in]
    {
        int scene = 1, pattern = 1, repeats = 1;
        double bpm = 0;
        in >> scene >> pattern >> repeats >> bpm;
        core::model::Section s;
        s.scene = static_cast<std::uint8_t>((scene - 1) & 15);
        s.pattern = static_cast<std::uint8_t>((pattern - 1) & 15);
        s.repeats = repeats;
        if (bpm > 0)
            s.fixedBpm = bpm;
        return s;
    };
    std::unique_ptr<core::Command> c;
    bool done = false, handled = true;
    int row = 0, to = 0;
    if (cmd == "arr-append")
        c = ar::appendRow(p, section());
    else if (cmd == "arr-insert" && (in >> row))
        c = ar::insertRowBefore(p, row - 1, section());
    else if (cmd == "arr-insert-current" && (in >> row))
        c = ar::insertCurrentBefore(p, row - 1);
    else if (cmd == "arr-edit" && (in >> row))
        c = ar::editRow(p, row - 1, section());
    else if (cmd == "arr-field" && (in >> row))
    {
        std::string field;
        double v = 0;
        in >> field >> v;
        const auto f = field == "scene" ? ar::RowField::Scene
                       : field == "pattern" ? ar::RowField::Pattern
                       : field == "repeat"  ? ar::RowField::Repeat
                                            : ar::RowField::Bpm;
        c = ar::editRowField(p, row - 1, f, f == ar::RowField::Scene || f == ar::RowField::Pattern ? v - 1 : v);
    }
    else if (cmd == "arr-delete" && (in >> row))
        c = ar::deleteRow(p, row - 1);
    else if (cmd == "arr-move" && (in >> row >> to))
        c = ar::moveRow(p, row - 1, to - 1);
    else if (cmd == "arr-dup" && (in >> row))
        c = ar::duplicateRow(p, row - 1);
    else if (cmd == "arr-copy" && (in >> row))
        done = rowClipboard.copy(p, row - 1);
    else if (cmd == "arr-paste" && (in >> row))
        c = rowClipboard.pasteBefore(p, row - 1);
    else if (cmd == "arr-clear")
        c = ar::clearArrangement(p);
    else if (cmd == "arr-play" && (in >> row))
        c = ar::setPlayArrangement(p, row != 0);
    else if (cmd == "arr-start" && (in >> row))
    {
        if (const auto id = core::builtinParams().idOf("arrangement.start"))
            done = bridge.post(engine::ParamChange {*id, 0, static_cast<float>(row - 1), 0});
    }
    else if (cmd == "arr-state")
    {
        logArrangement();
        return;
    }
    else if (cmd == "undo")
        done = bus.undo();
    else if (cmd == "redo")
        done = bus.redo();
    else if (cmd == "pattern-length")
    {
        int scene = 1, pattern = 1, length = 16;
        in >> scene >> pattern >> length;
        c = core::commands::scene::resizePattern(core::commands::project(bus), scene - 1, pattern - 1, length);
    }
    else
        handled = false;
    if (c)
    {
        bus.execute(std::move(c));
        done = true;
    }
    log("ARR %s %s", cmd.c_str(), ! handled ? "bad arguments" : (done ? "ok" : "refused"));
}

// One line with the rows (S.P xREPEAT @BPM|--), the arrangement plan's start table (sixteenths) and song mode.
void Document::Impl::logArrangement()
{
    const auto& p = core::commands::project(bus);
    core::arrangement::ArrangementPlan plan;
    core::commands::arrangement::buildPlan(p, plan);
    std::string rows, starts;
    for (int i = 0; i < plan.count; ++i)
    {
        const auto& r = plan.rows[static_cast<std::size_t>(i)];
        char buf[48];
        if (r.bpm > 0)
            std::snprintf(buf, sizeof buf, "%s%d.%dx%d@%.1f", rows.empty() ? "" : ",", r.scene + 1, r.pattern + 1,
                          r.repeats, static_cast<double>(r.bpm));
        else
            std::snprintf(buf, sizeof buf, "%s%d.%dx%d@--", rows.empty() ? "" : ",", r.scene + 1, r.pattern + 1,
                          r.repeats);
        rows += buf;
    }
    for (int i = 0; i <= plan.count; ++i)
        starts += (i ? "," : "") + std::to_string(plan.startPpq16[static_cast<std::size_t>(i)]);
    const auto& live = core::arrangement::liveSongStatus();
    log("ARR STATE rows=%d play=%d list=%s starts16=%s live_row=%d", static_cast<int>(plan.count),
        p.settings.playArrangement ? 1 : 0, rows.empty() ? "-" : rows.c_str(), starts.c_str(),
        live.row.load(std::memory_order_relaxed) + 1);
}

namespace
{
bool writeWav16(const std::string& path, const std::vector<float>& l, const std::vector<float>& r, int rate)
{
    std::ofstream out(path, std::ios::binary);
    if (! out)
        return false;
    const std::uint32_t frames = static_cast<std::uint32_t>(l.size()), bytes = frames * 4;
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate) * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    for (std::uint32_t i = 0; i < frames; ++i)
        for (const float v : {l[i], r[i]})
        {
            const float c = std::clamp(v, -1.0f, 1.0f);
            u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(c * 32767.0f))));
        }
    return static_cast<bool>(out);
}
} // namespace

void Document::Impl::render(const std::string& file, double seconds, bool withChain, bool song)
{
    // Pastes still being prepared land first (EDT-004: the render sees the model as the user will hear it).
    for (const double t0 = juce::Time::getMillisecondCounterHiRes();
         core::edit::sharedEditor(bus).busy() && juce::Time::getMillisecondCounterHiRes() - t0 < 10000.0;)
    {
        workers.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // The document's samples, decoded (a load still running lands first, as the open's syncAll would wait for it).
    auto loading = [&] {
        for (int sc = 0; sc < 16; ++sc)
            for (int t = 0; t < 16; ++t)
                if (sync.loading(sc, t))
                    return true;
        return false;
    };
    for (const double t0 = juce::Time::getMillisecondCounterHiRes();
         loading() && juce::Time::getMillisecondCounterHiRes() - t0 < 30000.0;)
    {
        workers.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto& p = core::commands::project(bus);
    const auto& params = core::builtinParams();
    const double rate = config.sampleRate;
    // The render service's offline engine (services/render/OfflineEngine.h): the app's engine, no
    // device, every value of the project posted as on an open (app/ProjectFeed.h), the document's samples by
    // reference. `song`: the arrangement plan too, song mode (stop at the end), its position logged per row.
    core::arrangement::SongStatus songStatus;
    {
        services::render::OfflineEngine offline({rate, 1});
        offline.begin(p, services::render::SampleSnapshot::of(sync), feed::feeder(song), song ? &songStatus : nullptr);
        auto& q = offline.bridge();
        if (song)
        {
            if (const auto id = params.idOf("arrangement.loop"))
                q.post(engine::ParamChange {*id, 0, 0.0f, 0});
            if (const auto id = params.idOf("arrangement.play"))
                q.post(engine::ParamChange {*id, 0, 1.0f, 0});
        }
        const auto queueId = params.idOf("scene.queue").value_or(core::ParamId {});
        q.post(engine::ParamChange {queueId, 0xFFFF, 0.0f, 0}); // scene 1 pattern 1 (stopped: immediate)
        const auto& s1 = p.scenes[0];
        q.post(engine::TransportCmd {engine::TransportCmd::SetTempo, s1.bpm ? *s1.bpm : p.settings.tempo});
        const int preroll = offline.settle();

        // withChain: the chain last built on the PATTERN keys plays instead of the scene queue below.
        const bool chained = withChain && chain.has_value();
        if (chained)
        {
            auto* object = new core::chain::ChainPlan(*chain);
            if (! q.post(engine::ObjectSwap {{engine::SwapSlot::ProjectConfig, core::chain::kSwapScene,
                                              core::chain::kSwapIndex},
                                             object}))
                core::chain::destroyChainPlan(object);
            log("RENDER chain=%s", chainText(*chain).c_str());
        }
        q.post(engine::TransportCmd {engine::TransportCmd::Play, 0.0});
        if (song)
            log("RENDER song rows=%d", core::commands::arrangement::rowCount(p));
        int songRow = -1;
        bool played = false, ended = false;
        const long total = static_cast<long>(seconds * rate);
        std::vector<float> mainL, mainR;
        mainL.reserve(static_cast<std::size_t>(total));
        mainR.reserve(static_cast<std::size_t>(total));
        std::uint64_t hash = 1469598103934665603ull;
        engine::EngineEvent ev;
        for (long done = 0; done < total; done += engine::kBlockFrames)
        {
            if (done == engine::kBlockFrames && ! chained && ! song) // after the first block (at PPQ 0 a PATTERN switch would be immediate)
                q.post(engine::ParamChange {queueId, 0xFFFF, 16.0f, 0}); // scene 2 pattern 1 at the end of the pass
            offline.process();
            const float* l = offline.out(0, 0);
            const float* r = offline.out(0, 1);
            hash = services::render::fnv1a(l, sizeof(float) * engine::kBlockFrames, hash);
            hash = services::render::fnv1a(r, sizeof(float) * engine::kBlockFrames, hash);
            mainL.insert(mainL.end(), l, l + engine::kBlockFrames);
            mainR.insert(mainR.end(), r, r + engine::kBlockFrames);
            if (song)
            {
                // The engine's song position after this block (SectionSequencer::afterSwitch publishes it).
                const int row = songStatus.row.load(std::memory_order_relaxed);
                if (row != songRow && row >= 0)
                    log("RENDER song row=%d pos16=%d remaining=%d block=%ld", row + 1,
                        songStatus.position16.load(std::memory_order_relaxed),
                        songStatus.remaining.load(std::memory_order_relaxed), done);
                songRow = row;
                const bool playing = offline.playhead().playing;
                played = played || playing;
                if (played && ! playing && ! ended)
                {
                    ended = true;
                    log("RENDER song end block=%ld", done); // the first block the transport did not play
                }
            }
            while (offline.popEvent(ev))
                if (ev.kind == engine::EngineEvent::Kind::SwitchDone)
                {
                    const int scene = ev.value / 16;
                    log("RENDER switch scene=%d pattern=%d frame=%ld", scene + 1, ev.value % 16 + 1,
                        done + static_cast<long>(ev.step));
                    if (scene < 3 && ! chained && ! song)
                        q.post(engine::ParamChange {queueId, 0xFFFF, static_cast<float>((scene + 1) * 16), 0});
                }
        }
        const bool written = writeWav16(file, mainL, mainR, static_cast<int>(rate));
        float peak = 0;
        for (const float v : mainL)
            peak = std::max(peak, std::fabs(v));
        log("RENDER file=%s frames=%zu rate=%.0f preroll=%d tempo=%.1f peak=%.4f hash=%016llx%s", file.c_str(),
            mainL.size(), rate, preroll, s1.bpm ? *s1.bpm : p.settings.tempo, static_cast<double>(peak),
            static_cast<unsigned long long>(hash), written ? "" : " (WAV not written)");
    }
}
// Sampling diagnostics (host-smoke CHECK=sampling, scripts/smoke/91-sampling.sh), 1-based tracks; the settings
// (sampling.source / threshold / length, live.*) go through `cmd param`:
//   sample-rec <track> [1|2]   REC on the current scene's track (2 = stereo, 1 = mono source): "SAMPLING REC …"; the
//                              finished take logs "SAMPLING TAKE ok file=… frames=… channels=… rate=… bpm=… bars=… peak=…"
//   sample-stop <track>        REC again (ends an OFF-length take; cancels standby)
//   sample-state               "SAMPLING STATE state=… seq=… start=… end=… head=… peak=… clips=… latency=…"
//   live-arm <track> / live-disarm <track> / live-nudge <frames> / live-save <file.wav>
void Document::Impl::samplingCommand(const std::string& cmd, std::istringstream& in)
{
    if (! sampling)
        return;
    const auto& ph = bridge.playhead();
    const int scene = ph.scene & 15;
    int track = 1, channels = 2;
    if (cmd == "sample-rec" || cmd == "live-arm")
    {
        in >> track >> channels;
        if (! sampling->ensureCapture(ph.bpm > 0 ? ph.bpm : 120.0))
        {
            log("%s", "SAMPLING REC refused: no memory for the capture ring");
            return;
        }
        if (cmd == "live-arm")
        {
            sampling->liveArm(scene, (track - 1) & 15);
            log("LIVE ARM track=%d", track);
            return;
        }
        auto r = sampling->record(scene, (track - 1) & 15, channels == 1 ? 1 : 2);
        if (r)
            log("SAMPLING REC track=%d file=%s", track, r.value().getFullPathName().toRawUTF8());
        else
            log("SAMPLING REC refused: %s (%s)", r.error().message.c_str(), r.error().detail.c_str());
    }
    else if (cmd == "sample-stop" && (in >> track))
    {
        sampling->stopRecord(scene, (track - 1) & 15);
        log("SAMPLING STOP track=%d", track);
    }
    else if (cmd == "live-disarm" && (in >> track))
        sampling->liveDisarm(scene, (track - 1) & 15);
    else if (cmd == "live-nudge")
    {
        int frames = 0;
        in >> frames;
        sampling->setNudge(frames);
    }
    else if (cmd == "live-save")
    {
        std::string file;
        in >> file;
        sampling->saveLive(juce::File(file), [this](core::Result<juce::File> r) {
            if (r)
                log("LIVE SAVE ok file=%s", r.value().getFullPathName().toRawUTF8());
            else
                log("LIVE SAVE failed: %s", r.error().message.c_str());
        });
    }
    else if (cmd == "sample-state")
    {
        const auto* h = sampling->hub();
        if (h == nullptr)
        {
            log("%s", "SAMPLING STATE no-ring");
            return;
        }
        const auto& s = h->status;
        const auto peak = std::max(s.peakL.load(), s.peakR.load());
        log("SAMPLING STATE state=%d seq=%u start=%lld end=%lld head=%lld peak=%.2f clips=%u latency=%d live=%d",
            s.state.load(), s.takeSeq.load(), static_cast<long long>(s.takeStart.load()),
            static_cast<long long>(s.takeEnd.load()), static_cast<long long>(h->ring.head()),
            peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -999.0, s.clips.load(), s.latency.load(),
            s.liveState.load());
    }
    else
        log("CMD %s: bad arguments", cmd.c_str());
}
} // namespace app

// --- Preferences (services/settings/Preferences.h), UTILITY's providers -------------------------------------------
namespace app
{
void Document::Impl::loadPreferences(const juce::File& dataDir)
{
    namespace st = services::settings;
    const auto file = dataDir.getChildFile("preferences.json");
    const auto r = prefs.load(file);
    prefs.setProfilesFolder(dataDir.getChildFile("Controllers")); // the controllers' user profiles (SET-012 export)
    st::install(&prefs);
    // Every stored preference ParamSpec into the shell's values and the engine (pad curve, browse, render, …).
    const int applied = ui::utility::applyPreferences(prefs, recording);
    if (r)
        log("PREFS loaded file=%s values=%zu applied=%d", file.getFullPathName().toRawUTF8(), prefs.values().size(),
            applied);
    else
        log("PREFS reset file=%s: %s (%s)", file.getFullPathName().toRawUTF8(), r.error().message.c_str(),
            r.error().detail.c_str());

    // UTILITY's providers (ui/screens/utility/UtilityEnv.h).
    auto& env = ui::utility::utilityEnv();
    env.version = [] { return std::string(identity::version); };
    env.diskSpace = [this](std::function<void(double)> done)
    {
        auto bytes = std::make_shared<double>(-1.0);
        const juce::File root = library.root();
        workers.run([root, bytes] { *bytes = static_cast<double>(root.getBytesFreeOnVolume()); },
                    [done, bytes] { done(*bytes > 0 ? *bytes : -1.0); }); // statvfs on a worker (UID-887)
    };
    env.audioDevice = [this] { return config.audioDevice.empty() ? std::string("none") : config.audioDevice; };
    env.audioFormat = [this]
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f kHz", config.sampleRate / 1000.0);
        return std::string(buf);
    };
    env.inputs = [] { return std::string("1\xE2\x80\x93" "2"); }; // the codec's stereo input
    env.controllers = [] { return io::controllers::connectedControllers(); }; // UTILITY › CONTROLLERS
    env.keyboard = [device = config.device] { return ! device; }; // imx6 has no keyboard: no KEYBOARD MAP row
    env.libraryLocations = [this]
    {
        std::vector<ui::utility::LibraryLocation> out;
        const auto extra = prefs.getList("library.locations");
        out.push_back({"LIBRARY ROOT", library.root().getFullPathName().toStdString(), false});
        for (const auto& l : library.locations())
        {
            const auto path = l.root.getFullPathName().toStdString();
            if (l.root == library.root())
                continue;
            const bool user = std::find(extra.begin(), extra.end(), path) != extra.end();
            out.push_back({{}, path, user});
        }
        return out;
    };
    env.libraryChanged = [this] { applyLibraryLocations(); };
    env.midiPorts = [] { return io::midi::portNames(); }; // MIDI PORTS, SYNC SOURCE
    applyMidiInputs();
    applyPadPrefs();
}

void Document::Impl::watchPreferences()
{
    applyLibraryLocations();
    prefWatch.push_back(prefs.observe("project.resume", [this](std::string_view)
                                      { session->setResumeEnabled(prefs.get("project.resume") >= 0.5f); }));
    prefWatch.push_back(prefs.observe("settings.autosave",
                                      [this](std::string_view)
                                      {
                                          const bool on = prefs.get("settings.autosave") >= 0.5f;
                                          session->setAutosaveEnabled(on);
                                          session->setAutosaveIdleInterval(autosaveIdleMs(prefs.get("settings.autosave")));
                                          if (const auto* spec = core::builtinParams().find("project.autosave"))
                                              prefAccess.set(*spec, 0, on ? 1.0f : 0.0f);
                                      }));
    prefWatch.push_back(prefs.observe("ui.theme",
                                      [this](std::string_view)
                                      {
                                          if (selection.shell != nullptr)
                                              selection.shell->state().dark = prefs.get("ui.theme") < 0.5f;
                                      }));
    prefWatch.push_back(prefs.observe("library.locations", [this](std::string_view) { applyLibraryLocations(); }));
    prefWatch.push_back(prefs.observe("midi.portIn", [this](std::string_view) { applyMidiInputs(); }));
    for (const char* prefix : {"pads.", "prefs.pads."}) // pads.threshold …, prefs.pads.velocityCurve / fixedVelocity
        prefWatch.push_back(prefs.observe(prefix, [this](std::string_view) { applyPadPrefs(); }));
}

// UTILITY › MACHINE SETTING pad rows: the reference panel's decoder takes PAD CURVE / FIXED VELOCITY /
// THRESHOLD / SENSITIVITY in its reader thread (io/controllers/PanelPads.h); sources that do not shape velocity
// themselves (the virtual panel) get PAD CURVE here; PAD MODE SELECT RESPONSE OVER 1sec = hold a mode key 1 s (PAD-007).
void Document::Impl::applyPadPrefs()
{
    const auto value = [this](std::string_view id)
    {
        const auto* spec = core::builtinParams().find(id);
        return spec != nullptr ? static_cast<int>(std::lround(prefs.get(services::settings::keyOf(*spec)))) : 0;
    };
    // PAD VELOCITY OFF (default): the FIXED curve at 127, so the strength never changes the sample's volume.
    const bool velocity = value("pads.velocity") >= 1;
    const io::PanelPadPrefs p {.curve = velocity ? std::clamp(value("pads.velocityCurve"), 0, 7)
                                                 : static_cast<int>(ui::input::pads::VelocityCurve::Fixed),
                               .fixedVelocity = velocity ? value("pads.fixedVelocity") : 127,
                               .threshold = value("pads.threshold"),
                               .sensitivity = value("pads.sensitivity")};
    const bool overOneSecond = value("pads.modeSelectResponse") >= 1;
    if (padPrefsApplied && p == padPrefs && overOneSecond == padModeHold)
        return;
    padPrefsApplied = true;
    padPrefs = p;
    padModeHold = overOneSecond;
    io::publishPanelPads(p);
    pads.setVelocityCurve(static_cast<ui::input::pads::VelocityCurve>(p.curve), p.fixedVelocity);
    pads.setModeSelectHoldMs(overOneSecond ? 1000.0 : 0.0);
    log("PADS curve=%d fixed=%d threshold=%d sensitivity=%d mode_select=%s", p.curve, p.fixedVelocity, p.threshold,
        p.sensitivity, overOneSecond ? "OVER 1sec" : "IMMEDIATELY");
}

// MIDI PORTS › IN (MID-002): the preference midi.portIn#p → the MIDI service's input slots.
void Document::Impl::applyMidiInputs()
{
    for (int slot = 0; slot < 8; ++slot)
        io::midi::setInputEnabled(slot, prefs.get("midi.portIn#" + std::to_string(slot)) >= 0.5f);
}

// LIBRARY LOCATIONS (settings.md Library "extra locations"): every folder of the preference list is a library location
// "$Ext:<folder name>" (LIB-002); a folder that is not there (an unplugged stick) is skipped until the next start.
void Document::Impl::applyLibraryLocations()
{
    const auto extra = prefs.getList("library.locations");
    std::vector<std::string> gone;
    for (const auto& l : library.locations())
        if (l.alias.rfind("$Ext:", 0) == 0 && ! l.removable &&
            std::find(extra.begin(), extra.end(), l.root.getFullPathName().toStdString()) == extra.end())
            gone.push_back(l.alias);
    for (const auto& alias : gone)
        library.removeLocation(alias);
    for (const auto& path : extra)
    {
        const juce::File folder {juce::String(path)};
        if (folder.isDirectory())
            library.addLocation({"$Ext:" + folder.getFileName().toStdString(), folder, false, false});
    }
}

// `cmd prefs` logs "PREFS <key>=<value> …" (every stored value, sorted) and "PREFS file=… writes=n";
// `cmd pref <key> <value>` sets one (a ParamSpec preference through the shell's ParamAccess, so the engine follows);
// `cmd prefs-flush`, `cmd prefs-export <file>`, `cmd prefs-import <file>` (SET-012).
void Document::Impl::prefsCommand(const std::string& cmd, std::istringstream& in)
{
    namespace st = services::settings;
    if (cmd == "prefs")
    {
        std::string line;
        for (const auto& [k, v] : prefs.values())
        {
            char buf[96];
            std::snprintf(buf, sizeof buf, " %s=%g", k.c_str(), static_cast<double>(v));
            line += buf;
        }
        for (const auto& path : prefs.getList("library.locations"))
            line += " library.locations+=" + path;
        log("PREFS%s", line.empty() ? " (defaults)" : line.c_str());
        log("PREFS file=%s writes=%d dirty=%d", prefs.file().getFullPathName().toRawUTF8(), prefs.writes(),
            prefs.dirty() ? 1 : 0);
    }
    else if (cmd == "pref")
    {
        std::string key;
        float v = 0;
        in >> key >> v;
        const auto& params = core::builtinParams();
        const core::ParamSpec* spec = nullptr;
        for (const auto& p : params.all())
            if (p.storageKey() == key && st::persists(p))
                spec = &p;
        if (spec != nullptr)
            prefAccess.set(*spec, 0, v);
        else
            prefs.set(key, v);
        log("PREFS set %s=%g", key.c_str(), static_cast<double>(prefs.get(key)));
    }
    else if (cmd == "prefs-flush")
    {
        prefs.flush();
        log("PREFS flushed writes=%d", prefs.writes());
    }
    else if (cmd == "prefs-export" || cmd == "prefs-import")
    {
        std::string path;
        in >> path;
        const juce::File f {juce::String(path)};
        const auto r = cmd == "prefs-export" ? prefs.exportTo(f) : prefs.importFrom(f);
        if (r && cmd == "prefs-import")
            ui::utility::applyPreferences(prefs, recording);
        log("PREFS %s %s %s", cmd == "prefs-export" ? "export" : "import", path.c_str(),
            r ? "ok" : r.error().message.c_str());
    }
    else
        log("CMD %s: unknown", cmd.c_str());
}

io::LampBrightness Document::lampBrightness() const
{
    const int v = static_cast<int>(std::lround(impl_->prefs.get("lamps.brightness")));
    return static_cast<io::LampBrightness>(std::clamp(v, 0, 3)); // UI-159, PAD/BUTTON BRIGHTNESS
}
} // namespace app
