#include "core/commands/track/TrackCommands.h"

#include "core/commands/SetParam.h"
#include "core/model/Project.h"

#include <charconv>
#include <cstdio>
#include <string>

namespace core::commands::track
{
namespace
{
using model::ParamPath;
using model::Project;
using model::Scope;

model::Track& at(Project& p, const ParamPath& a) { return p.scenes[a.scene & 15].tracks[a.track & 15]; }

bool& muteField(Project& p, const ParamPath& a) { return at(p, a).mute; }
bool& soloField(Project& p, const ParamPath& a) { return at(p, a).solo; }
model::TrackMode& modeField(Project& p, const ParamPath& a) { return at(p, a).mode; }
model::TriggerMode& triggerField(Project& p, const ParamPath& a) { return at(p, a).triggerMode; }
int& chokeField(Project& p, const ParamPath& a) { return at(p, a).chokeGroup; }
int& colourField(Project& p, const ParamPath& a) { return at(p, a).colour; }
bool& velocityField(Project& p, const ParamPath& a) { return at(p, a).ampEnv.velocitySensitive; }
model::PluginTrack& pluginField(Project& p, const ParamPath& a) { return at(p, a).plugin; }
bool& patternMuteField(Project& p, const ParamPath& a)
{
    return p.scenes[a.scene & 15].patterns[a.pattern & 15].trackMutes[a.track & 15];
}

// The fields, by the path tail = ParamSpec id without "track." (ParamPath grammar, core/model/paths).
struct Field
{
    std::string_view tail, id, label;
};
constexpr Field kMute {"mute", "track.mute", "MUTE"};
constexpr Field kMode {"mode", "track.mode", "TRACK TYPE"};
constexpr Field kTrigger {"triggerMode", "track.triggerMode", "TRIGGER MODE"};
constexpr Field kChoke {"chokeGroup", "track.chokeGroup", "CHOKE"};
constexpr Field kColour {"padColor", "track.padColor", "PAD COLOR"};
constexpr Field kVelocity {"padVelocity", "track.padVelocity", "VELOCITY"};
constexpr Field kSolo {"solo", "track.solo", "SOLO"}; // not in kFields: ui::forwardModelChanges posts it
constexpr Field kPlugin {"plugin", "track.plugin", "SELECT PLUGIN"}; // not in kFields: plugin host swaps audio node
constexpr const Field* kFields[] = {&kMute, &kMode, &kTrigger, &kChoke, &kColour, &kVelocity};

std::string pathOf(int scene, int track, std::string_view tail)
{
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "scene[%d].track[%d].%.*s", scene & 15, track & 15,
                                static_cast<int>(tail.size()), tail.data());
    return std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0));
}

ParamPath trackAt(int scene, int track)
{
    ParamPath p {};
    p.scope = Scope::Track;
    p.scene = static_cast<std::uint8_t>(scene & 15);
    p.track = static_cast<std::uint8_t>(track & 15);
    return p;
}

template <typename T>
std::unique_ptr<Command> make(const Field& f, int scene, int track, typename SetParam<T>::Accessor field, T value)
{
    return makeSetParam<T>(pathOf(scene, track, f.tail), f.label, field, trackAt(scene, track), value,
                           SetParam<T>::Coalesce::No);
}

// "scene[3].track[7].mute" → 3, 7, "mute"; false for any other shape.
bool parseTrackPath(std::string_view s, int& scene, int& track, std::string_view& tail) noexcept
{
    auto number = [&s](std::string_view prefix, int& out) {
        if (s.substr(0, prefix.size()) != prefix)
            return false;
        s.remove_prefix(prefix.size());
        const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        if (r.ec != std::errc {} || r.ptr == s.data() || r.ptr == s.data() + s.size() || *r.ptr != ']')
            return false;
        s.remove_prefix(static_cast<std::size_t>(r.ptr - s.data()) + 1);
        return true;
    };
    if (! number("scene[", scene) || ! number(".track[", track) || s.substr(0, 1) != ".")
        return false;
    tail = s.substr(1);
    return scene >= 0 && scene < 16 && track >= 0 && track < 16;
}
} // namespace

std::unique_ptr<Command> setMute(int scene, int track, bool mute)
{
    return make<bool>(kMute, scene, track, &muteField, mute);
}
std::unique_ptr<Command> setMode(int scene, int track, model::TrackMode mode)
{
    return make<model::TrackMode>(kMode, scene, track, &modeField, mode);
}
std::unique_ptr<Command> setTriggerMode(int scene, int track, model::TriggerMode m)
{
    return make<model::TriggerMode>(kTrigger, scene, track, &triggerField, m);
}
std::unique_ptr<Command> setChokeGroup(int scene, int track, int group)
{
    return make<int>(kChoke, scene, track, &chokeField, group < 0 ? 0 : (group > 8 ? 8 : group));
}
std::unique_ptr<Command> setPadColour(int scene, int track, int colour)
{
    return make<int>(kColour, scene, track, &colourField, colour & 15);
}
std::unique_ptr<Command> setPadVelocity(int scene, int track, bool velocitySensitive)
{
    return make<bool>(kVelocity, scene, track, &velocityField, velocitySensitive);
}
std::unique_ptr<Command> setPlugin(int scene, int track, model::PluginTrack plugin)
{
    return make<model::PluginTrack>(kPlugin, scene, track, &pluginField, std::move(plugin));
}

