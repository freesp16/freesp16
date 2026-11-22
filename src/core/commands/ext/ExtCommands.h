// Commands of the P3 extensions: one undoable edit of an Extras entry, the store of every extension value
// the model has no field for (core/model/Extras.h): E11 TOGGLE per track ("native.ext.trk.toggle"), E4
// the project's MIDI learn bindings ("native.ext.midi.learn"). The native codec writes "native.<key>" entries back as
// plain keys of their entity, so a file written with a flag on loads with it off and keeps the data.
//
// Paths reported to observers / the bridge hook: "scene[s].track[t].extras.<key>" (track entry) and "extras.<key>"
// (project entry) — no ParamSpec forwarder knows them; the extensions' glue (app/ext) reacts. Message thread only.
#pragma once

#include "core/commands/Command.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace core::commands::ext
{
// scene / track < 0: the project's extras. value nullopt erases the entry.
std::unique_ptr<Command> setExtra(int scene, int track, std::string key, std::optional<std::string> value,
                                  std::string_view label);
std::string extraPath(int scene, int track, std::string_view key);
} // namespace core::commands::ext
