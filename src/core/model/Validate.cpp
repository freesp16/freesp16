// validate(): data-model.md "Invariants" plus value ranges and cross-references. Problems are
// reported with the entity's model path; the happy path allocates nothing (paths are formatted only on failure).
#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/model/enums/ValueLists.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace core::model
{
namespace
{
class Checker
{
public:
    Checker(std::vector<std::string>& out, std::size_t maxProblems) : out_(out), max_(maxProblems) {}

    // printf-style problem line.
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void fail(const char* fmt, ...)
    {
        ++failures_;
        if (failures_ > max_)
            return;
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, args);
        va_end(args);
        out_.emplace_back(buf);
    }

    void range(const char* where, const char* field, long long v, long long lo, long long hi)
    {
        if (v < lo || v > hi)
            fail("%s.%s: %lld not in %lld…%lld", where, field, v, lo, hi);
    }
    void rangeOrOff(const char* where, const char* field, long long v, long long off, long long lo, long long hi)
    {
        if (v != off)
            range(where, field, v, lo, hi);
    }
    void real(const char* where, const char* field, double v, double lo, double hi)
    {
        if (! std::isfinite(v) || v < lo || v > hi)
            fail("%s.%s: %g not in %g…%g", where, field, v, lo, hi);
    }
    template <typename En>
    void enumValue(const char* where, const char* field, En v)
    {
        if (! isValidEnum(v))
            fail("%s.%s: %d is not a %.*s value", where, field, static_cast<int>(v),
                 static_cast<int>(enumTable<En>().name.size()), enumTable<En>().name.data());
    }
    template <typename En>
    void enumInt(const char* where, const char* field, int v)
    {
        if (! enumFromInt<En>(v))
            fail("%s.%s: %d is not a %.*s value", where, field, v, static_cast<int>(enumTable<En>().name.size()),
                 enumTable<En>().name.data());
    }

    bool ok() const noexcept { return failures_ == 0; }
    void finish()
    {
        if (failures_ > max_)
            out_.push_back(std::to_string(failures_ - max_) + " more problems not listed");
    }

private:
    std::vector<std::string>& out_;
    std::size_t max_;
    std::size_t failures_ = 0;
};

enum class SlotKind { Insert, Bus };

void checkEffectSettings(Checker& c, const char* where, EffectType type, const std::array<double, 6>& params,
                         SlotKind kind)
{
    c.enumValue(where, "type", type);
    if (isValidEnum(type) && ! (kind == SlotKind::Insert ? effectAllowedOnInsert(type) : effectAllowedOnBus(type)))
        c.fail("%s.type: %.*s is not available on %s slots", where, static_cast<int>(storedName(type).size()),
               storedName(type).data(), kind == SlotKind::Insert ? "insert" : "send/master");
    for (std::size_t i = 0; i < params.size(); ++i)
        if (! std::isfinite(params[i]) || params[i] < 0.0 || params[i] > 1.0)
            c.fail("%s.params[%zu]: %g not in 0…1 (step fraction, FMT-013)", where, i, params[i]);
}

void checkSlot(Checker& c, const char* where, const EffectSlot& slot, SlotKind kind)
{
    checkEffectSettings(c, where, slot.type, slot.params, kind);
    if (kind == SlotKind::Insert && slot.lookAhead)
        c.fail("%s.lookAhead: only Send/Master compressors have look-ahead (OQ-15)", where);
    // FX-007 / FMT-014: one remembered setting per other type the slot has held.
    for (std::size_t i = 0; i < slot.typeMemory.size(); ++i)
    {
        const auto& m = slot.typeMemory[i];
        char sub[160];
        std::snprintf(sub, sizeof sub, "%s.typeMemory[%zu]", where, i);
        checkEffectSettings(c, sub, m.type, m.params, kind);
        if (m.type == EffectType::Off)
            c.fail("%s: Off has no settings to remember", sub);
        if (m.type == slot.type && m.type != EffectType::Off)
            c.fail("%s: remembers the slot's current type", sub);
        for (std::size_t j = 0; j < i; ++j)
            if (slot.typeMemory[j].type == m.type)
                c.fail("%s: type remembered twice", sub);
    }
}

void checkTrack(Checker& c, const char* w, const Track& t, const SamplePool& pool)
{
    c.enumValue(w, "mode", t.mode);
    c.enumValue(w, "triggerMode", t.triggerMode);
    c.range(w, "chokeGroup", t.chokeGroup, 0, kChokeGroups);
    c.range(w, "volume", t.volume, 0, 127);
    c.range(w, "pan", t.pan, -64, 64);
    c.range(w, "colour", t.colour, 0, kPaletteColours - 1);
    c.range(w, "instrument", t.instrument, 0, 0xFFFF);

    const auto& pb = t.playback;
    c.range(w, "playback.start", pb.start, 0, INT64_MAX);
    c.range(w, "playback.length", pb.length, 0, INT64_MAX);
    if (pb.length > 0)
        c.range(w, "playback.loopStart", pb.loopStart, pb.start, pb.start + pb.length);
    else
        c.range(w, "playback.loopStart", pb.loopStart, 0, INT64_MAX);
    c.range(w, "playback.pitchCents", pb.pitchCents, -2400, 2400);
    c.enumValue(w, "playback.stretchMode", pb.stretchMode);
    // data-model.md invariant: every SampleRef in a track resolves to a pool entry (missing is a pool state).
    if (! pb.sample.empty() && pool.find(pb.sample) == nullptr)
        c.fail("%s.playback.sample: '%s' is not in the sample pool", w, pb.sample.id.c_str());

    const auto& env = t.ampEnv;
    c.real(w, "ampEnv.attack", env.attack, 0.0, 1.0);
    c.real(w, "ampEnv.hold", env.hold, 0.0, 1.0);
    c.real(w, "ampEnv.release", env.release, 0.0, 1.0);
    c.range(w, "ampEnv.volumeIndex", env.volumeIndex, 0, 127);
    c.enumValue(w, "ampEnv.attackCurve", env.attackCurve);
    c.enumValue(w, "ampEnv.releaseCurve", env.releaseCurve);

    c.range(w, "seq.retriggerCount", t.seq.retriggerCount, 1, kMaxRetriggerCount);
    c.enumInt<RetriggerSpeed>(w, "seq.retriggerSpeed", t.seq.retriggerSpeed);
    c.enumInt<StepShiftArea>(w, "seq.stepShiftArea", t.seq.stepShiftArea);
    c.range(w, "seq.transpose", t.seq.transpose, -24, 24);

    c.range(w, "slices.gridCount", t.slices.gridCount, 1, kSteps);
    for (std::size_t i = 0; i < t.slices.grids.size(); ++i)
        if (t.slices.grids[i] < 0)
            c.fail("%s.slices.grids[%zu]: negative frame %lld", w, i, static_cast<long long>(t.slices.grids[i]));

    c.enumInt<ScaleType>(w, "padScale.scaleType", t.padScale.scaleType);
    c.range(w, "padScale.keyShift", t.padScale.keyShift, -12, 12);
    c.enumInt<ScaleType>(w, "midiScale.scaleTypeMidi", t.midiScale.scaleTypeMidi);
    c.range(w, "midiScale.rootNote", t.midiScale.rootNote, 0, 127);

    for (std::size_t i = 0; i < t.insertFx.size(); ++i)
    {
        char sub[96];
        std::snprintf(sub, sizeof sub, "%s.insertFx[%zu]", w, i);
        checkSlot(c, sub, t.insertFx[i], SlotKind::Insert);
    }

    c.range(w, "send.channel", t.send.channel, 0, 0); // one send bus (enumerations.md "Send destination")
    c.range(w, "send.amount", t.send.amount, 0, 127);

    const auto& lfo = t.lfo;
    if (! std::isfinite(lfo.speed) || lfo.speed < 0.0)
        c.fail("%s.lfo.speed: %g is not a finite non-negative value", w, lfo.speed);
    c.range(w, "lfo.depth", lfo.depth, 0, 127);
    c.range(w, "lfo.phaseOffset", lfo.phaseOffset, 0, 127);
    c.range(w, "lfo.randomSeedCount", lfo.randomSeedCount, 0, 127);
    c.enumValue(w, "lfo.shape", lfo.shape);
    c.enumValue(w, "lfo.triggerMode", lfo.triggerMode);
    c.enumValue(w, "lfo.triggerOn", lfo.triggerOn);

    const auto& m = t.midi;
    c.range(w, "midi.notes[0]", m.notes[0], 0, 127);
    for (std::size_t i = 1; i < m.notes.size(); ++i)
        if (m.notes[i] != -1 && (m.notes[i] < 0 || m.notes[i] > 127))
            c.fail("%s.midi.notes[%zu]: %d is neither OFF (-1) nor a note 0…127", w, i, m.notes[i]);
    c.range(w, "midi.velocity", m.velocity, 0, 127);
    c.real(w, "midi.length", m.length, 0.25, 64.0);
    c.range(w, "midi.pitchBendValue", m.pitchBendValue, -8192, 8191);
    c.range(w, "midi.aftertouchValue", m.aftertouchValue, 0, 127);
    for (const auto* cc : {&m.cc1[0], &m.cc1[1], &m.cc1[2], &m.cc1[3]})
    {
        c.range(w, "midi.cc1.cc", cc->cc, 0, 127);
        c.range(w, "midi.cc1.value", cc->value, 0, 127);
    }
    for (const auto& cc : m.cc2)
    {
        c.range(w, "midi.cc2.cc", cc.cc, 0, 127);
        c.range(w, "midi.cc2.value", cc.value, 0, 127);
    }
    c.rangeOrOff(w, "midi.bankMsb", m.bankMsb, -1, 0, 127);
    c.rangeOrOff(w, "midi.bankLsb", m.bankLsb, -1, 0, 127);
    c.rangeOrOff(w, "midi.program", m.program, -1, 0, 127);
    c.enumInt<MidiTrackMode>(w, "midi.mode", m.mode);

    c.enumValue(w, "routing.output", t.routing.output);
}

void checkPattern(Checker& c, int scene, int index, const Pattern& p)
{
    char w[48];
    std::snprintf(w, sizeof w, "scene[%d].pattern[%d]", scene, index);
    c.range(w, "length", p.length, 1, kSteps); // files may hold 1–64 (SEQ-001)

    std::vector<std::uint8_t> refs; // allocated only when the pattern has locks
    if (! p.locks.empty())
        refs.assign(p.locks.size(), 0);
    for (int s = 0; s < kSteps; ++s)
        for (int t = 0; t < kTracks; ++t)
        {
            const Step& cell = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            if (cell.trigger == Step::kEmpty && cell.lockIndex == Step::kNoLocks && cell.value2 == Step::kNoValue2 &&
                cell.triggerType == TriggerType::Full)
                continue; // the common empty cell
            if (! isValidEnum(cell.triggerType))
                c.fail("%s.step[%d].track[%d].triggerType: %d is not a TriggerType value", w, s, t,
                       static_cast<int>(cell.triggerType));
            if (cell.value2 != Step::kNoValue2 && (cell.value2 < 1 || cell.value2 > 127))
                c.fail("%s.step[%d].track[%d].velocity: %d not in 1…127", w, s, t, cell.value2);
            if (cell.lockIndex != Step::kNoLocks)
            {
                if (cell.lockIndex >= p.locks.size())
                    c.fail("%s.step[%d].track[%d].lockIndex: %u but the pattern has %zu lock entries", w, s, t,
                           static_cast<unsigned>(cell.lockIndex), p.locks.size());
                else if (refs[cell.lockIndex]++ != 0)
                    c.fail("%s.step[%d].track[%d].lockIndex: lock entry %u is shared with another cell", w, s, t,
                           static_cast<unsigned>(cell.lockIndex));
            }
        }

    constexpr std::uint64_t kValidMask = (std::uint64_t {1} << kLockSlots) - 1;
    for (std::size_t i = 0; i < p.locks.size(); ++i)
    {
        const auto& l = p.locks[i];
        if (refs[i] == 0)
            c.fail("%s.locks[%zu]: not referenced by any step", w, i);
        if (l.mask == 0)
            c.fail("%s.locks[%zu]: holds no lock", w, i);
        if ((l.mask & ~kValidMask) != 0)
            c.fail("%s.locks[%zu].mask: slots beyond %d set", w, i, kLockSlots - 1);
        for (int s = 0; s < kLockSlots; ++s)
        {
            if ((l.mask & (std::uint64_t {1} << s)) == 0)
                continue;
            if (! std::isfinite(l.values[static_cast<std::size_t>(s)]))
                c.fail("%s.locks[%zu].values[%d]: not finite", w, i, s);
            if (! isValidEnum(l.change[static_cast<std::size_t>(s)]))
                c.fail("%s.locks[%zu].change[%d]: %d is not a LockChange value", w, i, s,
                       static_cast<int>(l.change[static_cast<std::size_t>(s)]));
        }
    }
}

void checkScene(Checker& c, int index, const Scene& s, const SamplePool& pool)
{
    char w[32];
    std::snprintf(w, sizeof w, "scene[%d]", index);
    c.range(w, "colour", s.colour, 0, kPaletteColours - 1);
    if (s.bpm)
        c.real(w, "bpm", *s.bpm, kMinTempo, kMaxTempo); // 0 = no scene BPM is decoded as absent (data-model.md)
    c.enumValue(w, "routing.assignMode", s.routing.assignMode);
    c.enumInt<HeadphoneCue>(w, "routing.headphoneSource", s.routing.headphoneSource);
    c.enumValue(w, "routing.fxMode", s.routing.fxMode);
    c.range(w, "routing.afsAssignment", s.routing.afsAssignment, -1, kTracks - 1);

    char sub[64];
    std::snprintf(sub, sizeof sub, "%s.sendFx", w);
    checkSlot(c, sub, s.sendFx, SlotKind::Bus);
    std::snprintf(sub, sizeof sub, "%s.masterFx", w);
    checkSlot(c, sub, s.masterFx, SlotKind::Bus);

    for (int t = 0; t < kTracks; ++t)
    {
        std::snprintf(sub, sizeof sub, "%s.track[%d]", w, t);
        checkTrack(c, sub, s.tracks[static_cast<std::size_t>(t)], pool);
    }
    for (int p = 0; p < kPatterns; ++p)
        checkPattern(c, index, p, s.patterns[static_cast<std::size_t>(p)]);
}
} // namespace