bool isMuted(const Project& p, int scene, int track) noexcept
{
    const auto& s = p.scenes[scene & 15];
    return s.tracks[track & 15].mute || s.patterns[p.settings.currentPattern & 15].trackMutes[track & 15];
}

std::unique_ptr<Command> setPatternMute(int scene, int pattern, int track, bool mute)
{
    char buf[48];
    const int n = std::snprintf(buf, sizeof buf, "scene[%d].pattern[%d].mutes", scene & 15, pattern & 15);
    ParamPath a {};
    a.scope = Scope::Pattern;
    a.scene = static_cast<std::uint8_t>(scene & 15);
    a.pattern = static_cast<std::uint8_t>(pattern & 15);
    a.track = static_cast<std::uint8_t>(track & 15);
    return makeSetParam<bool>(std::string(buf, static_cast<std::size_t>(n > 0 ? n : 0)), "MUTE", &patternMuteField, a,
                              mute, SetParam<bool>::Coalesce::No);
}

std::unique_ptr<Command> setMuted(const Project& p, int scene, int track, bool mute)
{
    if (isMuted(p, scene, track) == mute)
        return nullptr;
    const auto& s = p.scenes[scene & 15];
    const int current = p.settings.currentPattern & 15;
    if (mute || ! s.tracks[track & 15].mute)
        return setPatternMute(scene, current, track, mute);
    // Unmuting a track-level mute (an imported muteSetting, a pre-per-pattern project): it moves into the other
    // patterns' own mutes, so only the current pattern unmutes.
    auto group = std::make_unique<CommandGroup>("MUTE");
    group->add(setMute(scene, track, false));
    for (int q = 0; q < model::kPatterns; ++q)
    {
        const bool want = q != current;
        if (s.patterns[static_cast<std::size_t>(q)].trackMutes[track & 15] != want)
            group->add(setPatternMute(scene, q, track, want));
    }
    return group;
}

std::unique_ptr<Command> toggleMute(const Project& p, int scene, int track)
{
    return setMuted(p, scene, track, ! isMuted(p, scene, track));
}

std::unique_ptr<Command> setSolo(int scene, int track, bool solo)
{
    return make<bool>(kSolo, scene, track, &soloField, solo);
}
std::unique_ptr<Command> toggleSolo(const Project& p, int scene, int track)
{
    return setSolo(scene, track, ! p.scenes[scene & 15].tracks[track & 15].solo);
}

std::unique_ptr<CommandGroup> muteOthers(const Project& p, int scene, int track)
{
    const int n = track & 15;
    bool soloLike = ! isMuted(p, scene, n); // already "every track but n muted"?
    for (int t = 0; t < 16 && soloLike; ++t)
        if (t != n && ! isMuted(p, scene, t))
            soloLike = false;
    auto group = std::make_unique<CommandGroup>("MUTE");
    for (int t = 0; t < 16; ++t)
        if (auto c = setMuted(p, scene, t, soloLike ? false : t != n))
            group->add(std::move(c));
    return group->empty() ? nullptr : std::move(group);
}

void forEachTrackChange(const ChangeSet& changes, const Project& p,
                        const std::function<void(int, int, std::string_view, float)>& emit)
{
    for (const auto& path : changes.paths())
    {
        int scene = 0, track = 0;
        std::string_view tail;
        if (! parseTrackPath(path, scene, track, tail))
            continue;
        const auto& t = p.scenes[static_cast<std::size_t>(scene)].tracks[static_cast<std::size_t>(track)];
        for (const Field* f : kFields)
        {
            if (f->tail != tail)
                continue;
            float v = 0.0f;
            if (f == &kMute)
                v = t.mute ? 1.0f : 0.0f;
            else if (f == &kMode)
                v = static_cast<float>(t.mode);
            else if (f == &kTrigger)
                v = static_cast<float>(t.triggerMode);
            else if (f == &kChoke)
                v = static_cast<float>(t.chokeGroup);
            else if (f == &kColour)
                v = static_cast<float>(t.colour);
            else
                v = t.ampEnv.velocitySensitive ? 1.0f : 0.0f;
            emit(scene, track, f->id, v);
        }
    }
}
} // namespace core::commands::track
