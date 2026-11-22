// Touch strip settings commands (spec 02-features/touch-strip.md RIB-002/004/009/011, effects/parameters.md
// FX-095/096; state-and-undo.md). The project's RibbonSettings (core::model::Project::ribbon, saved with the project,
// RIB-009): USER1 / USER2 × 8 items {target ParamSpec id, bottom, top} and the PITCH range. Message thread; each edit
// is one undo step (EDT-U1) whose inverse restores the whole RibbonSettings (≈ 1 kB); knob edits of one item's range
// (or of the pitch range) coalesce (EDT-U2). The ChangeSet path is "ribbon"; io/controllers/StripSource.h forwards it
// to the engine.
//
//   assignItem    RIB-011: the parameter chosen in the popup goes to the focused slot; a slot past the list's end
//                 appends ("added to the list"), so the items stay contiguous. A new target starts at BOTTOM 0 /
//                 TOP 1 (the full range). Only strip targets (ParamSpec::stripTarget: PLAYBACK PITCH, AMP
//                 ENVELOPE, the mixer's volume / pan, every insert-FX knob on FX1/FX2, FX-095) are accepted.
//   setItemRange  the knobs: BOTTOM VALUE / TOP VALUE of the focused item, normalised 0–1 (clamped).
//   deleteItem    RIB-011 (the UI confirms first): removes the focused item; the ones after it move up.
//   setPitchRange RIB-002: bottomCent −2400…0, topCent 0…+2400 (model::Validate's ranges).
// A factory returns null for an edit that would change nothing or is invalid (unknown target, empty slot, full list).
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/params/ParamRegistry.h"

#include <memory>
#include <string>
#include <string_view>

namespace core::commands::ribbon
{
inline constexpr int kSets = 2, kItems = 8;
inline constexpr std::string_view kPath = "ribbon"; // the ChangeSet path of every ribbon edit

int itemCount(const model::RibbonSettings&, int set) noexcept; // contiguous items from slot 0
bool isStripTarget(const ParamRegistry&, std::string_view id) noexcept;
// FX-096: the target's on-screen name — the upper-case shortLabel, MIXRATIO shortened to MIX; FX targets carry their
// slot ("FX1 CUTOFF") so FX1 and FX2 items differ.
std::string targetName(const ParamSpec&);

std::unique_ptr<Command> assignItem(const model::Project&, int set, int slot, std::string_view targetId,
                                    const ParamRegistry& = builtinParams());
std::unique_ptr<Command> setItemRange(const model::Project&, int set, int slot, double bottom01, double top01);
std::unique_ptr<Command> deleteItem(const model::Project&, int set, int slot);
std::unique_ptr<Command> setPitchRange(const model::Project&, int bottomCents, int topCents);
// Replaces the whole settings (the inverse of every edit above; also a project import).
std::unique_ptr<Command> replaceRibbon(model::RibbonSettings, std::string_view label = "TOUCH STRIP");
} // namespace core::commands::ribbon
