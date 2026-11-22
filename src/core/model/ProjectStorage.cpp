// Heap construction of a Project (2.3 MB: never on a stack). The big arrays in Project.h / Scene.h are
// default-initialised (no `{}`), because GCC 11 (the armel toolchain) expands a brace-initialised array of aggregates
// element by element: `new Project()` then needed > 3 GB and > 10 min of cc1plus. Values are the
// same as with `{}`: every Track / Pattern / Scene member has its own default member initialiser.
#include "core/model/Project.h"

#include <memory>

namespace core::model::detail
{
std::unique_ptr<Project> allocateProject() { return std::make_unique<Project>(); }
} // namespace core::model::detail
