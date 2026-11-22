// Sampling commands (spec 02-features/sampling.md SMP-009/010/025; state-and-undo.md, EDT-U5): what a finished take
// does to the document. Each is one undo step (a CommandGroup with its own label) built from AssignSample and the
// library's loadSample / unloadSample; undo restores references and values, never a file.
//
//   assignTake      SMP-010: the take (already a WAV in Recordings/, SMP-007) replaces the track's sample; its pool
//                   record's display name is "RECORDED TEMP." until it is saved. The caller keeps the replaced track
//                   state (replacedOf(), before executing) for discardTake.
//   saveAndReplace  SMP-009 SAVE & REPLACE: the saved selection's file becomes the track's sample (LIB-012 values).
//   discardTake     SMP-010 Discard (ours): the replaced reference and its region values come back (an empty track
//                   stays empty); the take file stays on disk (Recordings/, EDT-U5 — a command never deletes files).
//   unloadTrack     SMP-025 Unload: the track's reference is cleared (other settings stay, as LIB-012's unload).
#pragma once

#include "core/commands/Command.h"
#include "core/commands/library/LibraryCommands.h"
#include "core/model/enums/Enums.h"
#include "core/model/Sample.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace core::model
{
struct Project;
}

namespace core::commands::sampling
{
inline constexpr std::string_view kRecordedTempName = "RECORDED TEMP."; // SMP-010 (M p.33)

// The track state a take replaces (SMP-010): the reference (nullopt = the track was empty) and the region values.
struct ReplacedTrack
{
    std::optional<model::SampleRef> ref;
    std::int64_t start = 0, loopStart = 0, length = 0;
    model::StretchMode stretch = model::StretchMode::Off;
    bool padVelocity = true;
};
ReplacedTrack replacedOf(const model::Project&, int scene, int track);

// The take's SampleRef as assignTake stores it: displayName = "RECORDED TEMP.".
model::SampleRef takeRef(model::SampleRef fileRef);
bool isRecordedTemp(const model::Project&, int scene, int track);

std::unique_ptr<core::Command> assignTake(int scene, int track, const model::SampleRef& take,
                                          const library::LoadDefaults&);
std::unique_ptr<core::Command> saveAndReplace(int scene, int track, const model::SampleRef& saved,
                                              const library::LoadDefaults&);
std::unique_ptr<core::Command> discardTake(int scene, int track, const ReplacedTrack& replaced);
std::unique_ptr<core::Command> unloadTrack(int scene, int track);
} // namespace core::commands::sampling
