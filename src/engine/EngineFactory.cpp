// engine::createEngine() — the engine the composition root runs: the real
// engine::Engine frame (engine/Engine.h). It keeps the walking skeleton's audible behaviour
// through the temporary test-tone stage (440 Hz −12 dBFS on track 1, a ping per live trigger, track-1 "track.volume"),
// which the emulator smoke test checks (scripts/emulator-smoke.sh: MAIN = WAV ch 0/2).
#include "engine/Engine.h"

#include <memory>

namespace engine
{
std::unique_ptr<AudioEngine> createEngine(const EngineSetup& setup)
{
    EngineOptions options;
    options.testTone = setup.testTone;
    return std::make_unique<Engine>(setup, options); // rt-ok: message thread, once at start-up
}
} // namespace engine
