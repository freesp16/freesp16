// Registration of GainTest (see GainTest.h): test-only, behind the feature flag "fx.test"; every slot kind.
#include "engine/fx/effects/GainTest.h"

#include "engine/fx/EffectRegistryOrder.h"

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{.id = "gainTest",
                                                  .name = "GAIN TEST",
                                                  .slots = engine::fx::kAllSlotKinds,
                                                  .params = {"fx.gainTest.gain", "", "", "", "", ""},
                                                  .options = {},
                                                  .visual = "meters",
                                                  .shell = {},
                                                  .create = &engine::fx::make<engine::fx::effects::GainTest>,
                                                  .featureFlag = engine::fx::kTestEffectFlag}))
