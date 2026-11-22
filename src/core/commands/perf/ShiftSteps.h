// The SEQUENCE screen's performance edits (spec 02-features/sequencer.md
// SEQ-036 step shift, SEQ-038 targeting; 01-architecture/state-and-undo.md EDT-U1/U2). Message thread only.
//
// shiftSteps   rotates one track's cells — triggers, velocities, kinds and their locks (the lock index travels with
//              the cell) — by `delta` whole steps, circularly, inside SHIFT AREA: BAR = the 16 steps of bar `bar`
//              (0-based, clipped to the pattern length; a bar past the end is a no-op), ALL = steps 0 … length − 1.
//              The range is resolved from the pattern when the command applies. Its inverse is the opposite rotation
//              of the same resolved range (a few bytes, never a pattern copy: bit-identical undo), and consecutive
//              detents on one track / range coalesce into one undo step (EDT-U2: the inverses add up). Reports
//              "scene[s].pattern[p]", so the bridge hook rebuilds that pattern object and the engine plays it from
//              the next block (ObjectSwap).
// setStepsParam SEQ-038: `stepMask` 0 = the track-level value (TrackSequencerSettings: seq.offset, seq.retrigCount,
//              seq.retrigSpeed, seq.transpose, seq.shiftArea; model path "scene[s].track[t].<id>", a coalescing
//              SetParam), else a step lock of `param` on exactly the masked steps (bit s = step s), kept even when it
//              equals the track value (removing it would let a different track value apply). Values are the
//              ParamSpec's units, clamped. nullptr when `param` has no track field (mask 0) or no lock slot (mask ≠ 0).
#pragma once

#include "core/commands/Command.h"
#include "core/model/enums/ValueLists.h"
#include "core/params/ParamId.h"
#include "core/params/ParamRegistry.h"

#include <cstdint>
#include <memory>

namespace core::perf
{
using ShiftArea = ::core::model::StepShiftArea; // BAR, ALL (SEQ-036)

std::unique_ptr<Command> shiftSteps(int scene, int pattern, int track, ShiftArea area, int bar, int delta);
std::unique_ptr<Command> setStepsParam(int scene, int pattern, int track, std::uint64_t stepMask, ParamId param,
                                       float value, const ParamRegistry& registry = builtinParams());

// The step range shiftSteps rotates for a pattern of `length` steps: [first, first + count), count 0 = nothing.
struct ShiftRange
{
    int first = 0, count = 0;
};
constexpr ShiftRange shiftRange(int length, ShiftArea area, int bar) noexcept
{
    const int len = length < 1 ? 1 : (length > 64 ? 64 : length);
    if (area == ShiftArea::All)
        return {0, len};
    const int first = (bar < 0 ? 0 : (bar > 3 ? 3 : bar)) * 16;
    return first >= len ? ShiftRange {first, 0} : ShiftRange {first, len - first < 16 ? len - first : 16};
}
} // namespace core::perf
