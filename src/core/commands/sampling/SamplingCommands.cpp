#include "core/commands/sampling/SamplingCommands.h"

#include "core/commands/AssignSample.h"
#include "core/commands/CommandGroup.h"
#include "core/commands/SetParam.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"

#include <string>

namespace core::commands::sampling
{
namespace
{
using model::ParamPath;
using model::Project;
using model::Scope;
using model::StretchMode;
constexpr std::string_view kDiscard = "DISCARD";

model::Track& trackAt(Project& p, const ParamPath& at) { return p.scenes[at.scene].tracks[at.track]; }
std::int64_t& start(Project& p, const ParamPath& at) { return trackAt(p, at).playback.start; }
std::int64_t& loopStart(Project& p, const ParamPath& at) { return trackAt(p, at).playback.loopStart; }
std::int64_t& length(Project& p, const ParamPath& at) { return trackAt(p, at).playback.length; }
StretchMode& stretch(Project& p, const ParamPath& at) { return trackAt(p, at).playback.stretchMode; }
bool& velocity(Project& p, const ParamPath& at) { return trackAt(p, at).ampEnv.velocitySensitive; }

ParamPath at(int scene, int track, std::string_view id)
{
    ParamPath p {Scope::Track, static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(track & 15), 0, 0, 0, {}};
    if (auto pid = core::builtinParams().idOf(id))
        p.param = *pid;
    return p;
}
std::string path(int scene, int track, std::string_view field)
{
    return "scene[" + std::to_string(scene & 15) + "].track[" + std::to_string(track & 15) + "]." + std::string(field);
}
template <typename T>
void set(CommandGroup& g, int scene, int track, std::string_view id, std::string_view field,
         typename SetParam<T>::Accessor acc, T value)
{
    g.add(makeSetParam<T>(path(scene, track, field), kDiscard, acc, at(scene, track, id), value,
                          SetParam<T>::Coalesce::No));
}
std::unique_ptr<core::Command> wrap(std::string label, std::unique_ptr<core::Command> inner)
{
    auto g = std::make_unique<CommandGroup>(std::move(label));
    g->add(std::move(inner));
    return g;
}
} // namespace

ReplacedTrack replacedOf(const Project& p, int scene, int track)
{
    const auto& t = p.scenes[static_cast<std::size_t>(scene & 15)].tracks[static_cast<std::size_t>(track & 15)];
    ReplacedTrack r;
    if (! t.playback.sample.empty())
        if (const auto* e = p.sampleRefs.find(t.playback.sample))
            r.ref = *e;
    r.start = t.playback.start;
    r.loopStart = t.playback.loopStart;
    r.length = t.playback.length;
    r.stretch = t.playback.stretchMode;
    r.padVelocity = t.ampEnv.velocitySensitive;
    return r;
}

model::SampleRef takeRef(model::SampleRef fileRef)
{
    fileRef.displayName = std::string(kRecordedTempName);
    return fileRef;
}

bool isRecordedTemp(const Project& p, int scene, int track)
{
    const auto& t = p.scenes[static_cast<std::size_t>(scene & 15)].tracks[static_cast<std::size_t>(track & 15)];
    const auto* e = t.playback.sample.empty() ? nullptr : p.sampleRefs.find(t.playback.sample);
    return e != nullptr && e->displayName == kRecordedTempName;
}

std::unique_ptr<core::Command> assignTake(int scene, int track, const model::SampleRef& take,
                                          const library::LoadDefaults& d)
{
    return wrap("RECORD TAKE", library::loadSample(scene, track, takeRef(take), d));
}

std::unique_ptr<core::Command> saveAndReplace(int scene, int track, const model::SampleRef& saved,
                                              const library::LoadDefaults& d)
{
    return wrap("SAVE & REPLACE", library::loadSample(scene, track, saved, d));
}

std::unique_ptr<core::Command> discardTake(int scene, int track, const ReplacedTrack& r)
{
    auto g = std::make_unique<CommandGroup>(std::string(kDiscard));
    g->add(std::make_unique<AssignSample>(static_cast<std::uint8_t>(scene & 15), static_cast<std::uint8_t>(track & 15),
                                          r.ref ? *r.ref : model::SampleRef {}));
    set<std::int64_t>(*g, scene, track, "playback.start", "playback.start", &start, r.start);
    set<std::int64_t>(*g, scene, track, "playback.loopStart", "playback.loopStart", &loopStart, r.loopStart);
    set<std::int64_t>(*g, scene, track, "playback.length", "playback.length", &length, r.length);
    set<StretchMode>(*g, scene, track, "playback.timeStretch", "playback.timeStretch", &stretch, r.stretch);
    set<bool>(*g, scene, track, "track.padVelocity", "padVelocity", &velocity, r.padVelocity);
    return g;
}

std::unique_ptr<core::Command> unloadTrack(int scene, int track)
{
    return wrap("UNLOAD", library::unloadSample(scene, track));
}
} // namespace core::commands::sampling
