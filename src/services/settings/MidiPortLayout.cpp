#include "services/settings/MidiPortLayout.h"

#include "core/util/midi/MidiWire.h"

#include <cmath>
#include <string>

namespace services::settings
{
namespace
{
// Swaps two stored values (a missing one = the default: the key is removed on the other side).
bool swapKeys(Preferences& p, const std::string& a, const std::string& b)
{
    const bool hasA = p.has(a), hasB = p.has(b);
    if (! hasA && ! hasB)
        return false;
    const float va = p.get(a), vb = p.get(b);
    if (hasB)
        p.set(a, vb);
    else
        p.reset(a);
    if (hasA)
        p.set(b, va);
    else
        p.reset(b);
    return true;
}
} // namespace

bool migrateMidiPortLayout(Preferences& p, const core::ParamRegistry& reg)
{
    if (p.has(kMidiPortLayoutKey))
        return false;
    bool swapped = false;
    if (const auto* spec = reg.find("midi.portMode"))
    {
        // A mode names an input (1 / 3 = A, 2 / 4 = B), and A and B swapped sockets too: every port's merge / THRU
        // source follows its socket (old OUT + MERGE B = merge DIN = new OUT + MERGE A).
        for (int port = 0; port < core::midi::kMaxPorts; ++port)
        {
            const auto key = keyOf(*spec, static_cast<std::uint16_t>(port));
            if (! p.has(key))
                continue;
            const int mode = static_cast<int>(std::lround(p.get(key)));
            const int moved = mode == 1 ? 2 : mode == 2 ? 1 : mode == 3 ? 4 : mode == 4 ? 3 : mode;
            if (moved != mode)
            {
                p.set(key, static_cast<float>(moved));
                swapped = true;
            }
        }
        swapped = swapKeys(p, keyOf(*spec, 0), keyOf(*spec, 1)) || swapped;
        // The USB port now: OUT or OUT + MIDI IN only (manual 1.60 p.12).
        const auto usb = keyOf(*spec, core::midi::kUsbPort);
        if (p.has(usb))
        {
            const int mode = static_cast<int>(std::lround(p.get(usb)));
            if (const int legal = core::midi::legalPortMode(core::midi::kUsbPort, mode); legal != mode)
            {
                p.set(usb, static_cast<float>(legal));
                swapped = true;
            }
        }
    }
    if (const auto* spec = reg.find("midi.portOffset"))
        swapped = swapKeys(p, keyOf(*spec, 0), keyOf(*spec, 1)) || swapped;
    swapped = swapKeys(p, "midi.portIn#0", "midi.portIn#1") || swapped;
    if (const auto* spec = reg.find("midi.syncPort"))
    {
        const auto key = keyOf(*spec, 0);
        if (p.has(key))
        {
            const int port = static_cast<int>(p.get(key) + 0.5f);
            if (port == 1 || port == 2)
            {
                p.set(key, static_cast<float>(3 - port));
                swapped = true;
            }
        }
    }
    p.set(kMidiPortLayoutKey, 1.0f);
    return swapped;
}
} // namespace services::settings
