#include "core/commands/library/LibraryCommands.h"

#include "core/commands/AssignSample.h"
#include "core/commands/CommandGroup.h"
#include "core/commands/SetParam.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"

#include <string>

namespace core::commands::library
{
namespace
{
using model::ParamPath;
using model::Project;
using model::Scope;
using model::StretchMode;

constexpr std::string_view kLabel = "SAMPLE";

model::Track& trackAt(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].tracks[at.track];
}
std::int64_t& start(Project& p, const ParamPath& at)
{
    return trackAt(p, at).playback.start;
}
std::int64_t& loopStart(Project& p, const ParamPath& at)
{
    return trackAt(p, at).playback.loopStart;
}
std::int64_t& length(Project& p, const ParamPath& at)
{
    return trackAt(p, at).playback.length;
}
StretchMode& stretch(Project& p, const ParamPath& at)
{
    return trackAt(p, at).playback.stretchMode;
}
bool& velocity(Project& p, const ParamPath& at)
{
    return trackAt(p, at).ampEnv.velocitySensitive;
}

ParamPath at(int scene, int track, std::string_view id)
{
    ParamPath p{
        Scope::Track, static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(track & 15), 0, 0, 0, {}};
    if (auto pid = core::builtinParams().idOf(id))
        p.param = *pid;
    return p;
}

std::string path(int scene, int track, std::string_view field)
{
    return "scene[" + std::to_string(scene & 15) + "].track[" + std::to_string(track & 15) + "]." + std::string(field);
}

template <typename T>
void set(CommandGroup& g, int scene, int track, std::string_view id, typename SetParam<T>::Accessor field, T value)
{
    // Coalesce::No: two loads in a row are two undo steps, never merged into one.
    g.add(
        makeSetParam<T>(path(scene, track, id), kLabel, field, at(scene, track, id), value, SetParam<T>::Coalesce::No));
}

void addLoad(CommandGroup& g, int scene, int track, const model::SampleRef& ref, const LoadDefaults& d)
{
    g.add(std::make_unique<AssignSample>(static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(track & 15),
                                         ref));
    set<std::int64_t>(g, scene, track, "playback.start", &start, 0);
    set<std::int64_t>(g, scene, track, "playback.loopStart", &loopStart, 0); // LOOP START = START
    set<std::int64_t>(g, scene, track, "playback.length", &length, d.frames);
    set<StretchMode>(g, scene, track, "playback.timeStretch", &stretch,
                     d.loop ? StretchMode::MasterTempo : StretchMode::Off);
    // pad VELOCITY = the envelope's velocity sensitivity (TRK-032): ParamSpec "track.padVelocity", model path
    // "scene[s].track[t].padVelocity" (ParamPath grammar: "track." is dropped), as core/commands/track writes it.
    g.add(makeSetParam<bool>(path(scene, track, "padVelocity"), kLabel, &velocity, at(scene, track, "track.padVelocity"),
                             ! d.loop, SetParam<bool>::Coalesce::No));
}
} // namespace

LoadDefaults classify(std::int64_t frames, double rate, std::optional<double> bpm, std::optional<double> bars,
                      bool hasLoopPoint) noexcept
{
    LoadDefaults d;
    d.frames = frames < 0 ? 0 : frames;
    const bool tempoKnown = (bpm && *bpm > 0) || (bars && *bars > 0);
    const double oneBarAt120 = rate * 2.0; // 4 beats at 120 BPM = 2 s
    d.loop = tempoKnown || (hasLoopPoint && rate > 0 && static_cast<double>(frames) >= oneBarAt120);
    return d;
}

std::unique_ptr<core::Command> loadSample(int scene, int track, const model::SampleRef& ref, const LoadDefaults& d)
{
    auto g = std::make_unique<CommandGroup>(std::string(kLabel));
    addLoad(*g, scene, track, ref, d);
    return g;
}

std::unique_ptr<core::Command> unloadSample(int scene, int track)
{
    return std::make_unique<AssignSample>(static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(track & 15),
                                          model::SampleRef{});
}

std::unique_ptr<core::Command> loadFolder(int scene, std::span<const model::SampleRef> first16)
{
    return loadFolder(scene, first16, {});
}

std::unique_ptr<core::Command> loadFolder(int scene, std::span<const model::SampleRef> first16,
                                          std::span<const LoadDefaults> defaults)
{
    auto g = std::make_unique<CommandGroup>(std::string(kLabel));
    for (std::size_t i = 0; i < first16.size() && i < 16; ++i)
        addLoad(*g, scene, static_cast<int>(i), first16[i], i < defaults.size() ? defaults[i] : LoadDefaults{});
    return g;
}
} // namespace core::commands::library
