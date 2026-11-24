// The document side of the composition root (with the ProjectSession wiring): the CommandBus with its model, the
// sample library services, the project session (resume, autosave, missing media), the pad controller, the BROWSE /
// PLAYBACK screens' library / waveform adapters, the effect screens' model slots and the sequencer screens' session
// and project source, wired so that every edit reaches the engine through the bus's bridge hook:
//
//   CommandBus ── bridge hook ──▶ forwardTrackChanges (track fields) · forwardModelChanges (strip, playback, envelope,
//                                 scale, project levels) · forwardFxChanges (FX slots) · TrackSampleSync (samples)
//
// App.cpp builds one after the engine, hands values() to ui::createRoot, attach()es the root, calls startup() once,
// handle() for every control event, frame() once per UI frame (before the root's frame) and shutdown() at quit — then
// destroys the engine before the Document (the engine's destroy functions give back their buffer references first,
// so the pool frees every buffer). Message thread only.
//
// Dev setting dev_kit=<folder> (environment <PREFIX>_DEV_KIT; relative = under the data directory): after start-up the
// folder's first 16 audio files (A→Z) are loaded onto tracks 1–16 of scene 1 with the real LOAD TO = FOLDER commands
// (LIB-013); "KIT ready" is logged once all 16 are installed in the engine (the emulator smoke waits for it).
#pragma once

#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "io/controllers/LampRules.h"

#include <functional>
#include <memory>
#include <string>

namespace ui
{
class ParamAccess;
class Root;
struct UiState;
} // namespace ui

namespace app
{
struct DocumentConfig
{
    std::string dataDir;           // platform::Paths::dataDir
    bool device = false;           // imx6: /proc/mounts volumes, in-app trash
    double sampleRate = 44100;     // the engine's rate (SamplePool resamples to it)
    std::string devKit;            // dev_kit setting ("" = none)
    int devKitScenes = 1;          // dev_kit_scenes: the kit goes onto tracks 1–16 of scenes 1…N (scene/pattern check)
    std::string internalDisk = "/mnt/emmc"; // internal_disk: the reference hardware's internal disk
    std::function<void(const std::string&)> log; // one app log line
    std::string audioDevice;       // the running audio device (UTILITY › AUDIO DEVICE), "" = none
    bool testTone = false;         // test_tone: the engine's 440 Hz tone on an empty track 1 (HOME labels it)
};

class Document
{
public:
    Document(engine::EngineBridge&, DocumentConfig);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    ui::ParamAccess& values() noexcept;
    // RootContext::beforeFrame: the track summaries (HOME, MIXER, context bar) from the model.
    std::function<void(ui::UiState&, double)> summaries();
    void attach(ui::Root&); // the pad controller's selection = the shell's selected track
    void startup();         // resume or new project (ProjectSession::startup), then dev_kit
    // Pads, pad-mode keys, SHIFT, the SCENE / PATTERN modes, the step keys, ERASE (before the root gets the event);
    // true = consumed (the SCENE / PATTERN keys and, in those modes, the step and BAR keys): the root does not get it.
    bool handle(const io::ControlEvent&);
    void frame(double nowMs); // workers, session, pool, preview, notifications, location poll, sequencer / scene sync
    // An app command (the scripted input's `cmd …`, io::takeAppCommand; diagnostics and the scene/pattern check):
    //   scene-bpm <scene 1–16> <bpm|0>       SCENE BPM (setSceneBpm; 0 = none)
    //   trigger-type full|half|param          the step keys' TRIGGER TYPE (seq.triggerType)
    //   lock <step 1–16> <param id> <value>   a step lock on the selected track's shown bar (the hold + turn command)
    //   save-as <name>                        PROJECT SAVE AS through the PROJECT screen's source
    //   state                                 logs "M2 STATE …" (project, selection, every pattern's content)
    //   render <file.wav> <seconds>           offline render of the open project (below), logs "RENDER …"
    //   render-job <source> [k=v …] | render-cancel | normalize <in> <out> [dBFS] | analyze <file>
    //                                         the render service, normalize and analysis (Document.cpp)
    //   open <screen id>                      opens a screen through the shell's router, logs "UI OPEN <id> ok|refused"
    //   ribbon                                logs "RIBBON STATE …" (the touch strip's mode, HOLD, position, items)
    //   ribbon-assign U S <id> | ribbon-range U S <bottom> <top> | ribbon-delete U S | ribbon-pitch <lo> <hi>
    //                                         TOUCH STRIP SETTING's edits (core/commands/ribbon), 1-based
    //   sample-rec <track> [1|2] | sample-stop <track> | sample-state | live-arm <track> | live-disarm <track> |
    //   live-nudge <frames> | live-save <file.wav>  sampling (Document.cpp samplingCommand), logs "SAMPLING …"
    // render: a second engine (no device; services::render::OfflineEngine), every value of the project posted as on an open, scene 1 pattern 1
    // from the top at scene 1's BPM (else the project tempo), scenes 2, 3, 4 queued in turn (each at the previous
    // one's SwitchDone, so each switch lands at the end of a pass); 16-bit stereo MAIN, FNV-1a 64 hash of the float
    // output. Runs on the message thread (blocks it for the render).
    // import-sp16 <file.prj | data.rsm> [copy]   opens a reference project through the sp16-prj codec
    //   (ProjectStore::openAs, the file never written: the project becomes Projects/<name>.sixteen, saved on the next
    //   SAVE); `copy` copies its samples into $Lib/Samples/Imported/<name>/ first, else they are used in place. Logs
    //   one "IMPORT <line>" per report line (FMT-042), then "IMPORT done ok|FAILED …" and the M2 STATE lines.
    //   `internal:<file.prj>` = that file in the internal disk's Projects folder (internal_disk),
    //   `internal:<n>` (digits) = its n-th project (A→Z, 1-based), `internal:` alone = log "IMPORT unit project <n>
    //   <name> <bytes>" for each and import nothing.
    void command(const std::string& text);
    // The touch strip's lamps (8 LEDs, MODE indicators, HOLD) over the shell's lamp frame, before io::apply;
    // the step keys (the selected track's shown bar) or, in a SCENE / PATTERN mode, those keys' lamps.
    void lamps(io::LampFrame&) const;
    void shutdown(bool powerOff = false); // clean exit (keeps the resume state); flushes the preferences;
                                          // powerOff (the power key): unsaved changes are autosaved too
    // PAD/BUTTON BRIGHTNESS (UI-159) for io::apply. Commands (`cmd …`): prefs | pref <key> <value> |
    // prefs-flush | prefs-export <file> | prefs-import <file> (logged "PREFS …").
    io::LampBrightness lampBrightness() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace app
