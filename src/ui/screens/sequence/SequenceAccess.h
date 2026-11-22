// Read access to the SEQUENCE body for tests and benchmarks (tests/snapshot/m2, tests/bench/bench_ui_m2.cpp): the grid
// model it draws, its target steps, and a stand-in play step while no engine feed runs. Null / 0 for another body.
#pragma once

#include "ui/widgets/StepGrid.h"

#include <cstdint>

namespace ui
{
const StepGridModel* sequenceModel(Widget* body, double nowMs);
std::uint64_t sequenceTargets(Widget* body); // bit s = pattern step s (0-based) is a target
void sequencePlayStep(Widget* body, int step1to16);
} // namespace ui
