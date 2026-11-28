// Native schema 3 → 4 (Migrations.h). Schema 4 stores a MIDI track's NOTE2–4 (core::model::MidiTrack::notes[1..3]) as
// semitones from NOTE, −127…+127 with 0 = OFF (the reference's encoding, the manual's MIDI NOTE settings), where
// schema 3 meant absolute notes with −1 = OFF. Nothing before schema 4 saved an absolute harmony note: the app's MIDI
// NOTE edits did not reach the model, and the SP-16 importer stored the reference's own offsets, clamped to −1…127:
// 0 (OFF there) read as −1, a negative offset clamped to −1 — the original value kept in the track's MIDI extras
// ("sp16.MidiNoteData.note2" … "note4", the importer's {"int": n}). So:
//   - an original value in the extras (−127…127) is the offset itself: restored (0 = OFF), whatever the model held;
//   - else −1 → 0 (OFF); an imported positive offset stays as it is.
// Idempotent: the extras win over the model on every run, and a restored −1 always has its extras entry. A track whose
// "notes" member is absent held the schema 3 default [60, −1, −1, −1], which schema 4's default [60, 0, 0, 0] says the
// same way — unless its extras carry an offset, which then makes the array.
#include "services/store/Migrations.h"

#include <juce_core/juce_core.h>

#include <array>
#include <optional>

namespace services::store
{
namespace
{
// The importer's original NOTE2–4 value (`k` 1–3) from the extras of a track's "midi" object, when it is an offset.
std::optional<int> keptOffset(const juce::var& midi, int k)
{
    const auto* extras = midi["_extras"].getArray();
    if (extras == nullptr)
        return std::nullopt;
    const juce::String key = "sp16.MidiNoteData.note" + juce::String(k + 1);
    for (const auto& e : *extras)
    {
        if (e["key"].toString() != key || e["encoding"].toString() != "json")
            continue;
        const auto v = juce::JSON::parse(e["value"].toString());
        const auto& n = v.hasProperty("int") ? v["int"] : v["int64"]; // varToJson: {"int": n} / {"int64": "n"}
        if (! n.isInt() && ! n.isInt64() && ! n.isString())
            return std::nullopt;
        const auto text = n.toString();
        if (! text.containsOnly("-0123456789") || text.isEmpty())
            return std::nullopt;
        const auto offset = text.getLargeIntValue();
        return offset >= -127 && offset <= 127 ? std::optional<int>(static_cast<int>(offset)) : std::nullopt;
    }
    return std::nullopt;
}

void toSchema4(juce::var& root)
{
    auto* project = root["project"].getDynamicObject();
    if (project == nullptr)
        return; // nothing to keep (the decoder reports a missing project.json)
    if (const auto* scenes = project->getProperty("scenes").getArray())
        for (const auto& scene : *scenes)
            if (const auto* tracks = scene["tracks"].getArray())
                for (const auto& track : *tracks)
                {
                    auto* midi = track["midi"].getDynamicObject();
                    if (midi == nullptr)
                        continue;
                    std::array<std::optional<int>, 4> kept{};
                    bool any = false;
                    for (int k = 1; k < 4; ++k)
                        any |= (kept[static_cast<std::size_t>(k)] = keptOffset(track["midi"], k)).value_or(0) != 0;
                    if (! midi->hasProperty("notes") && any)
                        midi->setProperty("notes", juce::Array<juce::var>{60, 0, 0, 0}); // the schema 3 default
                    if (auto* notes = midi->getProperty("notes").getArray())
                        for (int i = 1; i < notes->size() && i < 4; ++i)
                        {
                            auto& n = notes->getReference(i);
                            if (const auto offset = kept[static_cast<std::size_t>(i)])
                                n = *offset; // the reference's own offset, as the importer read it
                            else if (n.isInt() && static_cast<int>(n) == -1)
                                n = 0; // the same OFF in the new encoding: no migration note
                        }
                }
}
} // namespace
} // namespace services::store

APP_REGISTER_MIGRATION({3, 4, &services::store::toSchema4, "MIDI NOTE2-4 are semitones from NOTE, 0 = OFF"})
