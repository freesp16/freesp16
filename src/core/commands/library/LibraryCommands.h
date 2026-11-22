// Library commands (LIB-012, LIB-013; state-and-undo.md, EDT-007, EDT-U5): load a sample onto a track, unload it, load
// a folder across tracks. Each is one undo step (a CommandGroup labelled "SAMPLE") built from
// AssignSample (the reference + pool record) and SetParams for the values LIB-012 resets. Undo restores the previous
// reference and those values — never a file (EDT-U5); the engine follows through the bridge (TrackSampleSync).
//
// LIB-012: loading replaces only the sample; other track settings are kept, except START = 0, LOOP START = START,
// LENGTH = the whole file, TIME STRETCH and pad VELOCITY, which take the analysis-based defaults (PLY-034, TRK-032):
// a one-shot gets TIME STRETCH OFF + VELOCITY ON, a loop MASTER TEMPO + VELOCITY OFF. Until the render
// analysis (REN-024) feeds it the classifier is our own heuristic: a sidecar BPM or bars, or a length of at least
// one bar at 120 BPM together with a loop point in the file, marks a loop.
#pragma once

#include "core/commands/Command.h"
#include "core/model/Sample.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace core::commands::library
{
struct LoadDefaults
{
    std::int64_t frames = 0; // LENGTH (0 = unknown: LENGTH is set to 0, the voice plays to the end)
    bool loop = false;       // the classifier's verdict
};

// The heuristic above. `rate` is the buffer's (engine) rate.
LoadDefaults classify(std::int64_t frames, double rate, std::optional<double> bpm, std::optional<double> bars,
                      bool hasLoopPoint) noexcept;

std::unique_ptr<core::Command> loadSample(int scene, int track, const model::SampleRef&,
                                          const LoadDefaults&);    // LIB-012
std::unique_ptr<core::Command> unloadSample(int scene, int track); // the reference only; other settings stay
// LIB-013: first16[i] onto tracks 1–16 (index i) of `scene`; fewer files leave the remaining tracks untouched.
std::unique_ptr<core::Command> loadFolder(int scene, std::span<const model::SampleRef> first16);
std::unique_ptr<core::Command> loadFolder(int scene, std::span<const model::SampleRef> first16,
                                          std::span<const LoadDefaults> defaults);
} // namespace core::commands::library
