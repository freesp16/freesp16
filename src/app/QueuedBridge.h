// The composition root's paced EngineBridge: services::render::QueuedBridge (moved there so the
// offline render service and the render CLI use the same pacing as the app's project open).
#pragma once

#include "services/render/QueuedBridge.h"

namespace app
{
using QueuedBridge = services::render::QueuedBridge;
} // namespace app
