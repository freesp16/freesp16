// Thread roles (threading.md "Threads"): which CPU cores each long-lived thread may run on and how urgent it is, so
// the display, the file work and the library never take CPU time from the audio engine and MIDI.
//
//   role       default   the thread
//   audio      FIFO 70   the audio device's thread (alsa-direct, the file device, the JUCE fallback's callback)
//   clock      FIFO 80   a MIDI clock sender (tiny, deadline-precise)
//   midi_in    FIFO 75   each MIDI input reader (parse + stamp + enqueue only)
//   panel      FIFO 72   the panel link: pad hits go from it straight to the engine (UI-118)
//   midi_out   FIFO 65   the MIDI sender
//   strip      FIFO 60   the touch strip reader
//   recorder   FIFO 40   the disk recorder, the file device's writer
//   ui         nice 0    the message / UI thread (never renamed here: its name is the process name)
//   worker     nice 10   the library, render and edit workers (below the UI)
//
// The cores come from a map: the platform's default (platform::Platform::tuning().threadMap) and the setting
// thread_map over it, both in the syntax
//   <role>:<cores>[@<n>],…    cores = "*" (every core) or core numbers joined by "+" ("0+3"), or "-" (keep the
//                             inherited cores); n = the FIFO priority (1…99) of a real-time role, the nice value
//                             (−20…19) of the others. Example: "audio:1@75,worker:0+3@15".
// A role the map does not name keeps the cores it inherited from the thread that created it and its default
// priority. Threads this program does not create (the JUCE / X / GL driver threads) inherit the UI thread's cores.
//
// apply() is called once by the thread itself, at its start: it names the thread (≤ 15 characters, as `ps -T`,
// `top -H` and the app's `DIAG thread` lines show it), pins it, and sets SCHED_FIFO (refused without CAP_SYS_NICE:
// the thread stays at normal priority, Applied::realtime says so) or the nice value. Every step is best effort and
// nothing allocates (safe on a thread already marked real-time). configure() runs on the message thread before any
// role thread starts; apply() may run on any thread at any time (the table is atomics).
#pragma once

#include <cstdint>
#include <string>

namespace core::threads
{
enum class Role
{
    Audio,
    Clock,
    MidiIn,
    Panel,
    MidiOut,
    Strip,
    Recorder,
    Ui,
    Worker,
    Count
};

struct RoleSpec
{
    bool realtime = false;   // SCHED_FIFO at `priority`, else SCHED_OTHER at nice `priority`
    int priority = 0;        // FIFO priority (realtime) or nice value
    std::uint32_t cores = 0; // bit n = core n; 0 = keep the inherited cores
};

constexpr std::uint32_t kAllCores = 0xFFFFFFFFu; // "*": every core the system has

const char* roleName(Role) noexcept;
RoleSpec defaultSpec(Role) noexcept;
RoleSpec spec(Role) noexcept; // the configured one

// Back to the defaults, then `map` over them (the platform's map, then the setting, in that order: call once with
// each). False when an entry does not parse: the entries that do are applied, `note` names the others.
void reset() noexcept;
bool configure(const std::string& map, std::string& note);
// "audio:1@70 clock:2@80 … ui:0@nice0 worker:0+3@nice10" ("-" = the inherited cores).
std::string describe();

struct Applied
{
    bool realtime = false; // SCHED_FIFO granted (realtime roles)
    bool pinned = false;   // the cores were set (false: no cores in the map, or refused)
    bool niced = false;    // the nice value was set (normal roles)
};

// The calling thread takes `role`; `name` (null = leave the name) names it, truncated to 15 characters.
Applied apply(Role role, const char* name = nullptr) noexcept;
} // namespace core::threads
