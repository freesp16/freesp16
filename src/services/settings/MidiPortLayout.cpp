#include "services/settings/MidiPortLayout.h"

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
    for (const char* id : {"midi.portMode", "midi.portOffset"})
        if (const auto* spec = reg.find(id))
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
