// The app's MIDI: a registered io::ControlSource "midi", so the composition root starts it like every
// other source and nothing in app/ changes. start() finds the live engine's hub (core::midi::liveHub), publishes the
// fixed CC table built from the registry, opens every provider's ports (MidiService) and starts the sender; poll()
// (message thread, every frame) runs the 1 s hot-plug pass and drains the engine's UI events. It produces no
// ControlEvents (controller profiles produce those, io/controllers/profiles).
//
// Settings (the app's settings file via io::midi::configure, <PREFIX>_<KEY> environment overrides):
//   midi=0|1                 default 1
//   midi_providers=a,b       providers to use (default: every registered one: alsa-rawmidi on Linux builds)
//   midi_virtual=<script>    add the scripted virtual port (io/midi/VirtualPort.h), first in slot order
//   midi_virtual_out=<file>  write what the virtual output received at the end
//   midi_spin_us=N           the sender's busy-wait before each deadline (default 100)
//   midi_usb=0|1             1 (default): on the unit, put USB-B into USB MIDI mode once if it was left in storage mode
// Log lines ("[sp16-mod] <PREFIX> MIDI …"): the start line (ports, sender), a stats line every 10 s and at stop:
//   MIDI stats in=<messages> sent=<n> clocks=<n> late=<n> dropped=<n> forwarded=<n> jitter_p50=<µs> jitter_p99=<µs>
//   jitter_max=<µs> samples=<n> tempo=<followed BPM> reconnects=<n>
#include "ProjectIdentity.h"
#include "core/params/ParamRegistry.h"
#include "core/util/midi/MidiWire.h"
#include "io/controllers/ControlSurface.h"
#include "io/midi/CcTableBuilder.h"
#include "io/midi/MidiService.h"
#include "io/midi/MidiSettings.h"
#include "io/midi/VirtualPort.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace io::midi
{
namespace
{

void logLine(const std::string& text)
{
    std::printf("[sp16-mod] %s %s\n", identity::envPrefix, text.c_str());
    std::fflush(stdout);
}

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

class MidiSource final : public ControlSource
{
public:
    explicit MidiSource(core::midi::MidiHub& hub) : hub_(hub) {}
    ~MidiSource() override { stop(); }
    std::string_view id() const override { return "midi"; }

    bool start(std::string& info) override
    {
        buildCcTable(core::builtinParams(), table_);
        hub_.setCcTable(&table_);
        std::vector<std::unique_ptr<PortProvider>> providers;
        const std::string script = io::midi::setting("midi_virtual");
        if (! script.empty())
        {
            std::ifstream f(script);
            std::stringstream text;
            text << f.rdbuf();
            auto parsed = parseVirtualScript(text.str());
            for (const auto& e : parsed.errors)
                logLine("MIDI virtual script: " + e);
            providers.push_back(
                std::make_unique<VirtualProvider>(std::move(parsed), io::midi::setting("midi_virtual_out")));
        }
        const std::string wanted = io::midi::setting("midi_providers");
        for (const auto& entry : portProviders())
        {
            if (! wanted.empty() && ("," + wanted + ",").find("," + std::string(entry.id) + ",") == std::string::npos)
                continue;
            if (auto p = entry.create())
                providers.push_back(std::move(p));
        }
        service_ = std::make_unique<MidiService>(hub_, std::move(providers));
        service_->sender().setSpinUs(std::atoi(io::midi::setting("midi_spin_us", "100").c_str()));
        std::string s;
        service_->start(s);
        info = s;
        last_ = nowMs();
        return true;
    }

    void poll(std::vector<ControlEvent>&) override
    {
        if (! service_)
            return;
        const double now = nowMs();
        service_->poll(now);
        publishPorts(); // UTILITY's port names
        drainUiEvents(hub_);
        if (now - last_ >= 10000.0)
        {
            last_ = now;
            report();
        }
    }

    void stop() override
    {
        if (! service_)
            return;
        drainUiEvents(hub_);
        report();
        service_->stop();
        service_.reset();
        hub_.setCcTable(nullptr);
    }

private:
    void report()
    {
        const auto s = service_->sender().stats();
        char buf[400];
        std::snprintf(buf, sizeof buf,
                      "MIDI stats in=%llu sent=%llu clocks=%llu late=%llu dropped=%llu forwarded=%llu jitter_p50=%.0f "
                      "jitter_p99=%.0f jitter_max=%.0f samples=%llu tempo=%.1f reconnects=%llu in_dropped=%llu",
                      static_cast<unsigned long long>(service_->messagesIn()), static_cast<unsigned long long>(s.sent),
                      static_cast<unsigned long long>(s.clocks), static_cast<unsigned long long>(s.late),
                      static_cast<unsigned long long>(s.dropped + hub_.outDropped()),
                      static_cast<unsigned long long>(s.forwarded), s.p50Us, s.p99Us, s.maxUs,
                      static_cast<unsigned long long>(s.samples), lastFollowedTempo(),
                      static_cast<unsigned long long>(service_->reconnects()),
                      static_cast<unsigned long long>(hub_.inDropped()));
        logLine(buf);
    }

    core::midi::MidiHub& hub_;
    core::midi::CcTable table_ {};
    // The slot names for UTILITY (io::midi::portNames), republished only when one changed (no allocation per frame).
    void publishPorts()
    {
        bool changed = false;
        for (int i = 0; i < MidiService::kPorts; ++i)
        {
            // A fixed slot keeps its port's name while the port is absent (the unit's USB in storage mode).
            const auto name = service_->slotName(i);
            if (name != published_[static_cast<std::size_t>(i)])
            {
                published_[static_cast<std::size_t>(i)] = name;
                changed = true;
            }
        }
        if (! changed)
            return;
        std::vector<std::string> names(published_.begin(), published_.end());
        while (! names.empty() && names.back().empty())
            names.pop_back();
        publishPortNames(std::move(names));
    }
    std::array<std::string, MidiService::kPorts> published_ {};
    std::unique_ptr<MidiService> service_;
    double last_ = 0;
};

std::unique_ptr<ControlSource> makeMidiSource(engine::TriggerSink*)
{
    if (io::midi::setting("midi", "1") == "0")
        return nullptr;
    core::midi::MidiHub* hub = core::midi::liveHub();
    if (hub == nullptr)
        return nullptr; // no engine with a MIDI stage in this process
    return std::make_unique<MidiSource>(*hub);
}
} // namespace
} // namespace io::midi

APP_REGISTER_CONTROL_SOURCE("midi", &io::midi::makeMidiSource)
