// Project settings (02-features/settings.md SET-001/002; 07-ui-design/screens/utility.md §Rows, PROJECT SETTING
// sections): the UTILITY values that live in the project model — saved with the project, undoable, marking it edited.
// Each field names its model value, the undoable command that sets it (one core::SetParam per commit, so an edit is
// one undo step) and the model path the bus reports, and the engine parameter the change is forwarded
// as (forwardSettingsChanges, a bridge-hook half like ui::forwardModelChanges).
//
// These are not ParamSpecs of their own in core/params/catalogue/settings.cpp: the MIDI enables and channels are the
// MIDI area's ParamSpecs (midi.in.clock … midi.out.channel, catalogue/midi.cpp) and the pattern quantise /
// pad-starts-sequence ones the scene's / sequencer's; registering them again would duplicate the ids.
// The fields here bind the model directly (the registry has no model accessors) and forward under those ids when they
// are registered (when an id is not registered the edit is still saved and undoable, and
// io::midi::postMidiSettings sends it on the next project open).
//
//   id                      model value                          path (ChangeSet)               engine id (scope)
//   settings.pitchRange     ribbon.pitch{Bottom,Top}Cents        "ribbon" (rb::setPitchRange)   — (StripSource)
//   project.patternQuantize settings.patternQuantize STEP/BAR/PATTERN "settings.patternQuantize" — (SceneSync)
//   seq.padStartsSequence   settings.padStartsSequence           "settings.padStartsSequence"   seq.padStartsSequence
//   midi.in.<x> / midi.out.<x>   midi.{input,output}.<x> (clock, startStop, noteCc, songPosition)  same id (0)
//   midi.in.channel / midi.out.channel [t]  midi.{input,output}.channels[t] (0 = OFF, 1–16)   same id (t)
// Message thread only.
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"
#include "engine/EngineApi.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace services::settings
{
struct ProjectField
{
    std::string_view id, label;
    core::ParamSpec spec;      // range, labels, format (not registered: the registry owner is another area)
    int count = 1;             // 16 for the per-track channel fields (index = track 0–15)
    std::string_view engineId; // forwarded as this ParamSpec id when registered ("" = another forwarder does it)
};

std::span<const ProjectField> projectFields() noexcept;
const ProjectField* findProjectField(std::string_view id) noexcept;

// The field's value in the spec's units: an option index for enums (PITCH RANGE: 0–12 = 1…12, 24 semitones; −1 = an
// imported asymmetric range), 0/1 for switches, the channel 0–16.
float projectValue(const core::model::Project&, const ProjectField&, int index = 0);
// The undoable command that sets it (null when the value is unchanged or out of range).
std::unique_ptr<core::Command> setProjectValue(const core::model::Project&, const ProjectField&, int index, float value);
// SET-002: track n ↔ channel n for the MIDI IN (input = true) or OUT mapping, one undo step (null when already so).
std::unique_ptr<core::Command> resetChannelMapping(const core::model::Project&, bool input);
// The text of the value (PITCH RANGE "2 Bipolar Semitones" / "−200 / +700 cents"; channels "OFF" / "CH 3").
int formatProjectValue(const core::model::Project&, const ProjectField&, int index, float value, char* buf, int n);

// The bridge hook's half for these fields (seq.padStartsSequence and the MIDI ids; the ribbon and pattern quantise
// have their own forwarders). Ids the registry lacks are skipped.
void forwardSettingsChanges(const core::ChangeSet&, const core::model::Project&, engine::EngineBridge&,
                            const core::ParamRegistry& = core::builtinParams());
// PITCH RANGE options: N semitones of option i.
int pitchRangeSemitones(int option) noexcept;
} // namespace services::settings
