// What the PROJECT screens need of the project lifecycle (PRJ-001…061, UI-316…319): list / create / open / save /
// remove / rename / duplicate. The product implementation is StoreProjectSource (over
// services::store::ProjectStore), installed by the composition root with installProjectSession(); tests install a fake
// (tests/snapshot/m2/FakeProjectSource.h). With none installed the screens get an unavailable source (an empty list;
// every action reports that project storage is not available). Message thread only. Completions may run later (the
// store's workers post back through workers.pump()) or at once; callers handle both.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ui::m2
{
struct ProjectEntry
{
    std::string name;
    std::string modified;         // display text ("Today 14:12", "18 Nov 2026")
    std::int64_t modifiedKey = 0; // sort key (seconds), newer = larger
    int scenes = 0;               // scenes with content
    bool external = false;        // on a removable volume (USB badge, PRJ-060)
    std::string location;         // badge text for external entries ("USB" or the volume name)
    std::string path;             // where it is (the source's key: two locations may hold the same name)
    bool foreign = false;         // a reference project (.prj) in a read-only location: OPEN imports it as a copy
};

enum class ProjectSort : std::uint8_t
{
    Modified, // newest first (default)
    Name      // A → Z
};
enum class SaveTarget : std::uint8_t
{
    Save,   // the current file (an unsaved project takes `name`)
    SaveAs, // `name` in the projects folder
    SaveTo  // `name` on an external location (SAVE TO USB, PRJ-060)
};

class ProjectSource
{
public:
    virtual ~ProjectSource() = default;
    virtual std::vector<ProjectEntry> list(ProjectSort) = 0;
    virtual const std::string& currentName() const = 0; // a reference: the title band reads it every frame
    virtual std::string replacementError() const { return {}; }
    virtual bool dirty() const = 0;                     // unsaved changes (CommandBus checkpoint)
    virtual bool hasFile() const = 0;                   // saved at least once (else SAVE asks for a name)
    virtual bool exists(std::string_view name) = 0;
    virtual void create(const std::string& name, std::function<void(bool ok, std::string error)> done) = 0;
    virtual void open(const ProjectEntry&, std::function<void(float)> progress,
                      std::function<void(bool ok, std::string error, std::vector<std::string> missing)> done) = 0;
    virtual void save(SaveTarget, const std::string& name, std::function<void(float)> progress,
                      std::function<void(bool ok, std::string error)> done) = 0;
    virtual bool remove(const ProjectEntry&, std::string& error) = 0;
    virtual bool rename(const ProjectEntry&, const std::string& name, std::string& error) = 0;
    virtual bool duplicate(const ProjectEntry&, std::string& error) = 0;
    virtual void cancel() {} // a running open (PRJ-051)
    // Once per frame while the screens show a job's Progress: forwards a polled progress to its callback.
    virtual void poll() {}

    // The installed source, else the local one.
    static ProjectSource& current();
    static void install(ProjectSource*) noexcept;
};

// PRJ-004 through services::store::validateProjectName (UTF-8 in): nullopt = valid, else the reason
// in UI wording.
std::optional<std::string_view> validateProjectName(std::string_view utf8);

// The name a SAVE AS proposes: the current name with its trailing number incremented ("Groove_Sketch_03" →
// "Groove_Sketch_04"), "<name>_2" without one; CREATE NEW proposes "Project_NNN" (the next free number).
std::string nextName(std::string_view name);
std::string newProjectName(ProjectSource&);

} // namespace ui::m2
