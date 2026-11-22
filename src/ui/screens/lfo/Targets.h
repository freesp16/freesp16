// Generated parameter target lists (UID-201, UID-734, UID-885, UI-333): the LFO's DESTINATION popup and
// column 4 (ParamRegistry::lfoTargets()) and TOUCH STRIP SETTING's SELECT PARAMETER (stripTargets()), never a hand
// list. Rows: `NONE` first (value −1), then one group per module with a non-focusable header — PLAYBACK,
// AMP ENVELOPE, `FX1 : <effect>`, `FX2 : <effect>` (only the parameters of the effect now in that slot of the track,
// FX-083), MIXER (the TRACK group's volume / pan / send), then any other group in registry order (the module
// chain's order, as the spec's popups list them). Labels are the ParamSpec captions; a row's value = the ParamId.
//
// Target names (the rail value box, column 4, TOUCH STRIP SETTING's list): the shortLabel, FX targets with their slot
// ("FX1 CUT OFF", core::commands::ribbon::targetName). A target is *stale* when it is an FX parameter of an effect no
// longer in its slot (LFO-013, UID-739): shown `text-3` with ⚠, it does nothing until the effect returns.
//
// Message thread. build() allocates (the popup and the list are made on demand); names are cached per ParamId.
#pragma once

#include "core/params/ParamRegistry.h"
#include "ui/descriptors/ListSpec.h"

#include <string_view>
#include <vector>

namespace ui::m4
{
enum class TargetKind : unsigned char
{
    Lfo,  // lfoTarget: true, groups "FX1 : <effect>"
    Strip // stripTarget: true, groups "FX1 · <effect>"
};

// The list for track t (0-based) of scene s (0-based). `out` is cleared first. Group header strings live until the
// next build() of the same kind.
void buildTargets(const core::ParamRegistry&, TargetKind, int scene0, int track0, std::vector<ListItem>& out);

// The on-screen name of a target ("PITCH", "FX1 CUT OFF"); "NONE" for an invalid id. Stable storage.
std::string_view targetName(const core::ParamRegistry&, core::ParamId);
// The module and caption ("FX1 › FILTER CUT OFF", "PLAYBACK › PITCH") of TOUCH STRIP SETTING's places. Stable.
std::string_view targetPath(const core::ParamRegistry&, core::ParamId);
// FX target whose effect is not in its slot of track t / scene s now.
bool targetStale(const core::ParamRegistry&, core::ParamId, int scene0, int track0);
// The effect id ("filter") of an FX target id ("fx1.filter.cutoff"), empty otherwise; slot = 0 FX1, 1 FX2.
std::string_view fxEffectOf(std::string_view paramId, int& slot) noexcept;
} // namespace ui::m4