bool validate(const Project& project, std::vector<std::string>& problems, std::size_t maxProblems)
{
    Checker c(problems, maxProblems);

    const auto& st = project.settings;
    const char* w = "settings";
    c.range(w, "masterVolume", st.masterVolume, 0, 127);
    c.range(w, "headphoneVolume", st.headphoneVolume, 0, 127);
    c.range(w, "sendTrackVolume", st.sendTrackVolume, 0, 127);
    c.range(w, "monitorVolume", st.monitorVolume, 0, 127);
    c.enumValue(w, "afsRouteMode", st.afsRouteMode);
    c.enumInt<AfsSendMode>(w, "afsSendMode", st.afsSendMode);
    c.range(w, "afsSendTrack", st.afsSendTrack, -1, kTracks - 1);
    c.range(w, "swing", st.swing, kMinSwing, kMaxSwing);
    c.real(w, "tempo", st.tempo, kMinTempo, kMaxTempo);
    c.enumValue(w, "patternQuantize", st.patternQuantize);
    c.enumValue(w, "padQuantize", st.padQuantize);
    c.range(w, "currentScene", st.currentScene, 0, kScenes - 1);
    c.range(w, "currentPattern", st.currentPattern, 0, kPatterns - 1);
    c.range(w, "formatVersion", st.formatVersion, 1, 0xFFFF);
    c.range(w, "formatMinorVersion", st.formatMinorVersion, 0, 0xFFFF);

    // Sample pool: one entry per content id; ids are never empty.
    const auto& entries = project.sampleRefs.entries;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].id.empty())
            c.fail("sampleRefs.entries[%zu].id: empty", i);
        for (std::size_t j = 0; j < i; ++j)
            if (! entries[i].id.empty() && entries[j].id == entries[i].id)
                c.fail("sampleRefs.entries[%zu].id: '%s' duplicates entry %zu", i, entries[i].id.c_str(), j);
    }

    for (int s = 0; s < kScenes; ++s)
        checkScene(c, s, project.scenes[static_cast<std::size_t>(s)], project.sampleRefs);

    const auto& arr = project.arrangement;
    if (arr.sections.size() > static_cast<std::size_t>(Arrangement::kMaxSections))
        c.fail("arrangement.sections: %zu rows, at most %d", arr.sections.size(), Arrangement::kMaxSections);
    c.range("arrangement", "length", arr.length, 0, INT32_MAX);
    for (std::size_t i = 0; i < arr.sections.size(); ++i)
    {
        const auto& sec = arr.sections[i];
        char sw[48];
        std::snprintf(sw, sizeof sw, "arrangement.sections[%zu]", i);
        c.range(sw, "scene", sec.scene, 0, kScenes - 1);
        c.range(sw, "pattern", sec.pattern, 0, kPatterns - 1);
        c.range(sw, "repeats", sec.repeats, 1, INT32_MAX);
        if (sec.fixedBpm)
            c.real(sw, "fixedBpm", *sec.fixedBpm, kMinTempo, kMaxTempo);
    }

    const auto& rib = project.ribbon;
    for (std::size_t u = 0; u < rib.user.size(); ++u)
        for (std::size_t i = 0; i < rib.user[u].size(); ++i)
        {
            char rw[48];
            std::snprintf(rw, sizeof rw, "ribbon.user[%zu][%zu]", u, i);
            c.real(rw, "top", rib.user[u][i].top, 0.0, 1.0);
            c.real(rw, "bottom", rib.user[u][i].bottom, 0.0, 1.0);
        }
    c.range("ribbon", "pitchTopCents", rib.pitchTopCents, 0, 2400);
    c.range("ribbon", "pitchBottomCents", rib.pitchBottomCents, -2400, 0);

    for (const auto* port : {&project.midi.input, &project.midi.output})
        for (std::size_t t = 0; t < port->channels.size(); ++t)
            if (port->channels[t] < 0 || port->channels[t] > 16)
                c.fail("midi.%s.channels[%zu]: %d is neither OFF (0) nor 1…16",
                       port == &project.midi.input ? "input" : "output", t, port->channels[t]);

    c.finish();
    return c.ok();
}
} // namespace core::model
