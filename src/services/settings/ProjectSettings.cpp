#include "services/settings/ProjectSettings.h"

#include "core/commands/SetParam.h"
#include "core/commands/ribbon/RibbonCommands.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace services::settings
{
namespace
{
namespace m = core::model;
using core::ParamScope;
using core::ParamType;

constexpr std::string_view kEnable[] = {"DISABLE", "ENABLE"}; // utility.md MIDI rows
constexpr std::string_view kOffOn[] = {"OFF", "ON"};
constexpr std::string_view kQuantize[] = {"STEP", "BAR", "PATTERN"}; // UI-327
constexpr int kPitchOptions[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 24}; // RIB-002: N Bipolar Semitones

constexpr core::ParamSpec boolSpec(std::string_view id, std::string_view label, std::span<const std::string_view> l)
{
    core::ParamSpec s {};
    s.id = id;
    s.label = label;
    s.shortLabel = label;
    s.group = "PROJECT";
    s.type = ParamType::Enum;
    s.minValue = 0;
    s.maxValue = 1;
    s.enumLabels = l;
    s.scope = ParamScope::Project;
    return s;
}
constexpr core::ParamSpec enumSpec(std::string_view id, std::string_view label, std::span<const std::string_view> l,
                                   float def)
{
    auto s = boolSpec(id, label, l);
    s.maxValue = static_cast<float>(l.size() - 1);
    s.defaultValue = def;
    return s;
}
constexpr core::ParamSpec intSpec(std::string_view id, std::string_view label, float lo, float hi, float def)
{
    core::ParamSpec s {};
    s.id = id;
    s.label = label;
    s.shortLabel = label;
    s.group = "PROJECT";
    s.type = ParamType::Integer;
    s.minValue = lo;
    s.maxValue = hi;
    s.defaultValue = def;
    s.scope = ParamScope::Project;
    return s;
}

const ProjectField kFields[] = {
    {"settings.pitchRange", "TOUCH STRIP SETTING (PITCH RANGE)", intSpec("settings.pitchRange", "PITCH RANGE", 0, 12, 1),
     1, {}},
    {"project.patternQuantize", "PATTERN QUANTIZE", enumSpec("project.patternQuantize", "PATTERN QUANTIZE", kQuantize, 2),
     1, {}},
    {"seq.padStartsSequence", "PAD STARTS SEQUENCE", boolSpec("seq.padStartsSequence", "PAD STARTS SEQUENCE", kOffOn), 1,
     "seq.padStartsSequence"},
    {"midi.in.clock", "CLOCK", boolSpec("midi.in.clock", "CLOCK", kEnable), 1, "midi.in.clock"},
    {"midi.in.startStop", "START/STOP/CONTINUE", boolSpec("midi.in.startStop", "START/STOP/CONTINUE", kEnable), 1,
     "midi.in.startStop"},
    {"midi.in.noteCc", "NOTE/CC", boolSpec("midi.in.noteCc", "NOTE/CC", kEnable), 1, "midi.in.noteCc"},
    {"midi.in.songPosition", "SONG POSITION", boolSpec("midi.in.songPosition", "SONG POSITION", kEnable), 1,
     "midi.in.songPosition"},
    {"midi.out.clock", "CLOCK", boolSpec("midi.out.clock", "CLOCK", kEnable), 1, "midi.out.clock"},
    {"midi.out.startStop", "START/STOP/CONTINUE", boolSpec("midi.out.startStop", "START/STOP/CONTINUE", kEnable), 1,
     "midi.out.startStop"},
    {"midi.out.noteCc", "NOTE/CC", boolSpec("midi.out.noteCc", "NOTE/CC", kEnable), 1, "midi.out.noteCc"},
    {"midi.out.songPosition", "SONG POSITION", boolSpec("midi.out.songPosition", "SONG POSITION", kEnable), 1,
     "midi.out.songPosition"},
    {"midi.in.channel", "RECEIVE CH", intSpec("midi.in.channel", "RECEIVE CH", 0, 16, 1), 16, "midi.in.channel"},
    {"midi.out.channel", "SEND CH", intSpec("midi.out.channel", "SEND CH", 0, 16, 1), 16, "midi.out.channel"},
};

bool startsWith(std::string_view s, std::string_view p) noexcept { return s.substr(0, p.size()) == p; }

// The four switches of one direction: index 0–3 = clock, startStop, noteCc, songPosition.
int switchIndex(std::string_view id) noexcept
{
    const auto dot = id.rfind('.');
    const auto name = id.substr(dot + 1);
    return name == "clock" ? 0 : name == "startStop" ? 1 : name == "noteCc" ? 2 : name == "songPosition" ? 3 : -1;
}
bool& switchOf(m::MidiPortSettings& s, int i) noexcept
{
    switch (i)
    {
        case 0: return s.clock;
        case 1: return s.startStop;
        case 2: return s.noteCc;
        default: return s.songPosition;
    }
}
constexpr std::string_view kSwitchNames[] = {"clock", "startStop", "noteCc", "songPosition"};

template <int Dir, int I> bool& midiSwitch(m::Project& p, const m::ParamPath&)
{
    return switchOf(Dir == 0 ? p.midi.input : p.midi.output, I);
}
template <int Dir> int& midiChannel(m::Project& p, const m::ParamPath& at)
{
    return (Dir == 0 ? p.midi.input : p.midi.output).channels[at.track & 15];
}
template <int Dir> std::array<int, 16>& midiChannels(m::Project& p, const m::ParamPath&)
{
    return (Dir == 0 ? p.midi.input : p.midi.output).channels;
}
bool& padStarts(m::Project& p, const m::ParamPath&) { return p.settings.padStartsSequence; }
m::QuantizeUnit& patternQuantize(m::Project& p, const m::ParamPath&) { return p.settings.patternQuantize; }

using SwitchAccessor = bool& (*)(m::Project&, const m::ParamPath&);
constexpr SwitchAccessor kSwitches[2][4] = {
    {&midiSwitch<0, 0>, &midiSwitch<0, 1>, &midiSwitch<0, 2>, &midiSwitch<0, 3>},
    {&midiSwitch<1, 0>, &midiSwitch<1, 1>, &midiSwitch<1, 2>, &midiSwitch<1, 3>}};

int quantizeOption(m::QuantizeUnit u) noexcept
{
    switch (u)
    {
        case m::QuantizeUnit::Bar: return 1;
        case m::QuantizeUnit::Pattern: return 2;
        default: return 0; // Off / Step / Beat: shown as STEP (as SceneSync maps them)
    }
}

int pitchOption(const m::RibbonSettings& r) noexcept
{
    if (r.pitchTopCents != -r.pitchBottomCents || r.pitchTopCents % 100 != 0)
        return -1;
    const int n = r.pitchTopCents / 100;
    for (int i = 0; i < static_cast<int>(std::size(kPitchOptions)); ++i)
        if (kPitchOptions[i] == n)
            return i;
    return -1;
}

std::string pathOf(const ProjectField& f, int index)
{
    if (f.id == "settings.pitchRange")
        return "ribbon";
    if (f.id == "project.patternQuantize")
        return "settings.patternQuantize";
    if (f.id == "seq.padStartsSequence")
        return "settings.padStartsSequence";
    const bool in = startsWith(f.id, "midi.in.");
    const std::string dir = in ? "midi.input." : "midi.output.";
    if (f.count > 1)
        return dir + "channels[" + std::to_string(index) + "]";
    return dir + std::string(kSwitchNames[switchIndex(f.id)]);
}
} // namespace

std::span<const ProjectField> projectFields() noexcept { return kFields; }

const ProjectField* findProjectField(std::string_view id) noexcept
{
    for (const auto& f : kFields)
        if (f.id == id)
            return &f;
    return nullptr;
}

int pitchRangeSemitones(int option) noexcept
{
    return kPitchOptions[std::clamp(option, 0, static_cast<int>(std::size(kPitchOptions)) - 1)];
}

float projectValue(const m::Project& p, const ProjectField& f, int index)
{
    if (f.id == "settings.pitchRange")
        return static_cast<float>(pitchOption(p.ribbon));
    if (f.id == "project.patternQuantize")
        return static_cast<float>(quantizeOption(p.settings.patternQuantize));
    if (f.id == "seq.padStartsSequence")
        return p.settings.padStartsSequence ? 1.0f : 0.0f;
    const auto& port = startsWith(f.id, "midi.in.") ? p.midi.input : p.midi.output;
    if (f.count > 1)
        return static_cast<float>(port.channels[static_cast<std::size_t>(index & 15)]);
    auto copy = port;
    return switchOf(copy, switchIndex(f.id)) ? 1.0f : 0.0f;
}

std::unique_ptr<core::Command> setProjectValue(const m::Project& p, const ProjectField& f, int index, float value)
{
    const int v = static_cast<int>(std::lround(f.spec.clamp(value)));
    if (static_cast<float>(v) == projectValue(p, f, index))
        return nullptr;
    const m::ParamPath at {m::Scope::Project, 0, static_cast<std::uint8_t>(index & 15), 0, 0, 0, core::ParamId {}};
    if (f.id == "settings.pitchRange")
    {
        const int cents = pitchRangeSemitones(v) * 100;
        return core::commands::ribbon::setPitchRange(p, -cents, cents);
    }
    if (f.id == "project.patternQuantize")
    {
        static constexpr m::QuantizeUnit units[] = {m::QuantizeUnit::Step, m::QuantizeUnit::Bar, m::QuantizeUnit::Pattern};
        return core::makeSetParam<m::QuantizeUnit>(pathOf(f, 0), f.label, &patternQuantize, at,
                                                   units[std::clamp(v, 0, 2)], core::SetParam<m::QuantizeUnit>::Coalesce::No);
    }
    if (f.id == "seq.padStartsSequence")
        return core::makeSetParam<bool>(pathOf(f, 0), f.label, &padStarts, at, v != 0, core::SetParam<bool>::Coalesce::No);
    const int dir = startsWith(f.id, "midi.in.") ? 0 : 1;
    if (f.count > 1)
        return core::makeSetParam<int>(pathOf(f, index), dir == 0 ? "RECEIVE CH" : "SEND CH",
                                       dir == 0 ? &midiChannel<0> : &midiChannel<1>, at, v,
                                       core::SetParam<int>::Coalesce::No);
    return core::makeSetParam<bool>(pathOf(f, 0), f.label, kSwitches[dir][switchIndex(f.id)], at, v != 0,
                                    core::SetParam<bool>::Coalesce::No);
}

std::unique_ptr<core::Command> resetChannelMapping(const m::Project& p, bool input)
{
    std::array<int, 16> identity {};
    for (int t = 0; t < 16; ++t)
        identity[static_cast<std::size_t>(t)] = t + 1;
    if ((input ? p.midi.input : p.midi.output).channels == identity)
        return nullptr;
    return core::makeSetParam<std::array<int, 16>>(input ? "midi.input.channels" : "midi.output.channels",
                                                   "CHANNEL MAPPING RESET", input ? &midiChannels<0> : &midiChannels<1>,
                                                   {m::Scope::Project, 0, 0, 0, 0, 0, core::ParamId {}}, identity,
                                                   core::SetParam<std::array<int, 16>>::Coalesce::No);
}

int formatProjectValue(const m::Project& p, const ProjectField& f, int index, float value, char* buf, int n)
{
    (void)index; // the value is given; the index only names the track for a channel
    const auto size = static_cast<std::size_t>(n);
    const int v = static_cast<int>(std::lround(value));
    if (f.id == "settings.pitchRange")
    {
        if (v >= 0)
            return std::snprintf(buf, size, pitchRangeSemitones(v) == 1 ? "%d Bipolar Semitone" : "%d Bipolar Semitones",
                                 pitchRangeSemitones(v));
        // An imported asymmetric range (utility.md): "−200 / +700 cents".
        return std::snprintf(buf, size, "\xE2\x88\x92%d / +%d cents", -p.ribbon.pitchBottomCents, p.ribbon.pitchTopCents);
    }
    if (f.count > 1)
        return v <= 0 ? std::snprintf(buf, size, "OFF") : std::snprintf(buf, size, "CH %d", v);
    return f.spec.formatValue(static_cast<float>(v), buf, n);
}

void forwardSettingsChanges(const core::ChangeSet& changes, const m::Project& p, engine::EngineBridge& bridge,
                            const core::ParamRegistry& reg)
{
    auto post = [&](std::string_view id, std::uint16_t scope, float v)
    {
        if (const auto pid = reg.idOf(id))
            bridge.post(engine::ParamChange {*pid, scope, v, 0});
    };
    for (const auto& path : changes.paths())
    {
        const std::string_view s = path;
        if (s == "settings" || s == "settings.padStartsSequence")
            post("seq.padStartsSequence", 0, p.settings.padStartsSequence ? 1.0f : 0.0f);
        for (int dir = 0; dir < 2; ++dir)
        {
            const std::string_view lead = dir == 0 ? "midi.input" : "midi.output";
            const std::string_view ids = dir == 0 ? "midi.in." : "midi.out.";
            if (! startsWith(s, lead) && s != "midi")
                continue;
            const auto rest = s == "midi" ? std::string_view {} : s.substr(lead.size());
            const auto& port = dir == 0 ? p.midi.input : p.midi.output;
            auto copy = port;
            for (int i = 0; i < 4; ++i)
                if (rest.empty() || rest == std::string(".") + std::string(kSwitchNames[i]))
                    post(std::string(ids) + std::string(kSwitchNames[i]), 0, switchOf(copy, i) ? 1.0f : 0.0f);
            for (int t = 0; t < 16; ++t)
                if (rest.empty() || rest == ".channels" || rest == ".channels[" + std::to_string(t) + "]")
                    post(std::string(ids) + "channel", static_cast<std::uint16_t>(t),
                         static_cast<float>(port.channels[static_cast<std::size_t>(t)]));
        }
    }
}
} // namespace services::settings
