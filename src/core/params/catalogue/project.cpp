// Parameter catalogue, area PROJECT (02-features/project-management.md PRJ-022, PRJ-030): the project-lifecycle
// preferences. Both are preferences (ParamScope::Global, the settings file of the device), never saved in a project:
// the composition root reads them at start-up and passes them to services::store::ProjectSession (setResumeEnabled /
// setAutosaveEnabled). Order band 1100 (project/scene).
#include "core/params/ParamRegistry.h"

namespace core
{
namespace
{
void addProjectParams(ParamRegistry& r)
{
    // PRJ-022: reopen the last saved/loaded project at start-up (default ON; the reference has no switch).
    r.add({.id = "project.resume",
           .label = "RESUME",
           .shortLabel = "RESUME",
           .group = "PROJECT",
           .type = ParamType::Bool,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 1,
           .scope = ParamScope::Global});
    // PRJ-030: autosave copies to System/autosave/ (default ON; never the user's project file, PRJ-031).
    r.add({.id = "project.autosave",
           .label = "AUTOSAVE",
           .shortLabel = "AUTOSAVE",
           .group = "PROJECT",
           .type = ParamType::Bool,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 1,
           .scope = ParamScope::Global});
}
} // namespace
} // namespace core

APP_REGISTER_PARAMS("project", 1100, &core::addProjectParams)
