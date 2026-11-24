#include "ui/shell/ModelParamAccess.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/track/TrackCommands.h"
#include "core/model/Project.h"
#include "core/util/ampenv/EnvTime.h"
#include "services/settings/ProjectSettings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace ui
{
// One model field: its ParamSpec id, the path tail (ParamPath grammar), and the conversions. Track fields read
// tracks[scene][track]; project fields ignore the indices.
struct ModelField
{
    std::string_view id;
    bool track;          // Track scope (else Project)
    bool forwardedByTrig; // core/commands/track's six: ui::input::pads::forwardTrackChanges posts them
    float (*get)(const core::model::Project&, int scene, int track) noexcept;
    void (*set)(core::model::Project&, int scene, int track, float v) noexcept;
    bool scene = false; // Scene scope — scenes[scene], the track index is unused
};

namespace
{
using core::model::Project;
using core::model::Track;

const Track& tr(const Project& p, int s, int t) noexcept
{
    return p.scenes[static_cast<std::size_t>(s & 15)].tracks[static_cast<std::size_t>(t & 15)];
}
Track& tr(Project& p, int s, int t) noexcept
{
    return p.scenes[static_cast<std::size_t>(s & 15)].tracks[static_cast<std::size_t>(t & 15)];
}
int toInt(float v) noexcept
{
    return static_cast<int>(std::lround(v));
}
std::uint8_t envIndex(core::ampenv::EnvStage stage, float v) noexcept
{
    return core::ampenv::clampIndex(stage, v);
}

#define APP_TRACK_FIELD(ID, TRIG, EXPR, ASSIGN)                                                                       \
    ModelField                                                                                                         \
    {                                                                                                                  \
        ID, true, TRIG, [](const Project& p, int s, int t) noexcept -> float {                                       \
            const auto& x = tr(p, s, t);                                                                               \
            return static_cast<float>(EXPR);                                                                           \
        },                                                                                                             \
            [](Project& p, int s, int t, float v) noexcept {                                                           \
                auto& x = tr(p, s, t);                                                                                 \
                ASSIGN;                                                                                                \
            },                                                                                                         \
            false                                                                                                      \
    }
#define APP_PROJECT_FIELD(ID, EXPR, ASSIGN)                                                                           \
    ModelField                                                                                                         \
    {                                                                                                                  \
        ID, false, false, [](const Project& p, int, int) noexcept -> float {                                         \
            const auto& x = p.settings;                                                                                \
            return static_cast<float>(EXPR);                                                                           \
        },                                                                                                             \
            [](Project& p, int, int, float v) noexcept {                                                               \
                auto& x = p.settings;                                                                                  \
                ASSIGN;                                                                                                \
            },                                                                                                         \
            false                                                                                                      \
    }
// A scene field (ParamScope::Scene, engine scope = the scene): x = scenes[s].
#define APP_SCENE_FIELD(ID, EXPR, ASSIGN)                                                                             \
    ModelField                                                                                                         \
    {                                                                                                                  \
        ID, false, false, [](const Project& p, int s, int) noexcept -> float {                                       \
            const auto& x = p.scenes[static_cast<std::size_t>(s & 15)];                                                \
            return static_cast<float>(EXPR);                                                                           \
        },                                                                                                             \
            [](Project& p, int s, int, float v) noexcept {                                                             \
                auto& x = p.scenes[static_cast<std::size_t>(s & 15)];                                                  \
                ASSIGN;                                                                                                \
            },                                                                                                         \
            true                                                                                                       \
    }

// ---- THRU BYPASS (MIX-051, routing.thruBypass) ----------------------------------------------------------------------
// The frozen TrackRouting has no field for it, so it lives in the routing's extras as "native.thruBypass" (JSON true;
// absent = OFF, so projects without it are unchanged): the native codec writes it as routing.thruBypass and reads it
// back into the same entry. Saved, undoable and posted like every other routing field.
constexpr std::string_view kThruBypassKey = "native.thruBypass";
float thruBypassOf(const core::model::TrackRouting& r) noexcept
{
    const auto* e = r.extras.find(kThruBypassKey);
    return e != nullptr && e->value == "true" ? 1.0f : 0.0f;
}
void setThruBypass(core::model::TrackRouting& r, float v) noexcept
{
    auto& e = r.extras.entries;
    if (v >= 0.5f)
        r.extras.set(std::string(kThruBypassKey), "true");
    else
        e.erase(std::remove_if(e.begin(), e.end(), [](const auto& x) { return x.key == kThruBypassKey; }), e.end());
}

// ---- MOTION REC (track.motionRec) -----------------------------------------------------------------------------------
// The frozen Track has no field for it: the track's extras hold "native.motionRec" (true; absent = OFF), saved by the
// native codec like THRU BYPASS.
constexpr std::string_view kMotionRecKey = "native.motionRec";
float motionRecOf(const core::model::Track& t) noexcept
{
    const auto* e = t.extras.find(kMotionRecKey);
    return e != nullptr && e->value == "true" ? 1.0f : 0.0f;
}
void setMotionRec(core::model::Track& t, float v) noexcept
{
    auto& e = t.extras.entries;
    if (v >= 0.5f)
        t.extras.set(std::string(kMotionRecKey), "true");
    else
        e.erase(std::remove_if(e.begin(), e.end(), [](const auto& x) { return x.key == kMotionRecKey; }), e.end());
}

// ---- MUTE MODE (track.muteMode) -------------------------------------------------------------------------------------
// Per track, in the track's extras like MOTION REC: "native.muteMode" = 1 (a JSON value: the native codec writes it
// back as the track's "muteMode") for STOP STEPS; absent = STOP SOUND.
constexpr std::string_view kMuteModeKey = "native.muteMode";
float muteModeOf(const core::model::Track& t) noexcept
{
    const auto* e = t.extras.find(kMuteModeKey);
    return e != nullptr && e->value == "1" ? 1.0f : 0.0f;
}
void setMuteMode(core::model::Track& t, float v) noexcept
{
    auto& e = t.extras.entries;
    if (v >= 0.5f)
        t.extras.set(std::string(kMuteModeKey), "1");
    else
        e.erase(std::remove_if(e.begin(), e.end(), [](const auto& x) { return x.key == kMuteModeKey; }), e.end());
}

// ---- LFO (lfo.* in the frozen model's Lfo) --------------------------------------------------------------------------
// The model's enums predate the catalogue: LfoShape has no PULSE25 / PULSE12 and two saws, Lfo::destination is the
// reference format's int. The table keeps the catalogue's values without a model change: SAW = SawDown, PULSE25 /
// PULSE12 = Square with the pulse width in the entity's extras ("native.pulseWidth": 25 / 12 — the native codec writes
// a "native.<key>" entry back as <key> and reads it in again, so it round-trips), and the destination as its ParamSpec
// id in "native.destinationId" (a JSON string; ParamIds are never written to files, lfo.cpp). A model destination
// without that entry (a reference id, an importer's job) reads as NONE. FULL / FULL & HALF = Pad / Both.
constexpr std::string_view kPulseKey = "native.pulseWidth", kDestKey = "native.destinationId";
using core::model::LfoShape;
using core::model::LfoTriggerMode;
using core::model::LfoTriggerOn;

const core::model::Extras::Entry* extra(const core::model::Lfo& l, std::string_view key) noexcept
{
    return l.extras.find(key);
}
void eraseExtra(core::model::Lfo& l, std::string_view key) noexcept
{
    auto& e = l.extras.entries;
    e.erase(std::remove_if(e.begin(), e.end(), [key](const auto& x) { return x.key == key; }), e.end());
}
float lfoShapeOf(const core::model::Lfo& l) noexcept
{
    switch (l.shape)
    {
    case LfoShape::Sine:
        return 0;
    case LfoShape::Triangle:
        return 1;
    case LfoShape::SawUp:
    case LfoShape::SawDown:
        return 2;
    case LfoShape::Square:
        if (const auto* e = extra(l, kPulseKey))
            return e->value == "25" ? 4.0f : (e->value == "12" ? 5.0f : 3.0f);
        return 3;
    case LfoShape::Random:
        return 6;
    }
    return 0;
}
void setLfoShape(core::model::Lfo& l, float v) noexcept
{
    static constexpr LfoShape kShapes[7] = {LfoShape::Sine,   LfoShape::Triangle, LfoShape::SawDown, LfoShape::Square,
                                            LfoShape::Square, LfoShape::Square,   LfoShape::Random};
    const int k = std::clamp(toInt(v), 0, 6);
    l.shape = kShapes[k];
    if (k == 4 || k == 5)
        l.extras.set(std::string(kPulseKey), k == 4 ? "25" : "12");
    else
        eraseExtra(l, kPulseKey);
}
float lfoDestinationOf(const core::model::Lfo& l) noexcept
{
    const auto* e = extra(l, kDestKey);
    if (e == nullptr || e->value.size() < 3 || e->value.front() != '"')
        return -1;
    const std::string_view id(e->value.data() + 1, e->value.size() - 2); // the JSON string without its quotes
    static std::string lastId;                                         // per-frame reads: one lookup per change
    static float lastValue = -1;
    if (id != lastId)
    {
        const auto pid = core::builtinParams().idOf(id);
        lastId.assign(id);
        lastValue = pid ? static_cast<float>(pid->value) : -1.0f;
    }
    return lastValue;
}
void setLfoDestination(core::model::Lfo& l, float v) noexcept
{
    const auto& reg = core::builtinParams();
    const int k = toInt(v);
    if (k < 0 || static_cast<std::size_t>(k) >= reg.size())
    {
        eraseExtra(l, kDestKey);
        return;
    }
    l.extras.set(std::string(kDestKey), "\"" + std::string(reg[core::ParamId{static_cast<std::uint16_t>(k)}].id) + "\"");
}

using core::ampenv::EnvStage;
using core::model::EnvelopeCurve;
using core::model::StretchMode;
using core::model::TrackMode;
using core::model::TriggerMode;

constexpr int kPitchCentsPerUnit = 100; // playback.pitch is in semitones

const ModelField kFields[] = {
    APP_TRACK_FIELD("fx1.plugin.mix", false, x.insertFx[0].params[5], x.insertFx[0].params[5] = v),
    APP_TRACK_FIELD("fx2.plugin.mix", false, x.insertFx[1].params[5], x.insertFx[1].params[5] = v),
    APP_SCENE_FIELD("send.plugin.mix", x.sendFx.params[5], x.sendFx.params[5] = v),
    APP_SCENE_FIELD("master.plugin.mix", x.masterFx.params[5], x.masterFx.params[5] = v),
    // MIXER strip
    APP_TRACK_FIELD("track.volume", false, x.volume, x.volume = toInt(v)),
    APP_TRACK_FIELD("track.pan", false, x.pan, x.pan = toInt(v)),
    APP_TRACK_FIELD("track.sendAmount", false, x.send.amount, x.send.amount = toInt(v)),
    APP_TRACK_FIELD("track.solo", false, x.solo ? 1 : 0, x.solo = v >= 0.5f),
    APP_TRACK_FIELD("track.sendChannel", false, x.send.channel, x.send.channel = toInt(v)),
    // TRACK (forwarded by forwardTrackChanges)
    APP_TRACK_FIELD("track.mute", true, x.mute ? 1 : 0, x.mute = v >= 0.5f),
    APP_TRACK_FIELD("track.mode", true, static_cast<int>(x.mode),
                    x.mode = static_cast<TrackMode>(std::clamp(toInt(v), 0, 4))),
    APP_TRACK_FIELD("track.triggerMode", true, static_cast<int>(x.triggerMode),
                    x.triggerMode = static_cast<TriggerMode>(toInt(v) & 1)),
    APP_TRACK_FIELD("track.chokeGroup", true, x.chokeGroup, x.chokeGroup = toInt(v)),
    APP_TRACK_FIELD("track.padColor", true, x.colour, x.colour = toInt(v) & 15),
    APP_TRACK_FIELD("track.padVelocity", true, x.ampEnv.velocitySensitive ? 1 : 0,
                    x.ampEnv.velocitySensitive = v >= 0.5f),
    // SCALE
    APP_TRACK_FIELD("scale.type", false, x.padScale.scaleType, x.padScale.scaleType = toInt(v)),
    APP_TRACK_FIELD("scale.keyShift", false, x.padScale.keyShift, x.padScale.keyShift = toInt(v)),
    APP_TRACK_FIELD("scale.area", false, x.padScale.windowOffset, x.padScale.windowOffset = toInt(v)),
    APP_TRACK_FIELD("scaleMidi.type", false, x.midiScale.scaleTypeMidi, x.midiScale.scaleTypeMidi = toInt(v)),
    APP_TRACK_FIELD("scaleMidi.root", false, x.midiScale.rootNote, x.midiScale.rootNote = toInt(v)),
    APP_TRACK_FIELD("scaleMidi.area", false, x.midiScale.scalePosition, x.midiScale.scalePosition = toInt(v)),
    // PLAYBACK
    APP_TRACK_FIELD("playback.pitch", false, static_cast<double>(x.playback.pitchCents) / kPitchCentsPerUnit,
                    x.playback.pitchCents = static_cast<int>(std::lround(v * kPitchCentsPerUnit))),
    APP_TRACK_FIELD("playback.timeStretch", false, static_cast<int>(x.playback.stretchMode),
                    x.playback.stretchMode =
                        static_cast<StretchMode>(toInt(v) < 0 ? 0 : (toInt(v) > 2 ? 2 : toInt(v)))),
    APP_TRACK_FIELD("playback.loop", false, x.playback.loop ? 1 : 0, x.playback.loop = v >= 0.5f),
    APP_TRACK_FIELD("playback.reverse", false, x.playback.reverse ? 1 : 0, x.playback.reverse = v >= 0.5f),
    APP_TRACK_FIELD("playback.bypass", false, x.playback.bypass ? 1 : 0, x.playback.bypass = v >= 0.5f),
    APP_TRACK_FIELD("playback.start", false, x.playback.start, x.playback.start = std::llround(v)),
    APP_TRACK_FIELD("playback.loopStart", false, x.playback.loopStart, x.playback.loopStart = std::llround(v)),
    APP_TRACK_FIELD("playback.length", false, x.playback.length, x.playback.length = std::llround(v)),
    // AMP ENVELOPE: display index ↔ normalised storage (ENV-001)
    APP_TRACK_FIELD("env.velocity", false, x.ampEnv.volumeIndex, x.ampEnv.volumeIndex = toInt(v)),
    APP_TRACK_FIELD(
        "env.attack", false,
        core::ampenv::clampIndex(EnvStage::Attack, core::ampenv::indexFromStored(static_cast<float>(x.ampEnv.attack))),
        x.ampEnv.attack = static_cast<double>(core::ampenv::storedFromIndex(envIndex(EnvStage::Attack, v)))),
    APP_TRACK_FIELD("env.hold", false, core::ampenv::indexFromStored(static_cast<float>(x.ampEnv.hold)),
                    x.ampEnv.hold = static_cast<double>(core::ampenv::storedFromIndex(envIndex(EnvStage::Hold, v)))),
    APP_TRACK_FIELD("env.release", false, core::ampenv::indexFromStored(static_cast<float>(x.ampEnv.release)),
                    x.ampEnv.release =
                        static_cast<double>(core::ampenv::storedFromIndex(envIndex(EnvStage::Release, v)))),
    APP_TRACK_FIELD("env.sync", false, x.ampEnv.sync ? 1 : 0, x.ampEnv.sync = v >= 0.5f),
    APP_TRACK_FIELD("env.attackCurve", false, static_cast<int>(x.ampEnv.attackCurve),
                    x.ampEnv.attackCurve = static_cast<EnvelopeCurve>(toInt(v) & 1)),
    APP_TRACK_FIELD("env.releaseCurve", false, static_cast<int>(x.ampEnv.releaseCurve),
                    x.ampEnv.releaseCurve = static_cast<EnvelopeCurve>(toInt(v) & 1)),
    APP_TRACK_FIELD("env.retrigger", false, x.ampEnv.retrigger ? 1 : 0, x.ampEnv.retrigger = v >= 0.5f),
    APP_TRACK_FIELD("env.bypass", false, x.ampEnv.bypass ? 1 : 0, x.ampEnv.bypass = v >= 0.5f),
    // LFO: saved with the project (data-model.md scene[s].track[t].lfo), undoable, posted on every
    // edit and after an open (forwardAllModel). Values are the catalogue's (lfo.cpp) — the engine's units.
    APP_TRACK_FIELD("lfo.speed", false, x.lfo.speed, x.lfo.speed = std::clamp(static_cast<double>(v), 0.0, 127.0)),
    APP_TRACK_FIELD("lfo.depth", false, x.lfo.depth, x.lfo.depth = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("lfo.phase", false, x.lfo.phaseOffset, x.lfo.phaseOffset = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("lfo.shape", false, lfoShapeOf(x.lfo), setLfoShape(x.lfo, v)),
    APP_TRACK_FIELD("lfo.destination", false, lfoDestinationOf(x.lfo), setLfoDestination(x.lfo, v)),
    APP_TRACK_FIELD("lfo.sync", false, x.lfo.sync ? 1 : 0, x.lfo.sync = v >= 0.5f),
    APP_TRACK_FIELD("lfo.triggerMode", false, static_cast<int>(x.lfo.triggerMode),
                    x.lfo.triggerMode = static_cast<LfoTriggerMode>(std::clamp(toInt(v), 0, 2))),
    APP_TRACK_FIELD("lfo.triggerOn", false, x.lfo.triggerOn == LfoTriggerOn::Both ? 1 : 0,
                    x.lfo.triggerOn = v >= 0.5f ? LfoTriggerOn::Both : LfoTriggerOn::Pad),
    APP_TRACK_FIELD("lfo.randomStart", false, x.lfo.randomSeedCount,
                    x.lfo.randomSeedCount = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("lfo.bypass", false, x.lfo.bypass ? 1 : 0, x.lfo.bypass = v >= 0.5f),
    // MIDI / AS-1 tracks (MIDI NOTE, CC1, CC2, BANK SELECT, PROG CHANGE; per scene and track): saved with the project,
    // undoable, re-posted after an open. NOTE2–4 are semitones from NOTE (0 = OFF).
    APP_TRACK_FIELD("midi.note1", false, x.midi.notes[0], x.midi.notes[0] = std::clamp(toInt(v), 0, 119)),
    APP_TRACK_FIELD("midi.note2", false, x.midi.notes[1], x.midi.notes[1] = std::clamp(toInt(v), -127, 127)),
    APP_TRACK_FIELD("midi.note3", false, x.midi.notes[2], x.midi.notes[2] = std::clamp(toInt(v), -127, 127)),
    APP_TRACK_FIELD("midi.note4", false, x.midi.notes[3], x.midi.notes[3] = std::clamp(toInt(v), -127, 127)),
    APP_TRACK_FIELD("midi.velocity", false, x.midi.velocity, x.midi.velocity = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.length", false, x.midi.length,
                    x.midi.length = std::clamp(static_cast<double>(v), 0.25, 64.0)),
    APP_TRACK_FIELD("midi.pitchBend", false, x.midi.pitchBendValue,
                    x.midi.pitchBendValue = std::clamp(toInt(v), -8192, 8191)),
    APP_TRACK_FIELD("midi.pitchBendActive", false, x.midi.pitchBendActive ? 1 : 0, x.midi.pitchBendActive = v >= 0.5f),
    APP_TRACK_FIELD("midi.aftertouch", false, x.midi.aftertouchValue,
                    x.midi.aftertouchValue = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.aftertouchActive", false, x.midi.aftertouchActive ? 1 : 0,
                    x.midi.aftertouchActive = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc1.cc3", false, x.midi.cc1[0].cc, x.midi.cc1[0].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.value3", false, x.midi.cc1[0].value, x.midi.cc1[0].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.active3", false, x.midi.cc1[0].active ? 1 : 0, x.midi.cc1[0].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc1.cc4", false, x.midi.cc1[1].cc, x.midi.cc1[1].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.value4", false, x.midi.cc1[1].value, x.midi.cc1[1].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.active4", false, x.midi.cc1[1].active ? 1 : 0, x.midi.cc1[1].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc1.cc5", false, x.midi.cc1[2].cc, x.midi.cc1[2].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.value5", false, x.midi.cc1[2].value, x.midi.cc1[2].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.active5", false, x.midi.cc1[2].active ? 1 : 0, x.midi.cc1[2].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc1.cc6", false, x.midi.cc1[3].cc, x.midi.cc1[3].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.value6", false, x.midi.cc1[3].value, x.midi.cc1[3].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc1.active6", false, x.midi.cc1[3].active ? 1 : 0, x.midi.cc1[3].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc2.cc1", false, x.midi.cc2[0].cc, x.midi.cc2[0].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.value1", false, x.midi.cc2[0].value, x.midi.cc2[0].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.active1", false, x.midi.cc2[0].active ? 1 : 0, x.midi.cc2[0].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc2.cc2", false, x.midi.cc2[1].cc, x.midi.cc2[1].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.value2", false, x.midi.cc2[1].value, x.midi.cc2[1].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.active2", false, x.midi.cc2[1].active ? 1 : 0, x.midi.cc2[1].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc2.cc3", false, x.midi.cc2[2].cc, x.midi.cc2[2].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.value3", false, x.midi.cc2[2].value, x.midi.cc2[2].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.active3", false, x.midi.cc2[2].active ? 1 : 0, x.midi.cc2[2].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc2.cc4", false, x.midi.cc2[3].cc, x.midi.cc2[3].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.value4", false, x.midi.cc2[3].value, x.midi.cc2[3].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.active4", false, x.midi.cc2[3].active ? 1 : 0, x.midi.cc2[3].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc2.cc5", false, x.midi.cc2[4].cc, x.midi.cc2[4].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.value5", false, x.midi.cc2[4].value, x.midi.cc2[4].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.active5", false, x.midi.cc2[4].active ? 1 : 0, x.midi.cc2[4].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc2.cc6", false, x.midi.cc2[5].cc, x.midi.cc2[5].cc = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.value6", false, x.midi.cc2[5].value, x.midi.cc2[5].value = std::clamp(toInt(v), 0, 127)),
    APP_TRACK_FIELD("midi.cc2.active6", false, x.midi.cc2[5].active ? 1 : 0, x.midi.cc2[5].active = v >= 0.5f),
    APP_TRACK_FIELD("midi.bankMsb", false, x.midi.bankMsb, x.midi.bankMsb = std::clamp(toInt(v), -1, 127)),
    APP_TRACK_FIELD("midi.bankLsb", false, x.midi.bankLsb, x.midi.bankLsb = std::clamp(toInt(v), -1, 127)),
    APP_TRACK_FIELD("midi.program", false, x.midi.program, x.midi.program = std::clamp(toInt(v), -1, 127)),
    APP_TRACK_FIELD("midi.mode", false, x.midi.mode, x.midi.mode = std::clamp(toInt(v), 0, 1)),
    APP_TRACK_FIELD("midi.noteBypass", false, x.midi.noteBypass ? 1 : 0, x.midi.noteBypass = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc1Bypass", false, x.midi.cc1Bypass ? 1 : 0, x.midi.cc1Bypass = v >= 0.5f),
    APP_TRACK_FIELD("midi.cc2Bypass", false, x.midi.cc2Bypass ? 1 : 0, x.midi.cc2Bypass = v >= 0.5f),
    // SEQUENCE track values: the paths core::perf::setStepsParam writes ("scene[s].track[t].seq.*"),
    // so its edits and the knob strip's are the same field; posted on change and after an open.
    APP_TRACK_FIELD("seq.offset", false, x.seq.offset, x.seq.offset = std::clamp(toInt(v), -24, 24)),
    APP_TRACK_FIELD("seq.retrigCount", false, x.seq.retriggerCount, x.seq.retriggerCount = std::clamp(toInt(v), 1, 16)),
    APP_TRACK_FIELD("seq.retrigSpeed", false, x.seq.retriggerSpeed, x.seq.retriggerSpeed = std::clamp(toInt(v), 0, 5)),
    APP_TRACK_FIELD("seq.transpose", false, x.seq.transpose, x.seq.transpose = std::clamp(toInt(v), -24, 24)),
    APP_TRACK_FIELD("seq.shiftArea", false, x.seq.stepShiftArea, x.seq.stepShiftArea = std::clamp(toInt(v), 0, 1)),
    // MIXER project levels
    APP_PROJECT_FIELD("mixer.masterVolume", x.masterVolume, x.masterVolume = toInt(v)),
    APP_PROJECT_FIELD("mixer.sendReturnVolume", x.sendTrackVolume, x.sendTrackVolume = toInt(v)),
    APP_PROJECT_FIELD("mixer.headphoneVolume", x.headphoneVolume, x.headphoneVolume = toInt(v)),
    APP_PROJECT_FIELD("mixer.softLimiter", x.softLimiter ? 1 : 0, x.softLimiter = v >= 0.5f),
    // Project tempo (transport.tempo, storage "tempo"; SYN-002 0.1 BPM grid): saved with the project,
    // undoable, posted to the engine's TransportStage on every edit and after a project opens (forwardAllModel).
    APP_PROJECT_FIELD("transport.tempo", x.tempo, x.tempo = std::round(static_cast<double>(v) * 10.0) / 10.0),
    // The routing in the model (OUTPUT SETTING edits are undoable, saved, and posted
    // through the bus hook like every other field; app/ProjectFeed's postRouting still sends the whole table after an
    // open). OUT ASSIGN per track, ASSIGN MODE and HP CUE per scene, MONITOR LEVEL per project. THRU BYPASS per track
    // in the routing's extras (see thruBypassOf).
    APP_TRACK_FIELD("routing.outAssign", false, static_cast<int>(x.routing.output),
                    x.routing.output = static_cast<core::model::OutputAssign>(std::clamp(toInt(v), 0, 12))),
    APP_TRACK_FIELD("routing.thruBypass", false, thruBypassOf(x.routing), setThruBypass(x.routing, v)),
    APP_TRACK_FIELD("track.motionRec", true, motionRecOf(x), setMotionRec(x, v)), // never posted: the UI's alone
    APP_TRACK_FIELD("track.muteMode", false, muteModeOf(x), setMuteMode(x, v)), // the mixer's strip (STOP STEPS)
    APP_SCENE_FIELD("routing.assignMode", static_cast<int>(x.routing.assignMode),
                    x.routing.assignMode = static_cast<core::model::AssignMode>(std::clamp(toInt(v), 0, 1))),
    APP_SCENE_FIELD("routing.headphoneCue", x.routing.headphoneSource,
                    x.routing.headphoneSource = std::clamp(toInt(v), 0, 12)),
    APP_PROJECT_FIELD("routing.monitorLevel", x.monitorVolume, x.monitorVolume = std::clamp(toInt(v), 0, 127)),
};
#undef APP_TRACK_FIELD
#undef APP_PROJECT_FIELD
#undef APP_SCENE_FIELD

const ModelField* fieldById(std::string_view id) noexcept
{
    for (const auto& f : kFields)
        if (f.id == id)
            return &f;
    return nullptr;
}

constexpr std::string_view kTrackPrefix = "track.";

// "scene[s].track[t].<tail>" for track ids (tail = id without "track."), the id itself for project ids.
std::string pathOf(const ModelField& f, int scene, int track)
{
    if (f.scene)
    {
        char buf[96];
        const int n = std::snprintf(buf, sizeof buf, "scene[%d].%.*s", scene & 15, static_cast<int>(f.id.size()),
                                    f.id.data());
        return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
    }
    if (! f.track)
        return std::string(f.id);
    std::string_view tail = f.id;
    if (tail.substr(0, kTrackPrefix.size()) == kTrackPrefix)
        tail.remove_prefix(kTrackPrefix.size());
    char buf[96];
    const int n = std::snprintf(buf, sizeof buf, "scene[%d].track[%d].%.*s", scene & 15, track & 15,
                                static_cast<int>(tail.size()), tail.data());
    return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}

// The inverse of pathOf: the field and indices a changed path names, or null.
const ModelField* parse(std::string_view path, int& scene, int& track) noexcept
{
    scene = track = 0;
    if (path.substr(0, 6) != "scene[")
        return fieldById(path); // a project id; nullptr for anything else
    std::string_view s = path.substr(6);
    auto number = [&s](int& out) {
        const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        if (r.ec != std::errc{} || r.ptr == s.data() + s.size() || *r.ptr != ']')
            return false;
        s.remove_prefix(static_cast<std::size_t>(r.ptr - s.data()) + 1);
        return true;
    };
    if (! number(scene))
        return nullptr;
    if (s.substr(0, 7) != ".track[") // "scene[s].<id>": a scene field
    {
        if (s.substr(0, 1) != "." || scene < 0 || scene > 15)
            return nullptr;
        const auto* f = fieldById(s.substr(1));
        return f != nullptr && f->scene ? f : nullptr;
    }
    s.remove_prefix(7);
    if (! number(track) || s.substr(0, 1) != ".")
        return nullptr;
    s.remove_prefix(1);
    if (scene < 0 || scene > 15 || track < 0 || track > 15)
        return nullptr;
    auto match = [s](const ModelField& f) {
        if (! f.track)
            return false;
        std::string_view id = f.id;
        if (id.substr(0, kTrackPrefix.size()) == kTrackPrefix)
            id.remove_prefix(kTrackPrefix.size());
        return id == s;
    };
    for (const auto& f : kFields)
        if (match(f))
            return &f;
    return nullptr;
}

// One edit of one table field: inverse = the old value, coalescing on the same path (EDT-U2: knob turns within the
// UndoStack's 1 s window are one undo step).
class SetModelParam final : public core::Command
{
public:
    SetModelParam(const ModelField& f, std::string_view label, int scene, int track, float value, bool undoable = true)
        : field_(f), label_(label), path_(pathOf(f, scene, track)), scene_(scene), track_(track), value_(value),
          undoable_(undoable)
    {
    }
    std::string_view label() const override { return label_; }
    bool undoable() const override { return undoable_; } // incoming CCs write the model without undo
    void apply(Project& p) override { field_.set(p, scene_, track_, value_); }
    std::unique_ptr<core::Command> inverse(const Project& before) const override
    {
        return std::make_unique<SetModelParam>(field_, label_, scene_, track_, field_.get(before, scene_, track_));
    }
    bool mergeWith(const core::Command& other, double) override
    {
        const auto* o = dynamic_cast<const SetModelParam*>(&other);
        return o != nullptr && &o->field_ == &field_ && o->path_ == path_;
    }
    void collectChanges(core::ChangeSet& c) const override { c.add(path_); }
    std::size_t sizeBytes() const override { return sizeof(*this) + path_.capacity(); }

private:
    const ModelField& field_;
    std::string_view label_;
    std::string path_;
    int scene_, track_;
    float value_;
    bool undoable_ = true;
};

void post(engine::EngineBridge& bridge, const core::ParamRegistry& params, const ModelField& f, const Project& p,
          int scene, int track)
{
    if (const auto id = params.idOf(f.id); id && id->valid())
        bridge.post(engine::ParamChange{*id, f.track   ? static_cast<std::uint16_t>((scene << 4) | track)
                                             : f.scene ? static_cast<std::uint16_t>(scene & 15)
                                                       : std::uint16_t{0},
                                        f.get(p, scene, track), engine::kBlockFrames});
}
} // namespace

ModelParamAccess::ModelParamAccess(core::CommandBus& bus, engine::EngineBridge* bridge,
                                   const core::ParamRegistry& params)
    : bus_(bus), params_(params), fallback_(params, bridge), muteField_(fieldById("track.mute"))
{
    const auto all = params_.all();
    byIndex_.resize(all.size());
    for (std::size_t i = 0; i < all.size(); ++i)
        if (const auto* f = fieldById(all[i].id);
            f != nullptr && (all[i].scope == core::ParamScope::Track) == f->track &&
            (all[i].scope == core::ParamScope::Scene) == f->scene)
            byIndex_[i] = f;
}

ModelParamAccess::~ModelParamAccess() = default;

const ModelField* ModelParamAccess::fieldOf(const core::ParamSpec& s) const noexcept
{
    const auto all = params_.all();
    const auto i = static_cast<std::size_t>(&s - all.data());
    return i < byIndex_.size() ? byIndex_[i] : nullptr;
}

bool ModelParamAccess::modelBacked(const core::ParamSpec& s) const noexcept
{
    return fieldOf(s) != nullptr;
}

float ModelParamAccess::get(const core::ParamSpec& s, std::uint16_t scope) const
{
    const auto* f = fieldOf(s);
    if (f == nullptr && core::commands::hasProject(bus_))
        if (const auto* pf = services::settings::findProjectField(s.id)) // UTILITY's project values (MIDI enables,
            return services::settings::projectValue(core::commands::project(bus_), *pf, scope & 15); // channels …)
    if (f == nullptr || ! core::commands::hasProject(bus_))
        return fallback_.get(s, scope);
    if (f->scene)
        return f->get(core::commands::project(bus_), scope & 15, 0);
    if (f == muteField_) // the per-pattern mute the user sees (track-level or the current pattern's)
        return core::commands::track::isMuted(core::commands::project(bus_), (scope >> 4) & 15, scope & 15) ? 1.0f
                                                                                                             : 0.0f;
    return f->get(core::commands::project(bus_), (scope >> 4) & 15, scope & 15);
}

void ModelParamAccess::set(const core::ParamSpec& s, std::uint16_t scope, float value)
{
    const auto* f = fieldOf(s);
    if (f == nullptr && core::commands::hasProject(bus_))
        if (const auto* pf = services::settings::findProjectField(s.id))
        {
            // The project value UTILITY edits (TRACK MENU's SELECT CH is midi.out.channel): one undoable project edit,
            // saved with the project, forwarded by services::settings::forwardSettingsChanges.
            if (auto c = services::settings::setProjectValue(core::commands::project(bus_), *pf, scope & 15, value))
                bus_.execute(std::move(c));
            return;
        }
    if (f == nullptr || ! core::commands::hasProject(bus_))
    {
        fallback_.set(s, scope, value);
        return;
    }
    const float v = s.clamp(value);
    const int scene = f->scene ? scope & 15 : (scope >> 4) & 15, track = f->scene ? 0 : scope & 15;
    if (f == muteField_) // a user mute goes to the current pattern (cmd::track::setMuted)
    {
        if (auto c = core::commands::track::setMuted(core::commands::project(bus_), scene, track, v >= 0.5f))
            bus_.execute(std::move(c));
        return;
    }
    if (f->get(core::commands::project(bus_), scene, track) == v)
        return; // nothing changes: no undo step
    bus_.execute(std::make_unique<SetModelParam>(*f, s.label, scene, track, v));
}

void ModelParamAccess::setRemote(const core::ParamSpec& s, std::uint16_t scope, float value)
{
    const auto* f = fieldOf(s);
    if (f == nullptr || ! core::commands::hasProject(bus_))
    {
        fallback_.set(s, scope, value);
        return;
    }
    const float v = s.clamp(value);
    const int scene = f->scene ? scope & 15 : (scope >> 4) & 15, track = f->scene ? 0 : scope & 15;
    if (f->get(core::commands::project(bus_), scene, track) == v)
        return;
    bus_.execute(std::make_unique<SetModelParam>(*f, s.label, scene, track, v, false));
}

bool ModelParamAccess::lockOf(const core::ParamSpec& s, std::uint16_t scope, int step, float& value) const
{
    return fallback_.lockOf(s, scope, step, value);
}

void ModelParamAccess::setLock(const core::ParamSpec& s, std::uint16_t scope, int step, float value)
{
    fallback_.setLock(s, scope, step, value);
}

void forwardModelChanges(const core::ChangeSet& changes, const Project& p, engine::EngineBridge& bridge,
                         const core::ParamRegistry& params)
{
    for (const auto& path : changes.paths())
    {
        int scene = 0, track = 0;
        if (const auto* f = parse(path, scene, track); f != nullptr && ! f->forwardedByTrig)
            post(bridge, params, *f, p, scene, track);
    }
}

void forwardAllModel(const Project& p, engine::EngineBridge& bridge, const core::ParamRegistry& params, int sceneOrCurrent)
{
    const int scene = (sceneOrCurrent >= 0 ? sceneOrCurrent : p.settings.currentScene) & 15;
    for (int t = 0; t < 16; ++t)
        for (const auto& f : kFields)
            if (f.track)
                post(bridge, params, f, p, scene, t);
    for (const auto& f : kFields)
        if (f.scene)
            post(bridge, params, f, p, scene, 0);
    for (const auto& f : kFields)
        if (! f.track && ! f.scene)
            post(bridge, params, f, p, 0, 0);
}

void fillTrackSummaries(UiState& st, const Project& p, bool (*loading)(const void*, int), const void* ctx) noexcept
{
    static constexpr std::string_view kTypes[] = {"SMPL", "MIDI", "THRU", "AS-1", "PLUG"};
    static constexpr std::string_view kStretch[] = {"OFF", "RSMPL", "M.TMP"};
    const auto& scene = p.scenes[static_cast<std::size_t>(p.settings.currentScene & 15)];
    for (int t = 0; t < 16; ++t)
    {
        const auto& x = scene.tracks[static_cast<std::size_t>(t)];
        auto& s = st.tracks[static_cast<std::size_t>(t)];
        const int modeIdx = static_cast<int>(x.mode);
        s.typeBadge = kTypes[modeIdx >= 0 && modeIdx < 5 ? modeIdx : 0];
        s.muted = core::commands::track::isMuted(p, p.settings.currentScene, t);
        s.soloed = x.solo;
        s.missing = false;
        if (x.mode == TrackMode::Plugin)
        {
            s.hasContent = ! x.plugin.path.empty() || ! x.plugin.pluginId.empty();
            s.name = ! x.name.empty() ? std::string_view(x.name) : std::string_view(x.plugin.name);
            s.stretchBadge = {};
            s.loopGlyph = {};
            continue;
        }
        if (x.mode != TrackMode::Sample)
        {
            s.hasContent = true;
            s.name = x.name;
            s.stretchBadge = {};
            s.loopGlyph = {};
            continue;
        }
        const auto* ref = x.playback.sample.empty() ? nullptr : p.sampleRefs.find(x.playback.sample);
        s.hasContent = ref != nullptr || (loading != nullptr && loading(ctx, t));
        s.missing = ref != nullptr && ref->missing;
        std::string_view name;
        if (ref != nullptr)
        {
            name = ref->displayName;
            if (name.empty())
            {
                name = ref->path;
                if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos)
                    name.remove_prefix(slash + 1);
            }
        }
        s.name = name;
        s.stretchBadge = kStretch[static_cast<int>(x.playback.stretchMode) % 3];
        s.loopGlyph = x.playback.loop ? std::string_view("\xE2\x9F\xB2") : std::string_view("\xE2\x86\x92");
    }
}
} // namespace ui
