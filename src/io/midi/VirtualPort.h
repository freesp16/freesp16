// The "virtual" MIDI provider (our addition): one scripted input port and one recording output port, on every
// platform, for unit tests and the host smoke check (scripts/smoke/93-midi.sh). Enabled in the app by the setting
// `midi_virtual=<script file>` (environment <PREFIX>_MIDI_VIRTUAL); `midi_virtual_out=<file>` writes what the output
// port received at the end (one line per message: the monotonic ns of the write, then the hex bytes).
//
// Script format: one step per line, `#` comments, times in ms since the port opened (absolute, any order):
//   <ms> <hex bytes…>                       raw bytes ("1000 90 05 64" = Note On note 5 velocity 100, channel 1)
//   <ms> clock <bpm> <seconds> [±jitter ms] 24-PPQN clock ticks from <ms> for <seconds>, each tick displaced by a
//                                           uniform random ±jitter (fixed seed: reproducible)
//   <ms> start | stop | continue            FA / FC / FB
//   <ms> spp <sixteenths>                   F2
// The input port's thread replays the steps on time (absolute sleeps), so the stamps carry the real arrival times.
#pragma once

#include "io/midi/MidiPort.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace io::midi
{
struct VirtualScript
{
    struct Bytes
    {
        double atMs;
        std::uint8_t bytes[3];
        int size;
    };
    struct Clock
    {
        double atMs, bpm, seconds, jitterMs;
    };
    std::vector<Bytes> bytes;   // sorted by time
    std::vector<Clock> clocks;
    std::vector<std::string> errors;
};
VirtualScript parseVirtualScript(std::string_view text);

// One output message as the virtual output port saw it.
struct VirtualRecord
{
    std::int64_t atNs;
    std::uint8_t bytes[3];
    std::uint8_t size;
};

class VirtualProvider final : public PortProvider
{
public:
    explicit VirtualProvider(VirtualScript script, std::string outFile = {});
    ~VirtualProvider() override;
    std::string_view id() const override { return "virtual"; }
    std::vector<PortInfo> list() override;
    std::unique_ptr<MidiInput> openInput(const PortInfo&, std::string& error) override;
    std::unique_ptr<MidiOutput> openOutput(const PortInfo&, std::string& error) override;

    // Hot-plug simulation (tests): a detached port disappears from list() and its open handles report "gone".
    void setPresent(bool present) noexcept { present_.store(present); }
    // What the output received so far (copy; any thread).
    std::vector<VirtualRecord> received() const;
    bool writeLog() const; // to outFile (when set)

    struct Shared; // state shared with the open ports

private:
    std::shared_ptr<Shared> shared_;
    std::atomic<bool> present_ {true};
    std::string outFile_;
};
} // namespace io::midi
