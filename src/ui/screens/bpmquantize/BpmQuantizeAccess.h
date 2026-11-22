// Test access to the BPM/QUANTIZE panel (the body of the `bpmQuantize` Modal): the focused field, 0 BPM · 1 SWING ·
// 2 QUANTIZE; -1 when `body` is not the panel.
#pragma once

#include "ui/widgets/Widget.h"

namespace ui
{
int bpmQuantizeFocus(Widget* body);
} // namespace ui
