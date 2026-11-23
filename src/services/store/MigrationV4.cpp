// Native schema 3 → 4 (Migrations.h). Schema 4 stores a MIDI track's NOTE2–4 (core::model::MidiTrack::notes[1..3]) as
// semitones from NOTE, −127…+127 with 0 = OFF (the reference's encoding, the manual's MIDI NOTE settings), where
// schema 3 meant absolute notes with −1 = OFF. Nothing before schema 4 saved an absolute harmony note: the app's MIDI
// NOTE edits did not reach the model, and the SP-16 importer stored the reference's own offsets (0 read as −1). So the
// only rewrite is −1 → 0 (OFF); an imported offset stays as it is. A track whose "notes" member is absent held the
// schema 3 default [60, −1, −1, −1], which schema 4's default [60, 0, 0, 0] says the same way.
#include "services/store/Migrations.h"

#include <juce_core/juce_core.h>

namespace services::store
{
namespace
{
void toSchema4(juce::var& root)
{
    auto* project = root["project"].getDynamicObject();
    if (project == nullptr)
        return; // nothing to keep (the decoder reports a missing project.json)
    if (const auto* scenes = project->getProperty("scenes").getArray())
        for (const auto& scene : *scenes)
            if (const auto* tracks = scene["tracks"].getArray())
                for (const auto& track : *tracks)
                    if (auto* notes = track["midi"]["notes"].getArray())
                        for (int i = 1; i < notes->size(); ++i)
                            if (auto& n = notes->getReference(i); n.isInt() && static_cast<int>(n) == -1)
                                n = 0; // the same OFF in the new encoding: no migration note
}
} // namespace
} // namespace services::store

APP_REGISTER_MIGRATION({3, 4, &services::store::toSchema4, "MIDI NOTE2-4 are semitones from NOTE, 0 = OFF"})
