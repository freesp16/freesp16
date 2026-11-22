// Knob-strip columns that run screen code (components/frame.md "ParamColumn", UID-160…166, UID-737): a body
// widget that also derives from StripOwner refines the shell's bound columns, like RailOwner does for the rail. The
// shell finds it once per screen entry (dynamic_cast on the body) and, per frame and bound column:
//   - stripColumn: after the shell filled the column from its ParamSpec (value, lock, rules), the body may replace the
//     shown value text (ParamColumnModel::valueText: LFO SPEED as a division in SYNC, DESTINATION as the target's
//     name, SEQUENCE's first-target value or `—`), mark it stepped (‹ ›) or disable / lock it.
//   - stripTurn: an encoder turn on a bound column before the shell's default set / lock write; returning true means
//     the body applied it (a list walked entry by entry, a command such as core::perf::setStepsParam on the targets,
//     a relative control). The shell then marks the column touched and logs the turn as usual.
// Message thread; no allocation in either call.
#pragma once

#include "core/params/ParamSpec.h"
#include "ui/widgets/ParamColumn.h"

namespace ui
{
class StripOwner
{
public:
    virtual ~StripOwner() = default;
    virtual void stripColumn(int column0, ParamColumnModel& column) const
    {
        (void)column0, (void)column;
    }
    virtual bool stripTurn(int column0, const core::ParamSpec& spec, int detents, bool fine)
    {
        (void)column0, (void)spec, (void)detents, (void)fine;
        return false;
    }
};
} // namespace ui
