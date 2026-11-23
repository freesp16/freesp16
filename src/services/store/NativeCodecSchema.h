// The native format's schema v1 (internal to services/store; see NativeCodec.h for the file layout). One schema()
// overload per model entity lists its JSON members once; it runs with an Encoder (model → JSON text) or a Decoder
// (parsed JSON → model), so writer and reader cannot drift apart.
//
// Rules of v1 (NativeCodec.h has the user-facing summary):
//   - key = the model field name, except Playback.stretchMode → "stretch", SampleRef.displayName → "name", the
//     location split (FMT-011) and the pattern-file addressing.
//   - sparse fields: a member equal to the default-constructed entity's member (the header defaults) is omitted;
//     an absent member reads as that default. tests/fixtures/native/v1-defaults.json pins those defaults.
//   - enums: the stored string of core/model/enums/EnumStrings.h (aliases accepted); a number reads as the value;
//     an unknown string keeps the default and is kept in the entity's extras as "native.<key>" (Text) and written back.
//   - extras: "native.<key>" JSON entries are written back as <key>, every other entry verbatim in "_extras";
//     on read, unknown keys become "native.<key>" entries (canonical compact JSON). Entities without an extras bag
//     report unknown keys in ImportReport::unmapped. Extras order after a read: unknown enum strings, unknown keys,
//     then "_extras" entries.
#pragma once

