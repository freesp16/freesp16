// Native schema 4 → 5 (Migrations.h). Schema 5 stores SET SCALE's scale (PadScale::scaleType,
// MidiScale::scaleTypeMidi) as one of the manual's 22 scales ("Selection of scale", core::model::ScaleType), where
// schema 4 had a 14-scale list of our own. Each old scale becomes the new one with the same notes: MAJOR → Ionian,
// MINOR → Aeolian, MELODIC MINOR → Hawaiian (the same intervals), BLUES → Minor Blues, MAJOR / MINOR PENTA → Major /
// Minor Pentatonic, the modes and WHOLE TONE keep their names. HARMONIC MINOR has no equivalent in the manual's list:
// it becomes Aeolian (one note lower on the 7th; a migration note names each track). The fields hold the stored name
// (or, never written by the app, the list index); an absent field is CHROMATIC in both schemas.
// Schema 5 also names MIDI notes C0…B9 (0–119, the manual's MIDI NOTE settings): NOTE (midi.notes[0]) and SET SCALE
// (MIDI)'s ROOT NOTE (midiScale.rootNote) above 119 become 119 (B9), with a migration note.
#include "services/store/Migrations.h"

#include <juce_core/juce_core.h>

#include <array>
#include <string_view>

namespace services::store
{
namespace
{
struct OldScale
{
    std::string_view stored, now;
};
// Schema 4's list in its index order → the schema 5 stored name.
constexpr std::array<OldScale, 14> kOld = {{{"Chromatic", "Chromatic"},
                                            {"Major", "Ionian"},
                                            {"Minor", "Aeolian"},
                                            {"HarmonicMinor", "Aeolian"},
                                            {"MelodicMinor", "Hawaiian"},
                                            {"Dorian", "Dorian"},
                                            {"Phrygian", "Phrygian"},
                                            {"Lydian", "Lydian"},
                                            {"Mixolydian", "Mixolydian"},
                                            {"Locrian", "Locrian"},
                                            {"MajorPenta", "MajorPentatonic"},
                                            {"MinorPenta", "MinorPentatonic"},
                                            {"Blues", "MinorBlues"},
                                            {"WholeTone", "WholeTone"}}};
constexpr std::size_t kHarmonicMinor = 3;

// The old entry `value` names (stored name, the 1.60 alias CHROMA, or the list index); nullptr = not a schema 4 scale
// (an unknown name the decoder keeps as it is).
const OldScale* oldScale(const juce::var& value) noexcept
{
    if (value.isString())
    {
        const auto text = value.toString();
        if (text == "CHROMA")
            return &kOld[0];
        for (const auto& s : kOld)
            if (text == juce::String(s.stored.data(), s.stored.size()))
                return &s;
        return nullptr;
    }
    if (value.isInt() || value.isInt64())
        if (const auto i = static_cast<int>(value); i >= 0 && i < static_cast<int>(kOld.size()))
            return &kOld[static_cast<std::size_t>(i)];
    return nullptr;
}

void rewrite(juce::var& root, const juce::var& object, const char* field, int scene, int track)
{
    auto* o = object.getDynamicObject();
    if (o == nullptr || ! o->hasProperty(field))
        return;
    const auto* old = oldScale(o->getProperty(field));
    if (old == nullptr)
        return;
    o->setProperty(field, juce::String(old->now.data(), old->now.size()));
    if (old == &kOld[kHarmonicMinor])
        appendMigrationNote(root, "scene " + std::to_string(scene + 1) + " track " + std::to_string(track + 1) + " " +
                                      field + ": HARMONIC MINOR is not in the SP-16's scale list, now AEOLIAN");
}

// A note above B9 (119) → 119 with a note; true when changed. Absent, not a number or in range: unchanged.
bool clampNote(juce::var& root, juce::var& value, const char* field, int scene, int track)
{
    if (! (value.isInt() || value.isInt64() || value.isDouble()) || static_cast<double>(value) <= 119.0)
        return false;
    appendMigrationNote(root, "scene " + std::to_string(scene + 1) + " track " + std::to_string(track + 1) + " " +
                                  field + ": " + value.toString().toStdString() + " is above B9, now B9 (119)");
    value = 119;
    return true;
}

void toSchema5(juce::var& root)
{
    auto* project = root["project"].getDynamicObject();
    if (project == nullptr)
        return; // nothing to keep (the decoder reports a missing project.json)
    const auto* scenes = project->getProperty("scenes").getArray();
    if (scenes == nullptr)
        return;
    for (int s = 0; s < scenes->size(); ++s)
        if (const auto* tracks = (*scenes)[s]["tracks"].getArray())
            for (int t = 0; t < tracks->size(); ++t)
            {
                const auto& track = (*tracks)[t];
                rewrite(root, track["padScale"], "scaleType", s, t);
                rewrite(root, track["midiScale"], "scaleTypeMidi", s, t);
                if (auto* o = track["midiScale"].getDynamicObject(); o != nullptr && o->hasProperty("rootNote"))
                    if (auto v = o->getProperty("rootNote"); clampNote(root, v, "midiScale.rootNote", s, t))
                        o->setProperty("rootNote", v);
                if (auto* notes = track["midi"]["notes"].getArray(); notes != nullptr && ! notes->isEmpty())
                    clampNote(root, notes->getReference(0), "midi.notes[0]", s, t);
            }
}
} // namespace
} // namespace services::store

APP_REGISTER_MIGRATION({4, 5, &services::store::toSchema5, "SET SCALE uses the SP-16's 22 scales; MIDI notes C0…B9"})
