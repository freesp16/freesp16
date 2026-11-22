// Parameter catalogue, area SEQUENCER (SEQ): swing, record quantise, the step-entry trigger type, the SEQUENCE strip
// (OFF SET, RETRIGGER CNT/SPD, SHIFT, SHIFT AREA, TRANSPOSE), PAD STARTS SEQ, post-quantize strength, and the internal
// record / erase states the engine needs (spec 02-features/sequencer.md SEQ-004/005/011/030/035/036/ 042/043/046/047;
// 03-ui/enumerations.md; 07-ui-design/screens/sequence.md and bpm-quantize.md). One file per area (UID-200/201). The
// engine binds swing / recordQuantize / padStartsSequence / offset / record / erase in
// engine/sequencer/SequencerStage.h; retrigger, shift and transpose are played by engine/sequencer/perf/PerfStage.h.
//
// Units are the lock slots' native units (SEQ-024): OFF SET in 1/24 step (−24…+24 = ±1 step, lock slot 15),
// RETRIGGER CNT 1–16 (slot 16), RETRIGGER SPD the enum index (slot 17).
#include "core/params/ParamRegistry.h"
#include "core/util/sequencer/RecordQuantize.h"

#include <cmath>
#include <cstdio>
#include <iterator>

#if defined(__GNUC__) && ! defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace core
{
namespace
{
constexpr std::string_view offOn[] = {"OFF", "ON"};
constexpr std::string_view recordQuantize[] = {"OFF", "1/32", "1/16T", "1/16", "1/8T", "1/8"}; // SEQ-042 (1.60 popup)
static_assert(std::size(recordQuantize) == sequencer::kRecordQuantizeCount);
constexpr std::string_view triggerType[] = {"FULL", "HALF", "PARAM"}; // SEQ-011 (= model::TriggerType)
// RETRIGGER SPD: our interval list 1/8, 1/6, 1/4, 1/3, 1/2, 1 step shown as note values (enumerations.md);
// default 1/4 step = "1/64" (SEQ-030 and 03-ui/enumerations.md "RETRIGGER SPD": the manual's default, M p.40; 1.60's
// displayed "1/32" is an open observation there), the same as core/model/Defaults.cpp and the engine (PerfStage).
constexpr std::string_view retrigSpeed[] = {"1/128", "1/96", "1/64", "1/48", "1/32", "1/16"};
constexpr std::string_view shiftArea[] = {"BAR", "ALL"}; // SEQ-036

int formatSigned(float v, char* buf, int n) noexcept
{
    const long i = std::lround(v);
    return std::snprintf(buf, static_cast<std::size_t>(n), i > 0 ? "+%ld" : "%ld", i);
}
int formatPercent(float v, char* buf, int n) noexcept
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "%ld %%", std::lround(v));
}
int formatShift(float, char* buf, int n) noexcept // a relative control: each detent rotates one step (SEQ-036)
{
    return std::snprintf(buf, static_cast<std::size_t>(n), "\xE2\x80\xB9 \xE2\x80\xBA");
}

