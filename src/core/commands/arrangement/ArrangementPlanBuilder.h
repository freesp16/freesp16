// The engine's ArrangementPlan from the document (ARR-001/002, OQ-02): built on the
// message thread after every command that changes it (the rows, or a pattern length a row's start depends on) and
// installed by ObjectSwap{SwapSlot::SceneConfig, 0, 15} (core/util/arrangement/ArrangementPlan.h shows the post).
// Budget: ≤ 0.5 ms for 999 rows on the A9 (tests/bench/bench_arrangement.cpp).
#pragma once

#include "core/commands/Command.h"
#include "core/model/Project.h"
#include "core/util/arrangement/ArrangementPlan.h"

namespace core::commands::arrangement
{
// Fills `out` from the project's rows (normalised as the commands store them: scene / pattern 0–15, repeats 1–99, BPM
// "--" or 20–300) and the start table from the current pattern lengths (steps = sixteenths). `status` is kept.
void buildPlan(const model::Project&, core::arrangement::ArrangementPlan& out) noexcept;

// True when a change touches what the ArrangementPlan is built from: "arrangement" / "arrangement.sections", a whole
// scene or pattern ("scene[s]", "scene[s].pattern[p]") or a pattern length ("scene[s].pattern[p].length").
bool affectsPlan(const ChangeSet&) noexcept;
bool affectsPlay(const ChangeSet&) noexcept; // "arrangement.play" (ProjectSettings::playArrangement)
} // namespace core::commands::arrangement
