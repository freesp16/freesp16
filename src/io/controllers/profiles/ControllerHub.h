// The app's MIDI pad controllers (control-surface.md §2.4 UI-130…136, screens.md UI-330): which MIDI input
// ports are controllers, the profile each one runs, its pad response, MIDI learn and the user profiles. One
// process-lifetime object (hub()), registered as the control source "controllers" (a thin proxy: the composition
// root starts, polls and lamps it like every source) and installed as the MIDI ports' InputRouter
// (io/midi/InputRouter.h), so the reader threads hand every message to the port's MidiControlSource first.
//
//   roles (UI-136)  a port is a controller or an instrument input, never both: ROLE AUTO (default) = controller when
//                   a profile's port pattern names it (not the "*" fallback), else instrument; CONTROLLER / INSTRUMENT
//                   set by the user (CONTROLLERS page). A controller port without a matching profile runs Generic
//                   16 + 8 (UI-131). Dev / test override: setting `controllers=<port glob>[:<profile id>],…`
//                   (io::midi::setting, environment <PREFIX>_CONTROLLERS) marks ports as controllers.
//   files           <data dir>/Controllers/ (the folder exported with the preferences, SET-012):
//                   user profiles <id>.json (MIDI learn writes user-<port>.json), and settings.json
//                   {"kind": "controller-settings", "schema": 1, "ports": {<name>: {role, profile, curve, threshold,
//                   sensitivity, fixed, trim[16]}}, "keys": [[code, class, "<control>"], …]} — the per-device pad
//                   response and the keyboard remap (KeyRemap), saved on every change (temp file + rename). As
//                   io may not include services (services::settings::Preferences), these live here.
//   MIDI learn      startLearn(port) arms the port; the next message it sends is captured (and consumed); assign(c)
//                   binds it to logical control c in the port's user profile (created from its current profile),
//                   saves and switches the port to it — it survives a restart (UI-130).
// Log lines ("[sp16-mod] <PREFIX> CTRL …"): ports becoming controllers, profile / role changes, learn, and
//   CTRL stats port=<name> profile=<id> in=<n> consumed=<n> events=<n> triggers=<n> lamps=<n> dropped=<n>
// every 10 s and at stop. Message thread for everything but the InputRouter's reader-thread calls.
#pragma once

#include "engine/EngineApi.h"
#include "io/controllers/ControlSurface.h"
#include "io/controllers/MidiControlSource.h"
#include "io/controllers/PadCurve.h"
#include "io/controllers/profiles/Profile.h"
#include "io/midi/InputRouter.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace io::controllers
{
enum class PortRole : std::uint8_t { Auto, Controller, Instrument };

struct PortSettings
{
    PortRole role = PortRole::Auto;
    std::string profile;          // chosen profile id ("" = by port name)
    PadResponse response {};
    std::array<std::int8_t, 16> trim {};
};

// One port as the CONTROLLERS page shows it.
struct ControllerPort
{
    std::string name;
    bool connected = false, controller = false, hasLamps = false;
    std::string profileId, profileName;
    PortSettings settings;
    int lastVelocity = 0;
};

enum class LearnState : std::uint8_t { Idle, Waiting, Captured };

class ControllerHub final : public midi::InputRouter
{
public:
    static constexpr int kPorts = 8;
    ControllerHub();
    ~ControllerHub() override;

    // ---- set-up (message thread) ----
    void setDataDir(std::string dir);          // "" = nothing persisted (tests); loads user profiles + settings.json
    const std::string& dataDir() const noexcept { return dataDir_; }
    void setTriggers(engine::TriggerSink* t) noexcept { triggers_ = t; }
    void setOverride(std::string spec);        // `controllers=` setting
    std::function<void(const std::string&)> log; // "CTRL …" lines (default: stdout with the app's prefix)

    // ---- the app's frame (message thread) ----
    void install();   // as the MIDI InputRouter
    void uninstall();
    void poll(std::vector<ControlEvent>& out);
    void setLamp(Control, std::uint32_t rgb, std::uint8_t level);
    void flush();
    void report();    // the CTRL stats lines

    // ---- the CONTROLLERS page (message thread) ----
    std::vector<ControllerPort> ports() const;  // every port seen (connected or remembered in settings.json)
    int connectedCount() const noexcept;        // connected controller ports (UTILITY's CONTROLLERS row)
    std::vector<const LoadedProfile*> profiles() const; // built-ins first, then user profiles
    const LoadedProfile* profile(std::string_view id) const;
    void setRole(std::string_view port, PortRole);
    void setProfile(std::string_view port, std::string profileId); // "" = by port name
    void setResponse(std::string_view port, const PadResponse&);
    void setTrim(std::string_view port, int pad0, int trim);
    bool startLearn(std::string_view port);
    void cancelLearn();
    void pollLearn(); // a captured message → Captured (poll() does it every frame; the page too)
    LearnState learnState() const noexcept { return learn_; }
    LearnedMessage learned() const noexcept { return learned_; }
    std::string learnPort() const { return learnPort_; }
    // Binds the captured message to `c` in the port's user profile; false (and why) when it cannot.
    bool assignLearned(Control c, std::string& why);
    MidiControlSource* source(std::string_view port) const; // tests

    // ---- midi::InputRouter ----
    void inputOpened(int slot, std::string_view name) override;
    void inputClosed(int slot) override;
    void outputOpened(int slot, std::string_view name, midi::MidiSender& sender) override;
    void outputClosed(int slot) override;
    void rawBytes(int slot, const std::uint8_t* bytes, int size, std::int64_t hostNs) noexcept override;
    bool message(int slot, const core::midi::MidiEvent& e) noexcept override;

private:
    struct Port
    {
        std::string name;
        PortSettings settings;
        int inSlot = -1, outSlot = -1;
        midi::MidiSender* sender = nullptr;
        std::unique_ptr<MidiControlSource> source;          // active (also in active_[inSlot])
        std::vector<std::unique_ptr<MidiControlSource>> retired; // replaced while the reader thread ran
    };
    Port& portNamed(std::string_view name);
    const Port* findPort(std::string_view name) const;
    bool isController(const Port&) const;
    const LoadedProfile& profileFor(const Port&) const;
    void activate(Port&);    // (re)creates or removes the port's source for its role / profile
    void wireLamps(Port&);
    void loadFiles();
    void saveSettings();
    bool writeFile(const std::string& path, const std::string& text);
    void say(const std::string& line);

    std::string dataDir_;
    engine::TriggerSink* triggers_ = nullptr;
    std::vector<std::pair<std::string, std::string>> override_; // port glob, profile id
    std::vector<LoadedProfile> user_;
    std::vector<std::unique_ptr<Port>> ports_;
    std::array<std::atomic<MidiControlSource*>, kPorts> active_ {};
    LearnState learn_ = LearnState::Idle;
    LearnedMessage learned_ {};
    std::string learnPort_;
    std::uint32_t keysSaved_ = 0;
    double lastReport_ = 0;
    bool installed_ = false;
};

ControllerHub& hub(); // the app's (process lifetime)
// Connected MIDI controllers (UTILITY's CONTROLLERS row: "n connected"); the app's hub.
int connectedControllers() noexcept;
} // namespace io::controllers
