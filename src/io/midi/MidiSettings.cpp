#include "io/midi/MidiSettings.h"

#include "ProjectIdentity.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace io::midi
{
namespace
{
struct Poster
{
    engine::EngineBridge& bridge;
    const core::ParamRegistry& reg;
    bool ok = true;
    // Posts `value` for `id` at `scope` when it differs from the ParamSpec default (the engine's initial value).
    void post(std::string_view id, std::uint16_t scope, float value, bool always = false)
    {
        const auto pid = reg.idOf(id);
        if (! pid)
            return;
        if (! always && std::fabs(reg[*pid].defaultValue - value) < 1e-6f)
            return;
        ok = bridge.post(engine::ParamChange {*pid, scope, value, 0}) && ok;
    }
};

UiEventSink& sink()
{
    static UiEventSink s;
    return s;
}
double& tempo()
{
    static double t = 0.0;
    return t;
}
} // namespace

bool postMidiSettings(const core::model::Project& p, engine::EngineBridge& b, const core::ParamRegistry& reg)
{
    Poster s {b, reg};
    const auto& in = p.midi.input;
    const auto& out = p.midi.output;
    // Every enable and channel, defaults too: the engine still holds the previous project's (manual p.45/46: the MIDI
    // IN / OUT settings are per project).
    s.post("midi.in.clock", 0, in.clock ? 1.0f : 0.0f, true);
    s.post("midi.in.startStop", 0, in.startStop ? 1.0f : 0.0f, true);
    s.post("midi.in.noteCc", 0, in.noteCc ? 1.0f : 0.0f, true);
    s.post("midi.in.songPosition", 0, in.songPosition ? 1.0f : 0.0f, true);
    s.post("midi.out.clock", 0, out.clock ? 1.0f : 0.0f, true);
    s.post("midi.out.startStop", 0, out.startStop ? 1.0f : 0.0f, true);
    s.post("midi.out.noteCc", 0, out.noteCc ? 1.0f : 0.0f, true);
    s.post("midi.out.songPosition", 0, out.songPosition ? 1.0f : 0.0f, true);
    for (int t = 0; t < 16; ++t)
    {
        const auto scope = static_cast<std::uint16_t>(t);
        s.post("midi.in.channel", scope, static_cast<float>(in.channels[static_cast<std::size_t>(t)]), true);
        s.post("midi.out.channel", scope, static_cast<float>(out.channels[static_cast<std::size_t>(t)]), true);
    }
    // The MIDI track values (16 × 16 × 47): the engine resets them all to the defaults first, then takes only this
    // project's differences.
    s.post("midi.mode", core::midi::kProjectOpenScope, 0.0f, true);
    static constexpr std::string_view notes[] = {"midi.note1", "midi.note2", "midi.note3", "midi.note4"};
    static constexpr std::string_view cc1[3][4] = {
        {"midi.cc1.cc3", "midi.cc1.cc4", "midi.cc1.cc5", "midi.cc1.cc6"},
        {"midi.cc1.value3", "midi.cc1.value4", "midi.cc1.value5", "midi.cc1.value6"},
        {"midi.cc1.active3", "midi.cc1.active4", "midi.cc1.active5", "midi.cc1.active6"}};
    static constexpr std::string_view cc2[3][6] = {
        {"midi.cc2.cc1", "midi.cc2.cc2", "midi.cc2.cc3", "midi.cc2.cc4", "midi.cc2.cc5", "midi.cc2.cc6"},
        {"midi.cc2.value1", "midi.cc2.value2", "midi.cc2.value3", "midi.cc2.value4", "midi.cc2.value5",
         "midi.cc2.value6"},
        {"midi.cc2.active1", "midi.cc2.active2", "midi.cc2.active3", "midi.cc2.active4", "midi.cc2.active5",
         "midi.cc2.active6"}};
    for (int sc = 0; sc < 16; ++sc)
        for (int t = 0; t < 16; ++t)
        {
            const auto& m = p.scenes[static_cast<std::size_t>(sc)].tracks[static_cast<std::size_t>(t)].midi;
            const auto scope = static_cast<std::uint16_t>((sc << 4) | t);
            for (int i = 0; i < 4; ++i)
                s.post(notes[i], scope, static_cast<float>(m.notes[static_cast<std::size_t>(i)]));
            s.post("midi.velocity", scope, static_cast<float>(m.velocity));
            s.post("midi.length", scope, static_cast<float>(m.length));
            s.post("midi.noteBypass", scope, m.noteBypass ? 1.0f : 0.0f);
            s.post("midi.pitchBend", scope, static_cast<float>(m.pitchBendValue));
            s.post("midi.pitchBendActive", scope, m.pitchBendActive ? 1.0f : 0.0f);
            s.post("midi.aftertouch", scope, static_cast<float>(m.aftertouchValue));
            s.post("midi.aftertouchActive", scope, m.aftertouchActive ? 1.0f : 0.0f);
            for (int i = 0; i < 4; ++i)
            {
                const auto& c = m.cc1[static_cast<std::size_t>(i)];
                s.post(cc1[0][i], scope, static_cast<float>(c.cc));
                s.post(cc1[1][i], scope, static_cast<float>(c.value));
                s.post(cc1[2][i], scope, c.active ? 1.0f : 0.0f);
            }
            s.post("midi.cc1Bypass", scope, m.cc1Bypass ? 1.0f : 0.0f);
            for (int i = 0; i < 6; ++i)
            {
                const auto& c = m.cc2[static_cast<std::size_t>(i)];
                s.post(cc2[0][i], scope, static_cast<float>(c.cc));
                s.post(cc2[1][i], scope, static_cast<float>(c.value));
                s.post(cc2[2][i], scope, c.active ? 1.0f : 0.0f);
            }
            s.post("midi.cc2Bypass", scope, m.cc2Bypass ? 1.0f : 0.0f);
            s.post("midi.bankMsb", scope, static_cast<float>(m.bankMsb));
            s.post("midi.bankLsb", scope, static_cast<float>(m.bankLsb));
            s.post("midi.program", scope, static_cast<float>(m.program));
            s.post("midi.mode", scope, static_cast<float>(m.mode));
        }
    return s.ok;
}

namespace
{
SettingsMap& configured()
{
    static SettingsMap m;
    return m;
}
} // namespace

void configure(const SettingsMap& settings) { configured() = settings; }

std::string setting(std::string_view key, std::string_view fallback)
{
    std::string env = std::string(identity::envPrefix) + "_";
    for (const char c : key)
        env += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    if (const char* v = std::getenv(env.c_str()))
        return v;
    if (const auto it = configured().find(key); it != configured().end())
        return it->second;
    return std::string(fallback);
}

namespace
{
std::vector<std::string>& names()
{
    static std::vector<std::string> n;
    return n;
}
std::array<std::atomic<bool>, 8>& enables()
{
    static std::array<std::atomic<bool>, 8> e {true, true, true, true, true, true, true, true};
    return e;
}
} // namespace

std::vector<std::string> portNames() { return names(); }
void publishPortNames(std::vector<std::string> n) { names() = std::move(n); }
void setInputEnabled(int slot, bool on) noexcept
{
    if (slot >= 0 && slot < 8)
        enables()[static_cast<std::size_t>(slot)].store(on, std::memory_order_relaxed);
}
bool inputEnabled(int slot) noexcept
{
    return slot < 0 || slot >= 8 || enables()[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
}

void setUiEventSink(UiEventSink s) { sink() = std::move(s); }

int drainUiEvents(core::midi::MidiHub& hub, int max)
{
    int n = 0;
    core::midi::UiEvent e;
    while (n < max && hub.popUi(e))
    {
        ++n;
        if (e.kind == core::midi::UiEvent::Kind::Tempo)
            tempo() = e.value;
        if (sink())
            sink()(e);
    }
    return n;
}

double lastFollowedTempo() noexcept { return tempo(); }
} // namespace io::midi