#include "core/model/Project.h"
#include "core/model/enums/EnumStrings.h"
#include "core/model/enums/ValueLists.h"
#include "services/store/NativeCodecJson.h"
#include "services/store/ProjectCodec.h"
#include "services/store/SampleRefs.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace services::store::detail
{
namespace m = ::core::model;
using Layout = JsonWriter::Layout;

// Step-lock slot names (sequencer.md "Lockable parameters": the 23 ParamIndex slots, then FX1/FX2 parameters 1–6).
// Format-owned (our choice): a slot is stored by name so a later slot order (OQ-01) does not change files.
inline constexpr std::array<std::string_view, m::kLockSlots> kLockSlotNames {
    "volume",  "start",   "loopStart", "length",  "pitch",          "pan",            "sendAmount",
    "stretch", "loop",    "triggerMode", "mute",  "velocity",       "attack",         "hold",
    "release", "offset",  "retriggerCount", "retriggerSpeed", "lfoSpeed", "lfoDepth", "lfoPhase",
    "reverse", "slice",   "fx1.p1",    "fx1.p2",  "fx1.p3",         "fx1.p4",         "fx1.p5",
    "fx1.p6",  "fx2.p1",  "fx2.p2",    "fx2.p3",  "fx2.p4",         "fx2.p5",         "fx2.p6"};

int lockSlotOf(std::string_view name) noexcept; // -1 = unknown

// Header defaults (one instance each, heap; read-only).
const m::Project& defaultProject();
const m::Pattern& defaultPatternValue();

template <typename T>
inline constexpr bool isModelEnum = std::is_enum_v<T>;

// ---------------------------------------------------------------------------------------------------------- Encoder
class Encoder
{
public:
    Encoder(JsonWriter& w, bool full) : w_(w), full_(full) {}

    JsonWriter& writer() noexcept { return w_; }
    void pushFrame() { frames_.push_back({}); }
    void popFrame() { frames_.pop_back(); }
    void known(std::string_view key) { frames_.back().known.push_back(key); }

    void begin(const m::Extras& extras) { frames_.back().extras = &extras; }
    void begin() {}
    void end();

    template <typename T>
    void field(const char* name, const T& v, const T& d)
    {
        known(name);
        const auto* textEnum = isModelEnum<T> ? unknownEnum(name) : nullptr;
        if (textEnum == nullptr && ! full_ && v == d)
            return;
        w_.key(name);
        if (textEnum != nullptr)
            w_.value(std::string_view(textEnum->value));
        else
            put(v);
    }
    template <typename E>
    void enumInt(const char* name, const int& v, const int& d)
    {
        known(name);
        const auto* textEnum = unknownEnum(name);
        if (textEnum == nullptr && ! full_ && v == d)
            return;
        w_.key(name);
        const auto stored = v >= 0 && v < 256 ? m::storedName(static_cast<E>(v)) : std::string_view {};
        if (textEnum != nullptr)
            w_.value(std::string_view(textEnum->value));
        else if (stored.empty())
            w_.value(v);
        else
            w_.value(stored);
    }
    template <typename T>
    void object(const char* name, const T& v, const T& d, Layout layout = Layout::Inline)
    {
        known(name);
        if (! full_ && v == d)
            return;
        w_.key(name);
        entity(v, d, layout);
    }
    template <typename T>
    void entity(const T& v, const T& d, Layout layout);
    template <typename T, std::size_t N>
    void objects(const char* name, const std::array<T, N>& v, const std::array<T, N>& d, Layout elem = Layout::Inline)
    {
        known(name);
        if (! full_ && v == d)
            return;
        w_.key(name);
        w_.beginArray(elem == Layout::Lines ? Layout::Lines : Layout::Inline);
        for (std::size_t i = 0; i < N; ++i)
            entity(v[i], d[i], elem);
        w_.endArray();
    }
    template <typename T, std::size_t N>
    void objectsUniform(const char* name, const std::array<T, N>& v, const T& d, Layout elem)
    {
        known(name);
        bool all = true;
        for (const auto& e : v)
            all = all && e == d;
        if (! full_ && all)
            return;
        w_.key(name);
        w_.beginArray(Layout::Lines);
        for (const auto& e : v)
            entity(e, d, elem);
        w_.endArray();
    }
    template <typename T>
    void objectVector(const char* name, const std::vector<T>& v, Layout elem = Layout::Inline)
    {
        known(name);
        if (! full_ && v.empty())
            return;
        static const T d {};
        w_.key(name);
        w_.beginArray(Layout::Lines);
        for (const auto& e : v)
            entity(e, d, elem);
        w_.endArray();
    }
    void effectMemory(const char* name, const std::vector<m::EffectSettings>& memory);
    void ribbonUser(const char* name, const std::array<std::array<m::RibbonItem, 8>, 2>& v,
                    const std::array<std::array<m::RibbonItem, 8>, 2>& d);
    void location(const std::string& modelPath); // "location" + "path"
    void steps(const m::Pattern& p);
    void trackFlags(const char* name, const std::array<bool, m::kTracks>& flags);

private:
    struct Frame
    {
        const m::Extras* extras = nullptr;
        std::vector<std::string_view> known;
        std::vector<const m::Extras::Entry*> consumed;
    };
    const m::Extras::Entry* unknownEnum(std::string_view name);

    void put(bool v) { w_.value(v); }
    void put(int v) { w_.value(v); }
    void put(std::int64_t v) { w_.value(v); }
    void put(double v) { w_.value(v); }
    void put(const std::string& v) { w_.value(std::string_view(v)); }
    void put(const std::optional<double>& v)
    {
        if (v)
            w_.value(*v);
        else
            w_.null();
    }
    template <typename E>
        requires std::is_enum_v<E>
    void put(E v)
    {
        const auto stored = m::storedName(v);
        if (stored.empty())
            w_.value(static_cast<int>(v));
        else
            w_.value(stored);
    }
    template <typename T, std::size_t N>
    void put(const std::array<T, N>& a)
    {
        w_.beginArray(Layout::Inline);
        for (const auto& x : a)
            put(x);
        w_.endArray();
    }
    void put(const std::vector<std::int64_t>& a)
    {
        w_.beginArray(Layout::Inline);
        for (const auto x : a)
            w_.value(x);
        w_.endArray();
    }

    JsonWriter& w_;
    bool full_;
    std::vector<Frame> frames_;
};

// ---------------------------------------------------------------------------------------------------------- Decoder
struct DecodeContext
{
    ImportReport& report;
    int warnings = 0;
    void warn(std::string text);
    void unmapped(std::string text);
};

class Decoder
{
public:
    explicit Decoder(DecodeContext& ctx) : ctx_(ctx) {}

    // Starts reading `object` (a JSON object, else a warning and an empty frame); `where` names it in messages.
    bool pushFrame(const juce::var& object, const std::string& where);
    void popFrame();
    const juce::var* get(const char* name); // marks the key as known
    DecodeContext& context() noexcept { return ctx_; }

    void begin(m::Extras& extras) { frames_.back().extras = &extras; }
    void begin() {}
    void end();

    template <typename T>
    void field(const char* name, T& v, const T&)
    {
        if (const auto* value = get(name))
            read(name, *value, v);
    }
    template <typename E>
    void enumInt(const char* name, int& v, const int&)
    {
        const auto* value = get(name);
        if (value == nullptr)
            return;
        if (value->isString())
        {
            const auto text = value->toString().toStdString();
            if (const auto e = m::enumFromStored<E>(text))
                v = static_cast<int>(*e);
            else
                unknownEnum(name, text);
        }
        else
            read(name, *value, v);
    }
    template <typename T>
    void object(const char* name, T& v, const T& d, Layout = Layout::Inline)
    {
        if (const auto* value = get(name))
            entity(*value, v, d, name);
    }
    template <typename T>
    void entity(const juce::var& value, T& v, const T& d, const std::string& where);
    template <typename T, std::size_t N>
    void objects(const char* name, std::array<T, N>& v, const std::array<T, N>& d, Layout = Layout::Inline)
    {
        if (const auto* a = array(name, N))
            for (int i = 0; i < std::min<int>(static_cast<int>(N), a->size()); ++i)
                entity(a->getReference(i), v[static_cast<std::size_t>(i)], d[static_cast<std::size_t>(i)],
                       std::string(name) + "[" + std::to_string(i) + "]");
    }
    template <typename T, std::size_t N>
    void objectsUniform(const char* name, std::array<T, N>& v, const T& d, Layout)
    {
        if (const auto* a = array(name, N))
            for (int i = 0; i < std::min<int>(static_cast<int>(N), a->size()); ++i)
                entity(a->getReference(i), v[static_cast<std::size_t>(i)], d,
                       std::string(name) + "[" + std::to_string(i) + "]");
    }
    template <typename T>
    void objectVector(const char* name, std::vector<T>& v, Layout = Layout::Inline)
    {
        if (const auto* a = array(name, 0))
        {
            static const T d {};
            v.clear();
            v.reserve(static_cast<std::size_t>(a->size()));
            for (int i = 0; i < a->size(); ++i)
            {
                v.emplace_back();
                entity(a->getReference(i), v.back(), d, std::string(name) + "[" + std::to_string(i) + "]");
            }
        }
    }
    void effectMemory(const char* name, std::vector<m::EffectSettings>& memory);
    void ribbonUser(const char* name, std::array<std::array<m::RibbonItem, 8>, 2>& v,
                    const std::array<std::array<m::RibbonItem, 8>, 2>& d);
    void location(std::string& modelPath);
    void steps(m::Pattern& p);
    void trackFlags(const char* name, std::array<bool, m::kTracks>& flags);

private:
    struct Frame
    {
        const juce::DynamicObject* object = nullptr;
        std::size_t pathLength = 0;
        std::uint64_t usedLow = 0;
        std::vector<int> usedHigh;
        m::Extras* extras = nullptr;
        std::vector<m::Extras::Entry> unknownEnums;
    };
    const juce::Array<juce::var>* array(const char* name, std::size_t expected);
    void unknownEnum(const char* name, const std::string& text);
    void warn(const char* name, const std::string& what);

    void read(const char* name, const juce::var& value, bool& v);
    void read(const char* name, const juce::var& value, int& v);
    void read(const char* name, const juce::var& value, std::uint8_t& v);
    void read(const char* name, const juce::var& value, std::int64_t& v);
    void read(const char* name, const juce::var& value, double& v);
    void read(const char* name, const juce::var& value, std::string& v);
    void read(const char* name, const juce::var& value, std::optional<double>& v);
    void read(const char* name, const juce::var& value, std::vector<std::int64_t>& v);
    template <typename E>
        requires std::is_enum_v<E>
    void read(const char* name, const juce::var& value, E& v)
    {
        if (value.isString())
        {
            const auto text = value.toString().toStdString();
            if (const auto e = m::enumFromStored<E>(text))
                v = *e;
            else
                unknownEnum(name, text);
            return;
        }
        int raw = 0;
        read(name, value, raw);
        if (raw >= 0 && static_cast<std::size_t>(raw) < m::enumCount<E>())
            v = static_cast<E>(raw);
        else
            warn(name, "value " + std::to_string(raw) + " out of range");
    }
    template <typename T, std::size_t N>
    void read(const char* name, const juce::var& value, std::array<T, N>& v)
    {
        const auto* a = value.getArray();
        if (a == nullptr)
            return warn(name, "expected an array");
        if (static_cast<std::size_t>(a->size()) != N)
            warn(name, "expected " + std::to_string(N) + " values");
        for (int i = 0; i < std::min<int>(static_cast<int>(N), a->size()); ++i)
            read(name, a->getReference(i), v[static_cast<std::size_t>(i)]);
    }

    DecodeContext& ctx_;
    std::vector<Frame> frames_;
    std::string path_;
};

// ----------------------------------------------------------------------------------------------------------- Schema
// V = Encoder or Decoder; T = the entity type, const for the Encoder.
template <typename V, typename T>
void schema(V& v, T& o, const m::Playback& d)
{
    v.begin(o.extras);
    v.field("sample", o.sample.id, d.sample.id);
    v.field("start", o.start, d.start);
    v.field("loopStart", o.loopStart, d.loopStart);
    v.field("length", o.length, d.length);
    v.field("loop", o.loop, d.loop);
    v.field("reverse", o.reverse, d.reverse);
    v.field("pitchCents", o.pitchCents, d.pitchCents);
    v.field("stretch", o.stretchMode, d.stretchMode);
    v.field("bypass", o.bypass, d.bypass);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::AmpEnvelope& d)
{
    v.begin(o.extras);
    v.field("attack", o.attack, d.attack);
    v.field("hold", o.hold, d.hold);
    v.field("release", o.release, d.release);
    v.field("volumeIndex", o.volumeIndex, d.volumeIndex);
    v.field("sync", o.sync, d.sync);
    v.field("velocitySensitive", o.velocitySensitive, d.velocitySensitive);
    v.field("retrigger", o.retrigger, d.retrigger);
    v.field("attackCurve", o.attackCurve, d.attackCurve);
    v.field("releaseCurve", o.releaseCurve, d.releaseCurve);
    v.field("bypass", o.bypass, d.bypass);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::TrackSequencerSettings& d)
{
    v.begin(o.extras);
    v.field("retriggerCount", o.retriggerCount, d.retriggerCount);
    v.template enumInt<m::RetriggerSpeed>("retriggerSpeed", o.retriggerSpeed, d.retriggerSpeed);
    v.field("offset", o.offset, d.offset);
    v.template enumInt<m::StepShiftArea>("stepShiftArea", o.stepShiftArea, d.stepShiftArea);
    v.field("transpose", o.transpose, d.transpose);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::SliceSettings& d)
{
    v.begin(o.extras);
    v.field("grids", o.grids, d.grids);
    v.field("gridCount", o.gridCount, d.gridCount);
    v.field("windowOffset", o.windowOffset, d.windowOffset);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::PadScale& d)
{
    v.begin(o.extras);
    v.template enumInt<m::ScaleType>("scaleType", o.scaleType, d.scaleType);
    v.field("keyShift", o.keyShift, d.keyShift);
    v.field("windowOffset", o.windowOffset, d.windowOffset);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::MidiScale& d)
{
    v.begin(o.extras);
    v.template enumInt<m::ScaleType>("scaleTypeMidi", o.scaleTypeMidi, d.scaleTypeMidi);
    v.field("scalePosition", o.scalePosition, d.scalePosition);
    v.field("rootNote", o.rootNote, d.rootNote);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::Send& d)
{
    v.begin(o.extras);
    v.field("channel", o.channel, d.channel);
    v.field("amount", o.amount, d.amount);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::Lfo& d)
{
    v.begin(o.extras);
    v.field("speed", o.speed, d.speed);
    v.field("depth", o.depth, d.depth);
    v.field("phaseOffset", o.phaseOffset, d.phaseOffset);
    v.field("destination", o.destination, d.destination);
    v.field("shape", o.shape, d.shape);
    v.field("triggerMode", o.triggerMode, d.triggerMode);
    v.field("triggerOn", o.triggerOn, d.triggerOn);
    v.field("sync", o.sync, d.sync);
    v.field("randomSeedCount", o.randomSeedCount, d.randomSeedCount);
    v.field("bypass", o.bypass, d.bypass);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::MidiCcParam& d)
{
    v.begin();
    v.field("cc", o.cc, d.cc);
    v.field("value", o.value, d.value);
    v.field("active", o.active, d.active);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::MidiTrack& d)
{
    v.begin(o.extras);
    v.field("notes", o.notes, d.notes);
    v.field("velocity", o.velocity, d.velocity);
    v.field("length", o.length, d.length);
    v.field("noteBypass", o.noteBypass, d.noteBypass);
    v.field("pitchBendValue", o.pitchBendValue, d.pitchBendValue);
    v.field("pitchBendActive", o.pitchBendActive, d.pitchBendActive);
    v.field("aftertouchValue", o.aftertouchValue, d.aftertouchValue);
    v.field("aftertouchActive", o.aftertouchActive, d.aftertouchActive);
    v.objects("cc1", o.cc1, d.cc1);
    v.field("cc1Bypass", o.cc1Bypass, d.cc1Bypass);
    v.objects("cc2", o.cc2, d.cc2);
    v.field("cc2Bypass", o.cc2Bypass, d.cc2Bypass);
    v.field("bankMsb", o.bankMsb, d.bankMsb);
    v.field("bankLsb", o.bankLsb, d.bankLsb);
    v.field("program", o.program, d.program);
    v.template enumInt<m::MidiTrackMode>("mode", o.mode, d.mode);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::TrackRouting& d)
{
    v.begin(o.extras);
    v.field("output", o.output, d.output);
    v.end();
}

// One remembered type of a slot (FMT-014, "perType"): the type is the key.
template <typename V, typename T>
void schema(V& v, T& o, const m::EffectSettings& d)
{
    v.begin();
    v.field("type1", o.type1, d.type1);
    v.field("type2", o.type2, d.type2);
    v.field("params", o.params, d.params);
    v.field("sync", o.sync, d.sync);
    v.field("lookAhead", o.lookAhead, d.lookAhead);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::EffectSlot& d)
{
    v.begin(o.extras);
    v.field("type", o.type, d.type);
    v.field("bypass", o.bypass, d.bypass);
    v.field("sync", o.sync, d.sync);
    v.field("lookAhead", o.lookAhead, d.lookAhead);
    v.field("type1", o.type1, d.type1);
    v.field("type2", o.type2, d.type2);
    v.field("params", o.params, d.params);
    v.effectMemory("perType", o.typeMemory);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::Track& d)
{
    v.begin(o.extras);
    v.field("mode", o.mode, d.mode);
    v.field("triggerMode", o.triggerMode, d.triggerMode);
    v.field("chokeGroup", o.chokeGroup, d.chokeGroup);
    v.field("volume", o.volume, d.volume);
    v.field("pan", o.pan, d.pan);
    v.field("mute", o.mute, d.mute);
    v.field("solo", o.solo, d.solo);
    v.field("colour", o.colour, d.colour);
    v.field("instrument", o.instrument, d.instrument);
    v.field("name", o.name, d.name);
    v.object("playback", o.playback, d.playback);
    v.object("ampEnv", o.ampEnv, d.ampEnv);
    v.object("seq", o.seq, d.seq);
    v.object("slices", o.slices, d.slices);
    v.object("padScale", o.padScale, d.padScale);
    v.object("midiScale", o.midiScale, d.midiScale);
    v.objectsUniform("insertFx", o.insertFx, d.insertFx[0], Layout::Inline);
    v.object("send", o.send, d.send);
    v.object("lfo", o.lfo, d.lfo);
    v.object("midi", o.midi, d.midi);
    v.object("routing", o.routing, d.routing);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::SceneRouting& d)
{
    v.begin(o.extras);
    v.field("assignMode", o.assignMode, d.assignMode);
    v.template enumInt<m::HeadphoneCue>("headphoneSource", o.headphoneSource, d.headphoneSource);
    v.field("fxMode", o.fxMode, d.fxMode);
    v.field("afsAssignment", o.afsAssignment, d.afsAssignment);
    v.end();
}

// A scene in project.json (its patterns are in patterns/scene-NN.json).
template <typename V, typename T>
void schema(V& v, T& o, const m::Scene& d)
{
    v.begin(o.extras);
    v.field("name", o.name, d.name);
    v.field("colour", o.colour, d.colour);
    v.field("bpm", o.bpm, d.bpm);
    v.objectsUniform("tracks", o.tracks, d.tracks[0], Layout::Lines);
    v.object("sendFx", o.sendFx, d.sendFx);
    v.object("masterFx", o.masterFx, d.masterFx);
    v.object("routing", o.routing, d.routing);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::Pattern& d)
{
    v.begin(o.extras);
    v.field("length", o.length, d.length);
    v.trackFlags("mutes", o.trackMutes);
    v.trackFlags("solos", o.trackSolos);
    v.steps(o);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::ProjectSettings& d)
{
    v.begin(o.extras);
    v.field("tempo", o.tempo, d.tempo);
    v.field("swing", o.swing, d.swing);
    v.field("patternQuantize", o.patternQuantize, d.patternQuantize);
    v.field("padQuantize", o.padQuantize, d.padQuantize);
    v.field("padStartsSequence", o.padStartsSequence, d.padStartsSequence);
    v.field("masterVolume", o.masterVolume, d.masterVolume);
    v.field("headphoneVolume", o.headphoneVolume, d.headphoneVolume);
    v.field("sendTrackVolume", o.sendTrackVolume, d.sendTrackVolume);
    v.field("monitorVolume", o.monitorVolume, d.monitorVolume);
    v.field("afsEnabled", o.afsEnabled, d.afsEnabled);
    v.field("afsRouteMode", o.afsRouteMode, d.afsRouteMode);
    v.template enumInt<m::AfsSendMode>("afsSendMode", o.afsSendMode, d.afsSendMode);
    v.field("afsSendTrack", o.afsSendTrack, d.afsSendTrack);
    v.field("softLimiter", o.softLimiter, d.softLimiter);
    v.field("playArrangement", o.playArrangement, d.playArrangement);
    v.field("currentScene", o.currentScene, d.currentScene);
    v.field("currentPattern", o.currentPattern, d.currentPattern);
    v.field("formatVersion", o.formatVersion, d.formatVersion);
    v.field("formatMinorVersion", o.formatMinorVersion, d.formatMinorVersion);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::Section& d)
{
    v.begin(o.extras);
    v.field("scene", o.scene, d.scene);
    v.field("pattern", o.pattern, d.pattern);
    v.field("repeats", o.repeats, d.repeats);
    v.field("fixedBpm", o.fixedBpm, d.fixedBpm);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::Arrangement& d)
{
    v.begin(o.extras);
    v.field("length", o.length, d.length);
    v.objectVector("sections", o.sections);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::RibbonItem& d)
{
    v.begin();
    v.field("target", o.target, d.target);
    v.field("top", o.top, d.top);
    v.field("bottom", o.bottom, d.bottom);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::RibbonSettings& d)
{
    v.begin(o.extras);
    v.ribbonUser("user", o.user, d.user);
    v.field("pitchTopCents", o.pitchTopCents, d.pitchTopCents);
    v.field("pitchBottomCents", o.pitchBottomCents, d.pitchBottomCents);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::MidiPortSettings& d)
{
    v.begin();
    v.field("clock", o.clock, d.clock);
    v.field("startStop", o.startStop, d.startStop);
    v.field("noteCc", o.noteCc, d.noteCc);
    v.field("songPosition", o.songPosition, d.songPosition);
    v.field("channels", o.channels, d.channels);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::MidiSettings& d)
{
    v.begin(o.extras);
    v.object("input", o.input, d.input);
    v.object("output", o.output, d.output);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::SampleRef& d)
{
    v.begin(o.extras);
    v.field("id", o.id, d.id);
    v.location(o.path);
    v.field("name", o.displayName, d.displayName);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::SamplePool& d)
{
    (void) d;
    v.begin(o.extras);
    v.objectVector("entries", o.entries);
    v.end();
}

template <typename V, typename T>
void schema(V& v, T& o, const m::ProjectOrigin& d)
{
    v.begin();
    v.field("format", o.format, d.format);
    v.field("file", o.file, d.file);
    v.field("contentId", o.contentId, d.contentId);
    v.field("sourceVersion", o.sourceVersion, d.sourceVersion);
    v.field("sourceMinorVersion", o.sourceMinorVersion, d.sourceMinorVersion);
    v.field("importer", o.importer, d.importer);
    v.end();
}

// project.json below "format": everything but steps and the sample pool. "origin" (schema 3) only for an imported
// project.
template <typename V, typename T>
void schema(V& v, T& o, const m::Project& d)
{
    v.begin(o.extras);
    v.object("origin", o.origin, d.origin);
    v.object("settings", o.settings, d.settings, Layout::Lines);
    v.objectsUniform("scenes", o.scenes, d.scenes[0], Layout::Lines);
    v.object("arrangement", o.arrangement, d.arrangement, Layout::Lines);
    v.object("ribbon", o.ribbon, d.ribbon, Layout::Lines);
    v.object("midi", o.midi, d.midi, Layout::Lines);
    v.end();
}

template <typename T>
void Encoder::entity(const T& v, const T& d, Layout layout)
{
    w_.beginObject(layout);
    pushFrame();
    schema(*this, v, d);
    popFrame();
    w_.endObject();
}

template <typename T>
void Decoder::entity(const juce::var& value, T& v, const T& d, const std::string& where)
{
    if (! pushFrame(value, where))
        return;
    schema(*this, v, d);
    popFrame();
}
} // namespace services::store::detail
