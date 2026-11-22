// Project file formats (04-file-formats/, FMT-001…045). A codec reads and/or writes a whole core::model::Project;
// the native format (JSON + WAV bundle, schema version N) and the SP-16 import (read-only) are codecs registered with
// APP_REGISTER_CODEC; schema migrations are pure functions on the JSON tree registered with APP_REGISTER_MIGRATION
// (FMT-003: readers accept every older version). Worker threads only (never the audio thread); writes are atomic
// via AtomicWriter (FMT-002).
#pragma once

#include "core/model/Project.h"
#include "core/util/Registrar.h"
#include "core/util/Result.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace services::store
{
namespace model = ::core::model;
using ::core::Result;

// What a read could not map (FMT-042 import report): shown to the user after an import, never fatal.
struct ImportReport
{
    std::vector<std::string> warnings;  // e.g. "scene 3: unknown effect type kept as Off"
    std::vector<std::string> unmapped;  // reference nodes/keys kept in Extras or dropped
    bool readOnly = false;              // a newer native file than this app understands
};

struct CodecInfo { std::string_view id, name; std::span<const std::string_view> extensions; bool canWrite; };
class ProjectCodec {
public:
    virtual ~ProjectCodec() = default;
    virtual CodecInfo info() const = 0;
    virtual bool canRead(const juce::File&) const = 0;                    // sniff, cheap
    // Heap result: a Project is 2.4 MB and must never travel through a caller's stack.
    virtual Result<std::unique_ptr<model::Project>> read(const juce::File&, ImportReport&) const = 0;
    virtual Result<void> write(const model::Project&, const juce::File&) const = 0;   // atomic via AtomicWriter
};
// Native format: JSON + WAV bundle, schema version N; migrations are pure functions on the JSON tree.
struct Migration { int from, to; void (*apply)(juce::var& root); std::string_view summary; };

// Registration lists (filled before main(); read-only afterwards). CodecRegistry / MigrationRunner build
// on them (lookup by extension, sniffing order, migration chains).
inline std::vector<std::unique_ptr<ProjectCodec>>& registeredCodecs()
{
    static std::vector<std::unique_ptr<ProjectCodec>> codecs;
    return codecs;
}
inline std::vector<Migration>& registeredMigrations()
{
    static std::vector<Migration> migrations;
    return migrations;
}
inline void registerCodec(std::unique_ptr<ProjectCodec> codec) { registeredCodecs().push_back(std::move(codec)); }
inline void registerMigration(const Migration& m) { registeredMigrations().push_back(m); }
} // namespace services::store

// APP_REGISTER_CODEC(CodecType): one default-constructed instance of CodecType (a ProjectCodec) is registered.
#define APP_REGISTER_CODEC(...)                                                                                         \
    APP_STATIC_REGISTRAR(::services::store::registerCodec(std::make_unique<__VA_ARGS__>()))
// APP_REGISTER_MIGRATION({from, to, &fn, "summary"})
#define APP_REGISTER_MIGRATION(...) APP_STATIC_REGISTRAR(::services::store::registerMigration(__VA_ARGS__))