void addSequencerParams(ParamRegistry& r)
{
    // SEQ-004: SWING 0–50 % shown, delay = swing × step (50 % = the classic 75 % shuffle; the model stores 50–75 %).
    r.add({.id = "seq.swing", .label = "SWING", .shortLabel = "SWING", .group = "SEQUENCE", .type = ParamType::Integer,
           .minValue = 0, .maxValue = 50, .defaultValue = 0, .format = formatPercent, .scope = ParamScope::Project,
           .storage = {.reference = "swingRate"}});
    // SEQ-042: real-time record quantise (its own enum, core/util/sequencer/RecordQuantize.h), default 1/16.
    r.add({.id = "seq.recordQuantize", .label = "QUANTIZE", .shortLabel = "QUANTIZE", .group = "SEQUENCE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = sequencer::kRecordQuantizeCount - 1,
           .defaultValue = static_cast<float>(sequencer::kDefaultRecordQuantize), .enumLabels = recordQuantize,
           .scope = ParamScope::Project, .storage = {.reference = "padQuantizeUnit"}});
    // SEQ-011 / UI-246: the type step entry writes (SEQUENCE TRIGGER TYPE), default FULL.
    r.add({.id = "seq.triggerType", .label = "TRIGGER TYPE", .shortLabel = "TRIGTYPE", .group = "SEQUENCE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 2, .defaultValue = 0, .enumLabels = triggerType,
           .scope = ParamScope::Project});
    // SEQ-005: OFF SET ±1 step in 1/24, CC 54, lockable (slot 15); the track value applies to steps without a lock.
    r.add({.id = "seq.offset", .label = "OFF SET", .shortLabel = "OFF SET", .group = "SEQUENCE",
           .type = ParamType::Integer, .minValue = -24, .maxValue = 24, .defaultValue = 0, .format = formatSigned,
           .lockable = true, .bipolar = true, .midiCc = 54, .scope = ParamScope::Track,
           .storage = {.reference = "offset"}});
    // SEQ-030: RETRIGGER CNT 1–16 (1 = a single trigger), CC 55, lockable (slot 16).
    r.add({.id = "seq.retrigCount", .label = "RETRIGGER CNT", .shortLabel = "RTRG CNT", .group = "SEQUENCE",
           .type = ParamType::Integer, .minValue = 1, .maxValue = 16, .defaultValue = 1, .lockable = true,
           .midiCc = 55, .scope = ParamScope::Track, .storage = {.reference = "retriggerCount"}});
    // SEQ-030: RETRIGGER SPD, CC 56, lockable (slot 17).
    r.add({.id = "seq.retrigSpeed", .label = "RETRIGGER SPD", .shortLabel = "RTRG SPD", .group = "SEQUENCE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 5, .defaultValue = 2, .enumLabels = retrigSpeed,
           .lockable = true, .midiCc = 56, .scope = ParamScope::Track, .storage = {.reference = "retriggerSpeed"}});
    // SEQ-036: SHIFT, a relative control (each detent rotates the triggers one step inside SHIFT AREA).
    r.add({.id = "seq.shift", .label = "SHIFT", .shortLabel = "SHIFT", .group = "SEQUENCE", .type = ParamType::Integer,
           .minValue = -1000, .maxValue = 1000, .defaultValue = 0, .format = formatShift, .scope = ParamScope::Track});
    r.add({.id = "seq.shiftArea", .label = "SHIFT AREA", .shortLabel = "SHFTAREA", .group = "SEQUENCE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 1, .enumLabels = shiftArea,
           .scope = ParamScope::Track, .storage = {.reference = "stepShiftArea"}});
    // SEQ-035: TRANSPOSE ±24 semitones.
    r.add({.id = "seq.transpose", .label = "TRANSPOSE", .shortLabel = "TRANSPOS", .group = "SEQUENCE",
           .type = ParamType::Integer, .minValue = -24, .maxValue = 24, .defaultValue = 0, .format = formatSigned,
           .bipolar = true, .scope = ParamScope::Track, .storage = {.reference = "transpose"}});
    // SEQ-047: the first pad hit while stopped and REC armed starts the transport.
    r.add({.id = "seq.padStartsSequence", .label = "PAD STARTS SEQ", .shortLabel = "PAD STRT", .group = "SEQUENCE",
           .type = ParamType::Enum, .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOn,
           .scope = ParamScope::Project, .storage = {.reference = "padStartsSequence"}});
    // SEQ-043: post-quantize strength (the SEQUENCE QUANTIZE button uses 100 %).
    r.add({.id = "seq.postQuantize", .label = "POST QUANTIZE", .shortLabel = "POST QNT", .group = "SEQUENCE",
           .type = ParamType::Integer, .minValue = 0, .maxValue = 100, .defaultValue = 100, .format = formatPercent,
           .scope = ParamScope::Project});
    // Internal states (never on a knob, never saved: Global "session." keys, like pads.mode): ERASE held (SEQ-046,
    // scope = the track) and REC armed (SEQ-041; posted by the REC key's owner, ui::input::steps::StepController).
    r.add({.id = "seq.erase", .label = "ERASE", .shortLabel = "ERASE", .group = "SEQUENCE", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOn, .scope = ParamScope::Global,
           .storage = {.key = "session.seq.erase"}});
    r.add({.id = "seq.record", .label = "REC", .shortLabel = "REC", .group = "SEQUENCE", .type = ParamType::Enum,
           .minValue = 0, .maxValue = 1, .defaultValue = 0, .enumLabels = offOn, .scope = ParamScope::Global,
           .storage = {.key = "session.seq.record"}});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("sequencer", 400, &core::addSequencerParams)
