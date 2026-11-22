// Registration of None (see None.h): test-only, behind the feature flag "fx.test"; every slot kind.
#include "engine/fx/effects/None.h"

#include "engine/fx/EffectRegistryOrder.h"
#include "engine/fx/FxShell.h"

APP_REGISTER_EFFECT((engine::fx::EffectDescriptor{
    .id = "none",
    .name = "NONE",
    .slots = engine::fx::kAllSlotKinds,
    .params = {"", "", "", "", "", "fx.none.mix"},
    .options = {},
    .visual = "meters",
    .shell = {.gateMode = 0,
              .mixLaw = static_cast<std::uint8_t>(engine::fx::MixLaw::CrossfadeK5),
              .wetHighPass = false,
              .skipWhenSilent = false,
              .clampOutput = false},
    .create = &engine::fx::make<engine::fx::effects::None>,
    .featureFlag = engine::fx::kTestEffectFlag}))
