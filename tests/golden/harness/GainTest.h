// GainTest lives in the engine: engine/fx/effects/GainTest.h (registered as "gainTest" behind the
// feature flag "fx.test"). The golden subject fx.gain_test and the harness self-tests use it through this alias; its
// golden reference stays tools/golden-gen/goldenlib/models/gain_test.py (operation for operation identical).
#pragma once

#include "engine/fx/effects/GainTest.h"

namespace golden
{
using GainTest = engine::fx::effects::GainTest;
static_assert(GainTest::kBrokenScale == 1.0f + 1.0f / 1024.0f, "the broken gain is the harness's kFaultGainScale");
} // namespace golden
