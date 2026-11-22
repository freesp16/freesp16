// Parameter catalogue, area EXTENSIONS / E4 (spec 02-features/midi.md MID-034, effects/parameters.md FX-088, P3, our
// design): the carrier of MIDI learn's CC overlay (services/midilearn/MidiLearn.h). Order 9951 (after every other
// catalogue, so no existing ParamId moves).
//
// `ext.midi.learn.cc` is internal (storage "internal.ext.midi.learn.cc": never saved, never shown in a list, not
// learnable): while ext.enabled + ext.midi.learn are on, the CCs the fixed map leaves free are applied to it as
// cc × 128 + value (0–16383), which the engine ignores and reports to the UI, where the learned bindings apply. With
// the flag off it is never published or posted.
#include "core/params/ParamRegistry.h"

namespace core
{
namespace
{
void addExtMidiLearnParams(ParamRegistry& r)
{
    r.add({.id = "ext.midi.learn.cc", .label = "MIDI LEARN CC", .shortLabel = "LEARN CC", .group = "MIDI",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 16383, .defaultValue = 0,
           .featureFlag = "ext.midi.learn", .scope = ParamScope::Track,
           .storage = {.key = "internal.ext.midi.learn.cc"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("ext_midi_learn", 9951, &core::addExtMidiLearnParams)
