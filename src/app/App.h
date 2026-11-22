// The composition root: the only place that knows all modules, and it
// talks to them through interfaces and registries only —
//   platform::select("auto") → Platform::audioDevices() (first that starts) with engine::createEngine() as the
//   AudioProcessor; Platform::display() → the renderer registry (ui/render/RendererRegistry.h); ui::createRoot();
//   Platform::controlSources() polled once per frame.
// Modules replace what these factories return in their own files, so adding one does not mean editing app/.
#pragma once

namespace app
{
// Runs the app until SIGTERM/SIGINT (or `run_frames` frames). Returns the process exit code.
int run();
}
