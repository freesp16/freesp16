// Helpers of the store tests: scratch folders, a seeded random project that validates, a model diff for messages.
// Everything here is generated: no device data.
#pragma once

#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/model/enums/ValueLists.h"
#include "services/store/NativeCodec.h"
#include "services/store/NativeCodecJson.h"
#include "services/store/SampleRefs.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <random>
#include <string>

namespace storetest
{
namespace m = core::model;

// A fresh empty folder under the build's artifact dir, removed by the destructor.
class ScratchFolder
{
public:
    explicit ScratchFolder(const juce::String& name)
        : folder_(juce::File(APP_ARTIFACT_DIR).getChildFile("store-tests").getChildFile(name + "-" +
                                                                                      juce::String(juce::Random::getSystemRandom().nextInt())))
    {
        folder_.deleteRecursively();
        folder_.createDirectory();
    }
    ~ScratchFolder() { folder_.deleteRecursively(); }
    const juce::File& get() const noexcept { return folder_; }
    juce::File operator/(const char* child) const { return folder_.getChildFile(child); }

private:
    juce::File folder_;
};

// A file's bytes (no text decoding or line-ending changes).
inline std::string readBytes(const juce::File& f)
{
    juce::MemoryBlock data;
    if (! f.loadFileAsData(data))
        return {};
    return std::string(static_cast<const char*>(data.getData()), data.getSize());
}
inline bool writeBytes(const juce::File& f, const std::string& s)
{
    f.deleteFile();
    return f.appendData(s.data(), s.size());
}

class Rng
{
public:
    explicit Rng(std::uint32_t seed) : g_(seed) {}
    int range(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g_); }
    bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(g_) < p; }
    double real(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(g_); }
    float realf(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_); }
    template <typename E>
    E pick()
    {
        return static_cast<E>(range(0, static_cast<int>(m::enumCount<E>()) - 1));
    }
    std::string text()
    {
        static const char* parts[] = {"Intro", "Drop", "\"q\"", "back\\slash", "tab\tx", "Größe", "日本", "a/b", "",
                                      "x\ny", "\001ctl"};
        std::string s;
        for (int i = range(0, 3); i > 0; --i)
            s += parts[range(0, static_cast<int>(std::size(parts)) - 1)];
        return s;
    }
    std::mt19937& engine() { return g_; }

private:
    std::mt19937 g_;
};

inline void randomExtras(Rng& r, m::Extras& x)
{
    if (! r.chance(0.15))
        return;
    // native entries (canonical JSON, keys no schema uses) first, then foreign ones: the order a read produces.
    for (int i = r.range(0, 2); i > 0; --i)
    {
        juce::var value;
        switch (r.range(0, 3))
        {
            case 0: value = r.range(-1000, 1000); break;
            case 1: value = juce::String::fromUTF8(r.text().c_str()); break;
            case 2:
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("k", r.real(-1, 1));
                o->setProperty("list", juce::Array<juce::var> {true, juce::var(), 3});
                value = juce::var(o);
                break;
            }
            default: value = r.real(-1e9, 1e9); break;
        }
        x.entries.push_back({"native.x_future" + std::to_string(i), m::Extras::Encoding::Json,
                             services::store::detail::canonicalJson(value)});
    }
    if (r.chance(0.5))
        x.entries.push_back({"sp16.node" + std::to_string(r.range(0, 9)), m::Extras::Encoding::Base64, "AAECAwQFBgc="});
    if (r.chance(0.3))
        x.entries.push_back({"sp16.enum", m::Extras::Encoding::Text, "WOBBLE"});
}

