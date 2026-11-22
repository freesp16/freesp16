// Model field accessors and command factories shared by the command tests and the bench.
#pragma once

#include "core/commands/AssignSample.h"
#include "core/commands/CommandBusAccess.h"
#include "core/commands/SetParam.h"
#include "core/model/Project.h"

#include <memory>
#include <string>

namespace cmdtest
{
using core::model::ParamPath;
using core::model::Project;
using core::model::Scope;

inline int& trackVolume(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].tracks[at.track].volume;
}
inline int& trackPan(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].tracks[at.track].pan;
}
inline double& envAttack(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].tracks[at.track].ampEnv.attack;
}
inline bool& trackMute(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].tracks[at.track].mute;
}
inline std::string& trackName(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].tracks[at.track].name;
}
inline double& tempo(Project& p, const ParamPath&)
{
    return p.settings.tempo;
}
inline core::model::Step& step(Project& p, const ParamPath& at)
{
    return p.scenes[at.scene].patterns[at.pattern].steps[at.step][at.track];
}

inline std::string trackPath(int scene, int track, const char* field)
{
    return "scene[" + std::to_string(scene) + "].track[" + std::to_string(track) + "]." + field;
}

inline std::unique_ptr<core::Command> setVolume(int scene, int track, int value)
{
    return core::makeSetParam<int>(
        trackPath(scene, track, "volume"), "VOLUME", &trackVolume,
        ParamPath{Scope::Track, static_cast<std::uint8_t>(scene), static_cast<std::uint8_t>(track), 0, 0, 0, {}},
        value);
}

inline std::unique_ptr<core::Command> setPan(int scene, int track, int value)
{
    return core::makeSetParam<int>(
        trackPath(scene, track, "pan"), "PAN", &trackPan,
        ParamPath{Scope::Track, static_cast<std::uint8_t>(scene), static_cast<std::uint8_t>(track), 0, 0, 0, {}},
        value);
}

inline std::unique_ptr<core::Command> setTempo(double bpm)
{
    return core::makeSetParam<double>("settings.tempo", "BPM", &tempo, ParamPath{Scope::Project, 0, 0, 0, 0, 0, {}},
                                      bpm);
}

inline std::unique_ptr<core::Command> setStep(int scene, int pattern, int stepIndex, int track, core::model::Step value)
{
    std::string path = "scene[" + std::to_string(scene) + "].pattern[" + std::to_string(pattern) + "].step[" +
                       std::to_string(stepIndex) + "].track[" + std::to_string(track) + "]";
    return core::makeSetParam<core::model::Step>(std::move(path), "STEP", &step,
                                                 ParamPath{Scope::Step,
                                                           static_cast<std::uint8_t>(scene),
                                                           static_cast<std::uint8_t>(track),
                                                           static_cast<std::uint8_t>(pattern),
                                                           static_cast<std::uint8_t>(stepIndex),
                                                           0,
                                                           {}},
                                                 value, core::SetParam<core::model::Step>::Coalesce::No);
}

// A bus with a default project (the owner's job, CommandBusAccess.h).
inline void giveProject(core::CommandBus& bus)
{
    core::commands::replaceProject(bus, std::make_unique<Project>());
}

inline core::model::SampleRef sampleRef(const std::string& id)
{
    core::model::SampleRef r;
    r.id = id;
    r.path = "$Lib/" + id + ".wav";
    r.displayName = id;
    return r;
}
} // namespace cmdtest
