// Start-up diagnostics and crash reporting, for every platform (Linux parts guarded): the facts a
// first run on the reference hardware needs in its log — kernel, CPUs, C library, memory, storage, sound cards and
// MIDI ports, the audio port map — as `DIAG …` lines, plus a fatal-signal handler that writes a short backtrace.
//
// Nothing here opens a device: the sound cards and MIDI ports are read from /proc/asound and /dev/snd names, the
// storage from /proc/mounts and statvfs. Message thread only (start-up / exit), never the audio thread.
#pragma once

#include "platform/Platform.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app::diag
{
using Log = std::function<void(const std::string&)>; // one line, without the `DIAG ` word

// The system block: kernel, cpu (+ frequency governor), libc, memory, the data dir + free space and file system, the
// FAT mounts, sound cards, PCMs, MIDI ports.
void systemLines(const platform::Paths&, const Log&);

// "MAIN=12/14 OUT3-4=8/10 OUT5-6=4/6 OUT7-8=0/2 PHONES=0/1 FILTERSEND=absent" (a started device's ports()).
std::string describePorts(const platform::AudioDevice&);

// "<mount point> <fs type>" of the mount holding `path` (the longest /proc/mounts prefix), empty when unknown.
std::string mountOf(const std::string& path);

// Free bytes on the file system holding `path` (statvfs), -1 when unknown.
std::int64_t freeBytes(const std::string& path);
// VmRSS / VmHWM of this process in kB from /proc/self/status, -1 without /proc.
long residentKb();
long peakResidentKb();
// Every vfat/msdos/exfat/fuseblk mount point in /proc/mounts (removable sticks and the internal disk).
std::vector<std::string> fatMounts();

// stdout line-buffered (the log file is complete up to the last line after a crash or a pulled power cord).
void lineBufferStdout();

// SIGSEGV / SIGBUS / SIGILL / SIGFPE / SIGABRT: write "<tag> CRASH …" lines (signal, fault address, pc/lr where the
// architecture's context is known, the thread id), a backtrace (backtrace_symbols_fd) and the executable mappings of
// /proc/self/maps to stdout, then die with the default action (exit status = the signal). Only async-signal-safe calls
// on that path (write, open/read/close, backtrace after a warm-up call here). The main thread gets an alternate signal
// stack (a stack overflow still reports). `tag` is copied (≤ 63 characters).
void installCrashHandler(const char* tag);
} // namespace app::diag
