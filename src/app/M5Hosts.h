// The composition root's hosts of the render and sampling screens: RENDER AUDIO's ui::m5::RenderHost over the
// Document's services::render::RenderService, and the SAMPLING/EDIT + LIVE SAMPLING screens' ui::m5::SamplingHost over
// its services::sampling::SamplingSession. Each is created and installed by app/Document.cpp and uninstalled before
// the services it refers to go. Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "services/library/SampleLibrary.h"
#include "services/library/SamplePool.h"
#include "services/library/Workers.h"
#include "services/render/RenderService.h"
#include "services/sampling/SamplingSession.h"
#include "ui/screens/render/RenderHost.h"
#include "ui/screens/sampling/SamplingHost.h"

#include <functional>
#include <optional>
#include <string_view>
#include <memory>
#include <string>

namespace app
{
struct M5HostEnv
{
    core::CommandBus& bus;
    services::library::SamplePool& pool;
    services::library::Workers& workers;
    engine::EngineBridge& bridge;
    std::function<void(const std::string&)> log;
    // REN-004 / SMP-009 / SMP-025: put a file on a track with the undoable LOAD (TrackLoader).
    std::function<void(int scene, int track, const juce::File&, std::function<void(bool ok)>)> loadToTrack;
    juce::File recordings; // <library>/Recordings (SMP-007)
    std::function<float(std::string_view id)> value;                              // a parameter's value (shell's)
    std::function<std::optional<juce::File>(const std::string& path)> resolve;    // a model path → its file
    std::function<const core::audio::SampleBuffer*(int scene, int track)> current; // the buffer a track plays
};

std::unique_ptr<ui::m5::RenderHost> makeRenderHost(M5HostEnv, services::render::RenderService&);
std::unique_ptr<ui::m5::SamplingHost> makeSamplingHost(M5HostEnv, services::sampling::SamplingSession&);
} // namespace app
