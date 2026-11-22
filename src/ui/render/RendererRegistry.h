// Renderer backends by name. Each backend file registers a factory with APP_REGISTER_RENDERER; the `render` setting is
// an ordered fallback list ("auto" expands to platform::DisplayDevice::rendererOrder()). Backends that load a GL driver
// are probed in a forked child first (needsChildProbe): a driver crash or hang only rules that backend out, and the
// parent never loads a driver flavour it does not run with (Vivante flavours must not be mixed in one process).
#pragma once

#include "core/util/Registrar.h"
#include "ui/render/Renderer.h"
#include "ui/render/RendererEnv.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui::render
{
using ui::Renderer;
using ui::RendererEnv;

using RendererFactory = std::unique_ptr<Renderer> (*)(const RendererEnv&, std::string& error);
void registerRenderer(std::string_view name, RendererFactory, bool needsChildProbe);
std::unique_ptr<Renderer> createFirstWorking(std::span<const std::string> order, const RendererEnv&, std::string& log);

struct RendererEntry
{
    std::string_view name;
    RendererFactory create = nullptr;
    bool needsChildProbe = false;
};
std::vector<RendererEntry> registeredRenderers(); // registration order is unspecified: sorted by name
const RendererEntry* findRenderer(std::string_view name) noexcept;
bool needsChildProbe(std::string_view name) noexcept; // false for unknown names

// Creates `name` in this process. Null + error for an unknown name or a failing backend.
std::unique_ptr<Renderer> create(std::string_view name, const RendererEnv&, std::string& error);

// Forks a child that creates `name` and runs `work` on it (probe + optional measurement; the child writes its own log
// lines). True when the child exited 0 within the timeout; otherwise false + why ("child killed by signal 11", …).
// `work` returning false makes the child exit non-zero; `onCreateError` (in the child) reports a backend that could
// not be created. POSIX only (fork/waitpid).
bool probeInChild(std::string_view name, const RendererEnv&, const std::function<bool(Renderer&)>& work,
                  double timeoutMs, std::string& why,
                  const std::function<void(const std::string& error)>& onCreateError = {});
} // namespace ui::render

// APP_REGISTER_RENDERER("soft", &factory, needsChildProbe)
#define APP_REGISTER_RENDERER(name, factory, needsChildProbe)                                                          \
    APP_STATIC_REGISTRAR(::ui::render::registerRenderer(name, factory, needsChildProbe))
