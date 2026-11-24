// Scripted input: a ControlSource that replays timed events from a text file, for automated end-to-end
// checks (scripts/host-smoke.sh) on every platform — the Linux desktop, the host, and later the device for diagnostics.
// Enabled by the setting `input_script=<file>` (environment <PREFIX>_INPUT_SCRIPT); the app hands the path over with
// setInputScriptPath() before it creates the registered control sources. Time 0 is the first poll() (≈ the app's RUN).
//
// Script format: one step per line, `#` starts a comment, blank lines are ignored; times are milliseconds
// since time 0 (absolute, any order: steps are replayed sorted by time, ties in file order).
//   <ms> press <control> [value]     Press (value = velocity, default 100); a pad also goes to the TriggerSink at once
//                                    (UI-118), like a panel press
//   <ms> release <control>           Release
//   <ms> turn <control> <detents>    Turn (signed)
//   <ms> value <control> <value>     Value (absolute: pots, strip)
//   <ms> strip <0–127> | off         the touch strip: touch / move to a position (Value on Control::Strip;
//                                    the first one lands the finger), `off` lifts it (Release)
//   <ms> touch <x> <y>               LCD touch down at a logical 800 × 480 point (Kind::Touch on Control::Screen, 1)
//   <ms> drag <x> <y>                touch moved (2)
//   <ms> release-touch [<x> <y>]     touch released (0), at the last touch point when no point is given
//   <ms> tap <x> <y>                 touch + release-touch 50 ms later
//   <ms> screenshot <file.png>       ask the app for a screenshot after this frame (io::requestScreenshot)
//   <ms> quit                        ask the app to stop (io::requestQuit)
//   <ms> standby                     the power key: the app stops, then powers off (io::requestStandby)
//   <ms> cmd <command> [args…]       an app command (io::requestAppCommand; app/Document.cpp lists them: diagnostics
//                                    and the sequencing checks — save-as, scene BPM, offline render …)
// Controls: the spec names and enum names of io::controlFromName ("pad.5", "Pad5", "enc.1", "sel.push") and the
// short forms "pad5", "step3", "bar2", "knob1"/"enc1" (knob n = enc.n).
#pragma once

#include "io/controllers/ControlSurface.h"

#include <string>
#include <string_view>
#include <vector>

namespace io
{
struct ScriptStep
{
    enum class Op : std::uint8_t { Event, Screenshot, Quit, Standby, Command } op = Op::Event;
    double atMs = 0;
    ControlEvent event {ControlEvent::Kind::Press, Control::Pad1, 0, kSourceVirtual, 0};
    std::string arg {}; // screenshot path / app command
};

struct ScriptParseResult
{
    std::vector<ScriptStep> steps;   // sorted by time (stable)
    std::vector<std::string> errors; // "line 3: unknown control 'pad17'"
};

ScriptParseResult parseInputScript(std::string_view text);
std::optional<Control> scriptControl(std::string_view name) noexcept; // the names above

// The path the registered "script" source reads (empty = no script: the factory creates nothing).
void setInputScriptPath(const std::string& path);
const std::string& inputScriptPath();

class ScriptedInputSource final : public ControlSource
{
public:
    ScriptedInputSource(engine::TriggerSink* triggers, ScriptParseResult script, std::string origin);
    std::string_view id() const override { return "script"; }
    bool start(std::string& info) override;
    void poll(std::vector<ControlEvent>& out) override;
    // poll() with an explicit clock (tests): nowMs on any monotonic millisecond scale, time 0 = the first call.
    void pollAt(double nowMs, std::vector<ControlEvent>& out);
    bool finished() const noexcept { return next_ >= script_.steps.size(); }

private:
    engine::TriggerSink* triggers_;
    ScriptParseResult script_;
    std::string origin_;
    std::size_t next_ = 0;
    double t0_ = 0;
    bool started_ = false;
};
} // namespace io
