// Native schema 1 → 2 (Migrations.h; requested default: 07-ui-design/screens/utility.md §Rows wins over the
// earlier model defaults). Schema 2 changes one default: core::model::MidiPortSettings' clock, startStop and
// songPosition are ENABLE (true) for MIDI IN and OUT; noteCc stays true. A v1 writer omitted those members when they
// held v1's default (false), so this migration writes `false` where a v1 file has no member: an old project keeps
// exactly the MIDI enables it was saved with, and only new projects start with ENABLE.
#include "services/store/ProjectCodec.h"

#include <juce_core/juce_core.h>

namespace services::store
{
namespace
{
juce::var objectAt(juce::DynamicObject& parent, const char* key)
{
    auto v = parent.getProperty(key);
    if (! v.isObject())
    {
        v = juce::var(new juce::DynamicObject());
        parent.setProperty(key, v);
    }
    return v;
}

void midiEnablesDefaultOn(juce::var& root)
{
    auto* project = root.getProperty("project", {}).getDynamicObject();
    if (project == nullptr)
        return; // nothing to keep (the decoder reports a missing project.json)
    auto midi = objectAt(*project, "midi");
    for (const char* dir : {"input", "output"})
    {
        auto port = objectAt(*midi.getDynamicObject(), dir);
        for (const char* key : {"clock", "startStop", "songPosition"})
            if (! port.getDynamicObject()->hasProperty(key))
                port.getDynamicObject()->setProperty(key, false); // v1's default, written out
    }
}
} // namespace
} // namespace services::store

APP_REGISTER_MIGRATION({1, 2, &services::store::midiEnablesDefaultOn,
                        "MIDI IN / OUT CLOCK, START/STOP, SONG POSITION: v1's default (off) kept explicitly"})
