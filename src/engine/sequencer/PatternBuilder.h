// Message-thread builder of the engine's pattern objects: one PatternData per
// scene × pattern, built from the model-neutral image (core/util/sequencer/PatternImage.h; why an image: see there),
// with its LockSets and their value storage in one owner. ≈ 9 KB + 0.3 KB per locked cell; ≤ 0.3 ms on the A9.
//
// Registered as core::sequencer::patternCompiler() at start-up (PatternBuilder.cpp), and as the destroy function of
// SwapSlot::Pattern (Engine set-up: acceptSwaps(SwapSlot::Pattern, destroyPattern)). Never called on the audio thread.
#pragma once

#include "core/util/sequencer/PatternImage.h"
#include "engine/sequencer/PatternData.h"

namespace engine::sequencer
{
// Allocates (message thread). Lengths outside 1–64 are clamped; cells without velocity are empty.
const PatternData* buildPattern(const core::sequencer::PatternImage&);
// Frees an object buildPattern made (message thread; the engine's retire queue calls it after the grace period).
void destroyPattern(const void* object) noexcept;
} // namespace engine::sequencer