inline void randomSlot(Rng& r, m::EffectSlot& s, bool insert)
{
    const auto types = insert ? m::insertEffectTypes() : m::busEffectTypes();
    s.type = types[static_cast<std::size_t>(r.range(0, static_cast<int>(types.size()) - 1))];
    s.bypass = r.chance(0.2);
    s.sync = r.chance(0.3);
    s.lookAhead = ! insert && r.chance(0.3);
    s.type1 = r.range(0, 3);
    s.type2 = r.range(0, 3);
    for (auto& p : s.params)
        p = r.chance(0.3) ? r.range(0, 20) / 20.0 : r.real(0, 1);
    s.typeMemory.clear();
    for (const auto t : types)
        if (t != m::EffectType::Off && t != s.type && r.chance(0.2))
        {
            m::EffectSettings e;
            e.type = t;
            e.type1 = r.range(0, 2);
            e.params[static_cast<std::size_t>(r.range(0, 5))] = r.real(0, 1);
            e.sync = r.chance(0.5);
            e.lookAhead = ! insert && r.chance(0.5);
            s.typeMemory.push_back(e);
        }
    randomExtras(r, s.extras);
}

inline void randomTrack(Rng& r, m::Track& t, const m::SamplePool& pool, bool force = false)
{
    if (! force && ! r.chance(0.3))
        return; // keep most tracks at their defaults
    t.mode = r.pick<m::TrackMode>();
    t.triggerMode = r.pick<m::TriggerMode>();
    t.chokeGroup = r.range(0, m::kChokeGroups);
    t.volume = r.range(0, 127);
    t.pan = r.range(-64, 64);
    t.mute = r.chance(0.2);
    t.solo = r.chance(0.1);
    t.colour = r.range(0, m::kPaletteColours - 1);
    t.instrument = r.chance(0.1) ? r.range(1, 40) : 0;
    t.name = r.text();
    auto& pb = t.playback;
    pb.start = r.range(0, 1 << 20);
    pb.length = r.chance(0.1) ? std::int64_t {5'000'000'000} : r.range(0, 1 << 22);
    pb.loopStart = pb.start + (pb.length > 0 ? r.range(0, 1000) % (pb.length + 1) : 0);
    pb.loop = r.chance(0.4);
    pb.reverse = r.chance(0.2);
    pb.pitchCents = r.range(-2400, 2400);
    pb.stretchMode = r.pick<m::StretchMode>();
    if (! pool.entries.empty() && r.chance(0.8))
        pb.sample.id = pool.entries[static_cast<std::size_t>(r.range(0, static_cast<int>(pool.entries.size()) - 1))].id;
    pb.bypass = r.chance(0.1);
    auto& env = t.ampEnv;
    env.attack = r.real(0, 1);
    env.hold = r.chance(0.2) ? 1.0 : r.real(0, 1);
    env.release = r.real(0, 1);
    env.volumeIndex = r.range(0, 127);
    env.sync = r.chance(0.5);
    env.velocitySensitive = r.chance(0.5);
    env.retrigger = r.chance(0.5);
    env.attackCurve = r.pick<m::EnvelopeCurve>();
    env.releaseCurve = r.pick<m::EnvelopeCurve>();
    t.seq.retriggerCount = r.range(1, m::kMaxRetriggerCount);
    t.seq.retriggerSpeed = r.range(0, static_cast<int>(m::enumCount<m::RetriggerSpeed>()) - 1);
    t.seq.offset = r.range(-8, 8);
    t.seq.stepShiftArea = r.range(0, 1);
    t.seq.transpose = r.range(-24, 24);
    if (r.chance(0.2))
        for (int i = r.range(1, 8); i > 0; --i)
            t.slices.grids.push_back(r.range(0, 1 << 24));
    t.slices.gridCount = r.range(1, m::kSteps);
    t.padScale.scaleType = r.range(0, static_cast<int>(m::enumCount<m::ScaleType>()) - 1);
    t.padScale.keyShift = r.range(-12, 12);
    t.midiScale.rootNote = r.range(0, 127);
    randomSlot(r, t.insertFx[0], true);
    if (r.chance(0.5))
        randomSlot(r, t.insertFx[1], true);
    t.send.amount = r.range(0, 127);
    t.lfo.speed = r.real(0, 40);
    t.lfo.depth = r.range(0, 127);
    t.lfo.destination = r.chance(0.5) ? r.range(100, 155) : r.range(0, 22);
    t.lfo.shape = r.pick<m::LfoShape>();
    t.lfo.triggerOn = r.pick<m::LfoTriggerOn>();
    t.midi.notes = {r.range(0, 127), r.chance(0.5) ? r.range(0, 127) : -1, -1, r.chance(0.2) ? 64 : -1};
    t.midi.velocity = r.range(0, 127);
    t.midi.length = r.real(0.25, 64);
    t.midi.cc1[2] = {r.range(0, 127), r.range(0, 127), r.chance(0.5)};
    t.midi.cc2[5].active = r.chance(0.5);
    t.midi.pitchBendValue = r.range(-8192, 8191);
    t.midi.program = r.chance(0.5) ? r.range(0, 127) : -1;
    t.midi.mode = r.range(0, 1);
    if (t.mode == m::TrackMode::Plugin || r.chance(0.15))
    {
        t.plugin.path = r.chance(0.5) ? "Anvil.clap" : "Sub/Synth.clap";
        t.plugin.pluginId = "com.hdslave.anvil";
        t.plugin.name = "Anvil";
        t.plugin.presetKey = r.chance(0.5) ? "fm-perc" : "";
        t.plugin.stateBase64 = r.chance(0.5) ? "AAECAw==" : "";
        t.plugin.bypass = r.chance(0.2);
        randomExtras(r, t.plugin.extras);
    }
    t.routing.output = r.pick<m::OutputAssign>();
    randomExtras(r, t.extras);
    randomExtras(r, t.playback.extras);
}

inline void randomPattern(Rng& r, m::Pattern& p, double density)
{
    p.length = r.range(1, m::kSteps);
    for (int t = 0; t < m::kTracks; ++t)
    {
        p.trackMutes[static_cast<std::size_t>(t)] = r.chance(0.1);
        p.trackSolos[static_cast<std::size_t>(t)] = r.chance(0.05);
    }
    for (int s = 0; s < m::kSteps; ++s)
        for (int t = 0; t < m::kTracks; ++t)
        {
            if (! r.chance(density))
                continue;
            auto& cell = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            cell.trigger = static_cast<std::int16_t>(r.range(0, 127));
            if (r.chance(0.7))
                cell.value2 = static_cast<std::int16_t>(r.range(1, 127));
            cell.triggerType = r.pick<m::TriggerType>();
            if (r.chance(0.15))
            {
                m::StepLocks l;
                for (int k = r.range(1, 4); k > 0; --k)
                {
                    const int slot = r.range(0, m::kLockSlots - 1);
                    l.mask |= std::uint64_t {1} << slot;
                    l.values[static_cast<std::size_t>(slot)] = r.chance(0.5) ? static_cast<float>(r.range(-2400, 2400))
                                                                             : r.realf(0, 1);
                    l.change[static_cast<std::size_t>(slot)] = r.pick<m::LockChange>();
                }
                cell.lockIndex = static_cast<std::uint16_t>(p.locks.size());
                p.locks.push_back(l);
            }
        }
    if (r.chance(0.1))
        p.extras.entries.push_back({"native.step[3].track[2].x_future", m::Extras::Encoding::Json, "[1,2]"});
    randomExtras(r, p.extras);
}

// A valid random project (validate() passes); `density` = probability of a filled step in used patterns;
// `everyPattern` = every scene, track and pattern is used (the benchmark's full project).
inline std::unique_ptr<m::Project> randomProject(std::uint32_t seed, double density = 0.1, bool everyPattern = false)
{
    Rng r(seed);
    auto p = m::newDefaultProject();
    auto& st = p->settings;
    st.tempo = r.chance(0.5) ? 120.0 + r.range(-50, 50) : r.real(m::kMinTempo, m::kMaxTempo);
    st.swing = r.range(m::kMinSwing, m::kMaxSwing);
    st.patternQuantize = r.pick<m::QuantizeUnit>();
    st.padQuantize = r.pick<m::QuantizeUnit>();
    st.masterVolume = r.range(0, 127);
    st.afsEnabled = r.chance(0.3);
    st.afsRouteMode = r.pick<m::AfsRouteMode>();
    st.afsSendMode = r.range(0, 1);
    st.afsSendTrack = r.range(-1, 15);
    st.softLimiter = r.chance(0.5);
    st.currentScene = r.range(0, 15);
    st.currentPattern = r.range(0, 15);
    randomExtras(r, st.extras);

    for (int i = r.range(0, 12); i > 0; --i)
    {
        m::SampleRef ref;
        const auto content = "sample-" + std::to_string(seed) + "-" + std::to_string(i);
        ref.id = services::store::contentIdOf(content.data(), content.size());
        static const char* paths[] = {"$Lib/Drums/kick.wav", "$Bundle/snare 2.wav", "$Ext:USB1/loops/a.wav",
                                      "/abs/path/x.wav", "$Lib", "$Ext:/x.wav", "relative.wav"};
        ref.path = paths[r.range(0, static_cast<int>(std::size(paths)) - 1)];
        ref.displayName = r.text();
        randomExtras(r, ref.extras);
        p->sampleRefs.entries.push_back(std::move(ref));
    }

    for (int s = 0; s < m::kScenes; ++s)
    {
        auto& scene = p->scenes[static_cast<std::size_t>(s)];
        if (! everyPattern && ! r.chance(0.35))
            continue;
        scene.name = r.text();
        scene.colour = r.range(0, m::kPaletteColours - 1);
        if (r.chance(0.3))
            scene.bpm = r.real(m::kMinTempo, m::kMaxTempo);
        scene.routing.assignMode = r.pick<m::AssignMode>();
        scene.routing.headphoneSource = r.range(0, 12);
        scene.routing.fxMode = r.pick<m::FxMode>();
        scene.routing.afsAssignment = r.range(-1, 15);
        randomSlot(r, scene.sendFx, false);
        randomSlot(r, scene.masterFx, false);
        for (auto& t : scene.tracks)
            randomTrack(r, t, p->sampleRefs, everyPattern);
        for (auto& pattern : scene.patterns)
            if (everyPattern || r.chance(0.3))
                randomPattern(r, pattern, density);
        randomExtras(r, scene.extras);
    }

    for (int i = r.range(0, 20); i > 0; --i)
    {
        m::Section sec;
        sec.scene = static_cast<std::uint8_t>(r.range(0, 15));
        sec.pattern = static_cast<std::uint8_t>(r.range(0, 15));
        sec.repeats = r.range(1, 16);
        if (r.chance(0.2))
            sec.fixedBpm = r.real(m::kMinTempo, m::kMaxTempo);
        if (r.chance(0.1))
            sec.extras.set("sp16.record", "AAECAwQFBgc=", m::Extras::Encoding::Base64);
        p->arrangement.sections.push_back(sec);
    }
    p->arrangement.length = static_cast<int>(p->arrangement.sections.size());
    p->ribbon.user[1][3] = {"track.volume", r.real(0, 1), r.real(0, 1)};
    p->ribbon.pitchTopCents = r.range(0, 2400);
    p->midi.input.clock = r.chance(0.5);
    p->midi.output.channels[5] = r.range(0, 16);
    randomExtras(r, p->extras);
    return p;
}

// "<first differing line of the full encodings>" or "" when equal: readable failure messages.
inline std::string firstDifference(const m::Project& a, const m::Project& b)
{
    services::store::SaveOptions o;
    o.writeDefaults = true;
    o.timestamp = "t";
    const auto fa = services::store::NativeCodec::encode(a, o), fb = services::store::NativeCodec::encode(b, o);
    for (std::size_t f = 0; f < fa.size(); ++f)
    {
        juce::StringArray la, lb;
        la.addLines(juce::String::fromUTF8(fa[f].text.c_str()));
        lb.addLines(juce::String::fromUTF8(fb[f].text.c_str()));
        for (int i = 0; i < std::max(la.size(), lb.size()); ++i)
            if (la[i] != lb[i])
                return fa[f].name + ":" + std::to_string(i + 1) + ": '" + la[i].toStdString() + "' vs '" +
                       lb[i].toStdString() + "'";
    }
    return a == b ? std::string() : std::string("(equal encodings, unequal models: a member the format drops)");
}
} // namespace storetest
