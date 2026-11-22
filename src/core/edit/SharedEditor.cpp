#include "core/edit/SharedEditor.h"

#include <memory>

namespace core::edit
{
namespace
{
struct Shared
{
    InlineWorkerPool inlinePool;
    WorkerPool* workers = nullptr;
    CommandBus* bus = nullptr;
    std::unique_ptr<SceneEditor> editor;
};

Shared& shared()
{
    static Shared s;
    return s;
}

WorkerPool& pool()
{
    auto& s = shared();
    return s.workers != nullptr ? *s.workers : s.inlinePool;
}
} // namespace

SceneEditor& sharedEditor(CommandBus& bus)
{
    auto& s = shared();
    if (! s.editor || s.bus != &bus)
    {
        Clipboard keep;
        if (s.editor)
            keep = s.editor->clipboard();
        s.editor = std::make_unique<SceneEditor>(bus, pool());
        s.editor->clipboard() = keep;
        s.bus = &bus;
    }
    return *s.editor;
}

void setSharedWorkers(WorkerPool* workers)
{
    auto& s = shared();
    s.workers = workers;
    if (s.editor)
        s.editor->setWorkers(pool()); // the same editor (references to it stay valid)
}

void resetSharedEditor()
{
    auto& s = shared();
    s.editor.reset();
    s.bus = nullptr;
}
} // namespace core::edit
