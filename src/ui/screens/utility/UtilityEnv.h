// What UTILITY and its sub-pages show that the UI cannot know itself (07-ui-design/screens/utility.md: INFORMATION,
// AUDIO DEVICE, MIDI PORTS, CONTROLLERS, LIBRARY LOCATIONS): the composition root installs the providers (app/
// Document.cpp); every one may be empty (tests, a platform without MIDI) and the row then shows "—". Message thread.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ui::utility
{
struct LibraryLocation
{
    std::string label, path;
    bool removable = false; // an extra location the user added (LIBRARY LOCATIONS lets it be removed)
};

struct UtilityEnv
{
    std::function<std::string()> version; // "0.1.0 (…)"; empty = identity::version
    // DISK SPACE (UID-887): free bytes of the library volume, measured on a worker; `done` runs on the message
    // thread (bytes < 0 = unknown). Called on entry to UTILITY.
    std::function<void(std::function<void(double bytes)> done)> diskSpace;
    std::function<std::string()> audioDevice;  // the running device ("alsa-direct hw:…"), "" = none
    std::function<std::string()> audioFormat;  // "44.1 kHz" (SAMPLE RATE row)
    std::function<std::string()> inputs;       // INPUT CHANNELS row ("1–2")
    std::function<std::vector<std::string>()> midiPorts; // output port names (MIDI PORTS, SYNC SOURCE)
    std::function<int()> controllers;          // connected controllers (−1 = unknown)
    // A computer keyboard drives this platform (host / linux desktop): the KEYBOARD MAP row is offered. False (the
    // hardware: no keyboard input) hides it; empty = shown.
    std::function<bool()> keyboard;
    std::function<std::vector<LibraryLocation>()> libraryLocations; // root first
    // LIBRARY LOCATIONS edits: the composition root re-reads the "library.locations" preference list.
    std::function<void()> libraryChanged;
};

UtilityEnv& utilityEnv() noexcept;
// Free space as the INFORMATION row shows it: "212.4 GB", "850 MB", "—".
std::string formatBytes(double bytes);
} // namespace ui::utility
