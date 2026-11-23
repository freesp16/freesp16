// Native schema 2 → 3 (Migrations.h). Schema 3 adds the project's "origin" (import provenance, core::model::
// ProjectOrigin) and repairs what the SP-16 importer before importer version 2 saved:
//
//   1. Step cells. That importer read the reference step matrix step-major (record k → step k / 16, track k % 16); the
//      matrix is track-major (record k = track × 64 + step, services/interop/sp16/StepMatrix.h), so every imported
//      cell sits at the wrong (step, track). The cell at model (s, t) came from record k = s × 16 + t and belongs at
//      step k % 64, track k / 64. An imported cell is recognised by what that importer stored, never by position
//      alone: its "trigger" is the record's first field (= k / 64, the true track) and the per-pattern blob
//      "sp16.PatternData.value2" holds the record's second field (= k % 64, the true step). A cell made or re-made in
//      the app has the trigger code 15 / 11 / 8 instead and stays where it is; so do cells of patterns without the
//      blob (made in the app, or imported with nothing triggered). The test is exact except for three positions: an
//      app cell at track 1, step 33 / 45 / 61 (k = 512 / 704 / 960) with code 8 / 11 / 15 matches it too (the blob
//      holds 0 where nothing was imported, and k % 64 = 0 there) and moves to step 1 of track 9 / 12 / 16 — accepted:
//      the blob carries nothing that could tell them apart. A recognised cell whose destination holds a cell
//      that stays is dropped (the app's edit wins) and counted in the migration notes. Cell extras
//      "sp16.step[s].track[t]" move with their cell; extras of cells without a step entry (only the import makes
//      those) move by the same permutation. The blob itself is removed: the record's fields are derived from the
//      position now. A moved cell gets the trigger code of its type, as a fresh import would.
//   2. HOLD / RELEASE locks of moved cells. The old importer scaled the reference's k/127 to 0–128 (round(k·128/127));
//      where the cell's extras keep the original lock word (ordinal 6 HOLD, 7 RELEASE of the built-in map) and the
//      lock still holds what the import made of it, the value becomes k (127 → INFINITY 128).
//   3. Envelope norms (imported projects only, item 4). AmpEnvelope attack / hold / release imported as the reference's
//      k/127 are rewritten as the app's k/128 (a value of the k/127 grid is never on the k/128 grid except 0 and 1, so
//      values set in the app after the import are left alone).
//   4. origin. A project holding "sp16." extras (only the importer writes those) gets origin {format "sp16-prj",
//      importer 1}: the importer version before the field existed.
#include "core/util/ampenv/EnvTime.h"
#include "services/store/Migrations.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace services::store
{
namespace
{
constexpr int kSteps = 64, kTracks = 16, kCells = kSteps * kTracks;
constexpr const char* kValue2Key = "sp16.PatternData.value2";
constexpr int kK127 = 16513;

juce::DynamicObject* obj(const juce::var& v) { return v.getDynamicObject(); }

std::optional<int> intOf(const juce::var& v)
{
    if (v.isInt() || v.isInt64())
        return static_cast<int>(v);
    if (v.isDouble() && std::floor(static_cast<double>(v)) == static_cast<double>(v))
        return static_cast<int>(static_cast<double>(v));
    return std::nullopt;
}

std::optional<std::vector<std::int32_t>> decodeValue2Blob(const juce::String& text)
{
    juce::MemoryOutputStream mo;
    if (! juce::Base64::convertFromBase64(mo, text) || mo.getDataSize() != static_cast<std::size_t>(kCells * 4))
        return std::nullopt;
    const auto* b = static_cast<const std::uint8_t*>(mo.getData());
    std::vector<std::int32_t> out(kCells);
    for (int k = 0; k < kCells; ++k)
        out[static_cast<std::size_t>(k)] = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(b[4 * k]) | static_cast<std::uint32_t>(b[4 * k + 1]) << 8 |
            static_cast<std::uint32_t>(b[4 * k + 2]) << 16 | static_cast<std::uint32_t>(b[4 * k + 3]) << 24);
    return out;
}

bool parseCellKey(const juce::String& key, int& s, int& t)
{
    return std::sscanf(key.toRawUTF8(), "sp16.step[%d].track[%d]", &s, &t) == 2 && s >= 0 && s < kSteps && t >= 0 &&
           t < kTracks && key == juce::String("sp16.step[") + juce::String(s) + "].track[" + juce::String(t) + "]";
}
juce::String cellKey(int s, int t) { return "sp16.step[" + juce::String(s) + "].track[" + juce::String(t) + "]"; }

// The reference trigger code a fresh import gives a cell of this stored type (core ValueLists referenceTriggerCode).
int triggerCodeOf(const juce::var& type)
{
    const auto t = type.toString();
    if (t.equalsIgnoreCase("Half"))
        return 11;
    if (t.equalsIgnoreCase("LockOnly") || t.equalsIgnoreCase("Param"))
        return 8;
    return 15;
}

// The original lock word of ordinal `ordinal` kept in a cell's extras value ({"words":{"7":123,…},…}), if any.
std::optional<std::uint32_t> keptWord(const juce::String& extrasJson, int ordinal)
{
    const auto parsed = juce::JSON::parse(extrasJson);
    const auto& w = parsed["words"][juce::Identifier(juce::String(ordinal))];
    if (w.isInt() || w.isInt64() || w.isDouble())
        return static_cast<std::uint32_t>(static_cast<juce::int64>(w));
    return std::nullopt;
}

// HOLD / RELEASE lock repair (item 2): the old value round(k·128/127) → k (INFINITY for 127).
void repairEnvLocks(juce::DynamicObject& step, const juce::String* extrasJson)
{
    auto* locks = obj(step.getProperty("locks"));
    if (locks == nullptr || extrasJson == nullptr)
        return;
    for (const auto& [name, ordinal] : {std::pair {"hold", 6}, std::pair {"release", 7}})
    {
        if (! locks->hasProperty(name))
            continue;
        const auto word = keptWord(*extrasJson, ordinal);
        if (! word || (*word & 0xffu) != static_cast<std::uint32_t>(ordinal) || (*word >> 30) != 0)
            continue;
        std::uint32_t v = (*word >> 8) & 0x3fffffu;
        const auto value22 = static_cast<std::int32_t>((v & 0x200000u) != 0 ? v - 0x400000u : v);
        if (value22 < 0 || value22 % kK127 != 0 || value22 / kK127 > 127)
            continue;
        const int k = value22 / kK127;
        const double old = std::round(static_cast<double>(k) * 128.0 / 127.0);
        const double now = static_cast<double>(locks->getProperty(name));
        if (now != old)
            continue; // edited in the app since the import
        locks->setProperty(name, k >= 127 ? 128 : k);
    }
}

struct PatternResult
{
    int moved = 0, dropped = 0;
};

// Item 1 for one pattern object; nullopt = not a legacy import (no blob).
std::optional<PatternResult> unscramblePattern(juce::DynamicObject& pattern)
{
    auto* extras = pattern.getProperty("_extras").getArray();
    if (extras == nullptr)
        return std::nullopt;
    int blobAt = -1;
    std::optional<std::vector<std::int32_t>> blob;
    for (int i = 0; i < extras->size(); ++i)
        if ((*extras)[i]["key"].toString() == kValue2Key)
        {
            blobAt = i;
            blob = decodeValue2Blob((*extras)[i]["value"].toString());
            break;
        }
    if (blobAt < 0)
        return std::nullopt;
    extras->remove(blobAt);
    PatternResult result;
    if (! blob)
    {
        if (extras->isEmpty())
            pattern.removeProperty("_extras");
        return result; // a damaged blob: nothing can be recognised; the cells stay
    }

    // Cell extras by (old) position.
    std::map<int, int> extrasAt; // k = s × 16 + t → index in `extras`
    for (int i = 0; i < extras->size(); ++i)
    {
        int s = 0, t = 0;
        if (parseCellKey((*extras)[i]["key"].toString(), s, t))
            extrasAt[s * kTracks + t] = i;
    }

    auto* steps = pattern.getProperty("steps").getArray();
    std::vector<juce::var> stay, move;
    std::vector<int> moveFrom; // old k of each entry of `move`
    std::set<int> occupied;    // new k of the cells that stay (s × 16 + t, model order)
    std::set<int> withEntry;   // old k of every step entry
    if (steps != nullptr)
        for (const auto& e : *steps)
        {
            auto* o = obj(e);
            const auto track = o ? intOf(o->getProperty("track")) : std::nullopt;
            const auto step = o ? intOf(o->getProperty("step")) : std::nullopt;
            if (! track || ! step || *track < 1 || *track > kTracks || *step < 1 || *step > kSteps)
            {
                stay.push_back(e); // the decoder reports it
                continue;
            }
            const int k = (*step - 1) * kTracks + (*track - 1);
            withEntry.insert(k);
            const auto trigger = intOf(o->getProperty("trigger"));
            const bool imported = trigger && *trigger == k / kSteps && (*blob)[static_cast<std::size_t>(k)] == k % kSteps;
            if (imported)
            {
                move.push_back(e);
                moveFrom.push_back(k);
            }
            else
            {
                stay.push_back(e);
                occupied.insert(k);
            }
        }

    juce::Array<juce::var> out;
    for (const auto& e : stay)
        out.add(e);
    std::vector<std::pair<int, int>> renames; // extras index → new k (model order)
    std::vector<int> dropExtras;
    for (std::size_t i = 0; i < move.size(); ++i)
    {
        const int k = moveFrom[i];
        const int ns = k % kSteps, nt = k / kSteps, nk = ns * kTracks + nt;
        auto* o = obj(move[i]);
        const auto it = extrasAt.find(k);
        if (occupied.count(nk) != 0)
        {
            ++result.dropped;
            if (it != extrasAt.end())
                dropExtras.push_back(it->second);
            continue;
        }
        occupied.insert(nk);
        o->setProperty("track", nt + 1);
        o->setProperty("step", ns + 1);
        o->setProperty("trigger", triggerCodeOf(o->getProperty("type")));
        if (it != extrasAt.end())
        {
            const auto text = (*extras)[it->second]["value"].toString();
            repairEnvLocks(*o, &text);
            renames.emplace_back(it->second, nk);
        }
        out.add(move[i]);
        ++result.moved;
    }
    // Extras of cells that have no step entry (only the import writes those): the same permutation.
    for (const auto& [k, index] : extrasAt)
        if (withEntry.count(k) == 0)
        {
            const int nk = (k % kSteps) * kTracks + k / kSteps;
            renames.emplace_back(index, nk);
        }
    // Apply renames; a rename onto a key that stays drops the moved entry.
    std::set<int> keepKeys; // new k of cell extras that do not move
    std::set<int> renamed;
    for (const auto& [index, nk] : renames)
        renamed.insert(index);
    for (const auto& [k, index] : extrasAt)
        if (renamed.count(index) == 0 && std::find(dropExtras.begin(), dropExtras.end(), index) == dropExtras.end())
            keepKeys.insert(k);
    for (const auto& [index, nk] : renames)
    {
        if (keepKeys.count(nk) != 0)
        {
            dropExtras.push_back(index);
            continue;
        }
        keepKeys.insert(nk);
        if (auto* e = obj((*extras)[index]))
            e->setProperty("key", cellKey(nk / kTracks, nk % kTracks));
    }
    std::sort(dropExtras.begin(), dropExtras.end());
    for (auto it = dropExtras.rbegin(); it != dropExtras.rend(); ++it)
        extras->remove(*it);
    if (extras->isEmpty())
        pattern.removeProperty("_extras");
    // Entries in model order (step, then track), as the writer emits them.
    std::stable_sort(out.begin(), out.end(), [](const juce::var& a, const juce::var& b) {
        const auto ka = std::pair(intOf(a["step"]).value_or(0), intOf(a["track"]).value_or(0));
        const auto kb = std::pair(intOf(b["step"]).value_or(0), intOf(b["track"]).value_or(0));
        return ka < kb;
    });
    pattern.setProperty("steps", out);
    return result;
}

// Item 3: k/127 → k/128 (index form of EnvTime.h).
int repairEnvelopeNorms(juce::DynamicObject& ampEnv)
{
    int changed = 0;
    for (const auto& [name, stage] : {std::pair {"attack", core::ampenv::EnvStage::Attack},
                                      std::pair {"hold", core::ampenv::EnvStage::Hold},
                                      std::pair {"release", core::ampenv::EnvStage::Release}})
    {
        const auto& v = ampEnv.getProperty(name);
        if (! (v.isDouble() || v.isInt() || v.isInt64()))
            continue;
        const double x = static_cast<double>(v);
        if (! (x > 0.0 && x < 1.0))
            continue;
        const double k = std::round(x * 127.0);
        if (std::fabs(x * 127.0 - k) > 1e-4) // not a k/127 value (the app's own k/128 values are ≥ 0.0078 away)
            continue;
        const auto index = static_cast<std::uint8_t>(k);
        ampEnv.setProperty(name, static_cast<double>(core::ampenv::storedFromIndex(index)));
        ++changed;
    }
    return changed;
}

bool hasSp16Extras(const juce::var& v)
{
    if (const auto* a = v.getArray())
    {
        for (const auto& e : *a)
            if (hasSp16Extras(e))
                return true;
        return false;
    }
    const auto* o = obj(v);
    if (o == nullptr)
        return false;
    if (const auto* extras = o->getProperty("_extras").getArray())
        for (const auto& e : *extras)
            if (e["key"].toString().startsWith("sp16."))
                return true;
    for (const auto& p : o->getProperties())
        if ((p.value.isObject() || p.value.isArray()) && hasSp16Extras(p.value))
            return true;
    return false;
}

void toSchema3(juce::var& root)
{
    int patterns = 0, moved = 0, dropped = 0, norms = 0;
    if (const auto* files = root["patterns"].getArray())
        for (const auto& file : *files)
            if (const auto* list = file["patterns"].getArray())
                for (const auto& p : *list)
                    if (auto* po = obj(p))
                        if (const auto r = unscramblePattern(*po))
                        {
                            ++patterns;
                            moved += r->moved;
                            dropped += r->dropped;
                        }
    auto* project = obj(root["project"]);
    if (project == nullptr)
        return;
    const bool imported = patterns > 0 || hasSp16Extras(root["project"]) || hasSp16Extras(root["patterns"]);
    if (! imported)
        return; // a project made in the app: nothing to repair, no origin
    if (const auto* scenes = project->getProperty("scenes").getArray())
        for (const auto& scene : *scenes)
            if (const auto* tracks = scene["tracks"].getArray())
                for (const auto& track : *tracks)
                    if (auto* env = obj(track["ampEnv"]))
                        norms += repairEnvelopeNorms(*env);
    if (! project->hasProperty("origin"))
    {
        auto* origin = new juce::DynamicObject();
        origin->setProperty("format", "sp16-prj");
        origin->setProperty("importer", 1);
        project->setProperty("origin", juce::var(origin));
    }
    if (patterns > 0)
        appendMigrationNote(root, "SP-16 import: " + std::to_string(moved) + " step(s) in " + std::to_string(patterns) +
                                      " pattern(s) moved to their track and step" +
                                      (dropped > 0 ? ", " + std::to_string(dropped) +
                                                         " dropped where a step made in the app now sits"
                                                   : std::string()));
    if (norms > 0)
        appendMigrationNote(root, "SP-16 import: " + std::to_string(norms) + " envelope time(s) rescaled (k/127 → k/128)");
}
} // namespace
} // namespace services::store

APP_REGISTER_MIGRATION({2, 3, &services::store::toSchema3,
                        "project origin recorded; SP-16 imports: steps on their own track and step, envelope scale"})
