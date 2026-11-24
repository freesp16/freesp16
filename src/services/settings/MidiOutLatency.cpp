#include "services/settings/MidiOutLatency.h"

namespace services::settings
{
bool migrateMidiOutLatency(Preferences& p, const core::ParamRegistry& reg)
{
    if (p.has(kMidiOutLatencyKey))
        return false;
    bool reset = false;
    if (const auto* spec = reg.find("midi.outLatency"))
    {
        const auto key = keyOf(*spec, 0);
        if (p.has(key) && static_cast<int>(p.get(key) + 0.5f) == 12) // the old default: the output buffer
        {
            p.reset(key);
            reset = true;
        }
    }
    p.set(kMidiOutLatencyKey, 1.0f);
    return reset;
}
} // namespace services::settings
