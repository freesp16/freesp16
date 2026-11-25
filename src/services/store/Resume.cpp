#include "services/store/Resume.h"

#include "services/store/AtomicWriter.h"
#include "services/store/Autosave.h"
#include "services/store/ProjectStore.h"

namespace services::store
{
namespace
{
constexpr const char* kResumeFormat = "sixteen-resume";

bool isProjectJson(const juce::String& rel)
{
    return rel == "meta.json" || rel == "project.json" || (rel.startsWith("patterns/") && rel.endsWith(".json"));
}

bool hasProject(const juce::File& bundle) { return bundle.getChildFile("project.json").existsAsFile(); }

void replaceMove(const juce::File& from, const juce::File& to)
{
    if (! from.exists())
        return;
    if (to.exists())
        to.deleteRecursively();
    if (! from.moveFileTo(to))
        from.deleteRecursively(); // never leave it where the next start would find it again
}
} // namespace

Resume::Resume(Config config) : config_(std::move(config)) {}

std::optional<Resume::Record> Resume::read() const
{
    const auto f = resumeFile();
    if (! f.existsAsFile() || f.getSize() > 64 * 1024)
        return std::nullopt;
    const auto text = f.loadFileAsString();
    juce::var v;
    if (! jsonNestingWithin(text.toStdString(), 16) || juce::JSON::parse(text, v).failed() || ! v.isObject() ||
        v["format"]["name"].toString() != kResumeFormat)
        return std::nullopt;
    const auto path = v["project"].toString();
    if (path.isEmpty() || ! juce::File::isAbsolutePath(path))
        return std::nullopt;
    Record r;
    r.project = juce::File(path);
    const auto& s = v["selection"];
    r.selection.scene = juce::jlimit(0, 15, static_cast<int>(s["scene"]));
    r.selection.pattern = juce::jlimit(0, 15, static_cast<int>(s["pattern"]));
    r.selection.track = juce::jlimit(0, 15, static_cast<int>(s["track"]));
    r.selection.screen = s["screen"].toString().substring(0, 64).toStdString();
    r.earlyExits = juce::jlimit(0, 1000, static_cast<int>(v["earlyExits"]));
    return r;
}

Result<void> Resume::write(const Record& r) const
{
    auto* format = new juce::DynamicObject();
    format->setProperty("name", kResumeFormat);
    format->setProperty("version", 1);
    auto* sel = new juce::DynamicObject();
    sel->setProperty("scene", r.selection.scene);
    sel->setProperty("pattern", r.selection.pattern);
    sel->setProperty("track", r.selection.track);
    sel->setProperty("screen", juce::String::fromUTF8(r.selection.screen.c_str()));
    auto* root = new juce::DynamicObject();
    root->setProperty("format", juce::var(format));
    root->setProperty("project", r.project.getFullPathName());
    root->setProperty("selection", juce::var(sel));
    root->setProperty("earlyExits", r.earlyExits);
    root->setProperty("savedAt", juce::Time::getCurrentTime().toISO8601(true));
    const auto text = juce::JSON::toString(juce::var(root)).toStdString() + "\n";
    if (! config_.systemDir.createDirectory())
        return Result<void>::failure("Could not save the resume state: cannot create " +
                                     config_.systemDir.getFullPathName().toStdString());
    return AtomicWriter::writeFileAtomically(resumeFile(), text.data(), text.size());
}

void Resume::quarantine()
{
    const auto crashed = crashedDir();
    crashed.createDirectory();
    replaceMove(resumeFile(), crashed.getChildFile("resume.json"));
    replaceMove(copyBundle(), crashed.getChildFile("resume.sixteen"));
}

StartupDecision Resume::atStartup(bool resumeEnabled)
{
    std::lock_guard lock(mutex_);
    StartupDecision d;
    auto rec = read();
    // PRJ-021: the previous boot load never finished.
    if (guardDir().exists())
    {
        quarantine();
        guardDir().deleteRecursively();
        watchMarker().deleteFile();
        d.crashed = true;
        d.message = "The last project was not reopened: the app stopped while loading it. Its resume copy was moved "
                    "to System/crashed. A new project was started.";
        return d;
    }
    // The app stopped soon after reopening the project.
    if (watchMarker().exists())
    {
        watchMarker().deleteFile();
        if (rec)
        {
            ++rec->earlyExits;
            if (rec->earlyExits >= kMaxEarlyExits)
            {
                quarantine();
                d.crashed = true;
                d.message = "The last project was not reopened: the app stopped twice soon after opening it. Its "
                            "resume copy was moved to System/crashed. A new project was started.";
                return d;
            }
            (void) write(*rec);
        }
    }
    if (! resumeEnabled || ! rec) // PRJ-022
        return d;
    d.selection = rec->selection;
    d.identity = rec->project;
    if (hasProject(rec->project))
        d.source = rec->project;
    else if (hasProject(copyBundle()))
    {
        d.source = copyBundle();
        d.message = "The last project was not found at " + rec->project.getFullPathName() +
                    ": it was reopened from its resume copy. Save it to keep it.";
    }
    else
    {
        d.message = "The last project was not found at " + rec->project.getFullPathName() + ".";
        return d;
    }
    d.kind = StartupDecision::Kind::Resume;
    d.newerAutosave = Autosave::newerThan(config_.autosaveDir, rec->project.getFileNameWithoutExtension(), rec->project);
    return d;
}

Result<void> Resume::beginLoad()
{
    std::lock_guard lock(mutex_);
    config_.systemDir.createDirectory();
    if (! guardDir().createDirectory())
        return Result<void>::failure("Could not create " + guardDir().getFullPathName().toStdString());
    return {};
}

void Resume::endLoad(bool ok)
{
    std::lock_guard lock(mutex_);
    if (ok)
        watchMarker().replaceWithText("resumed\n");
    guardDir().deleteRecursively();
}

void Resume::settle()
{
    std::lock_guard lock(mutex_);
    if (! watchMarker().exists())
        return;
    watchMarker().deleteFile();
    if (auto rec = read(); rec && rec->earlyExits != 0)
    {
        rec->earlyExits = 0;
        (void) write(*rec);
    }
}

Result<void> Resume::markLoaded(const juce::File& bundle, const Selection& selection)
{
    std::lock_guard lock(mutex_);
    if (! config_.systemDir.createDirectory())
        return Result<void>::failure("Could not save the resume state: cannot create " +
                                     config_.systemDir.getFullPathName().toStdString());
    const auto old = read();
    Result<void> copied;
    if (hasProject(bundle)) // a copy of what was saved or loaded (the reference's data.rsm)
    {
        AtomicWriter copy(copyBundle());
        copied = copy.carryOver(bundle, [](const juce::String& rel) { return ! isProjectJson(rel); });
        if (copied)
            copied = copy.commit();
    }
    else if (! old || old->project != bundle)
        copyBundle().deleteRecursively(); // the copy is another project's: never resume it under this name
    Record r;
    r.project = bundle;
    r.selection = selection;
    // The early-exit count belongs to the project: another project starts at 0, the same one keeps it until the
    // session settles (the boot load itself calls markLoaded).
    if (old && old->project == bundle)
        r.earlyExits = old->earlyExits;
    if (auto w = write(r); ! w)
        return w;
    return copied;
}

Result<void> Resume::persistSelection(const Selection& selection)
{
    std::lock_guard guard(mutex_);
    auto rec = read();
    if (! rec)
        return {};
    rec->selection = selection;
    return write(*rec);
}

void Resume::setAside()
{
    std::lock_guard lock(mutex_);
    quarantine();
    watchMarker().deleteFile();
}

void Resume::forget()
{
    std::lock_guard lock(mutex_);
    resumeFile().deleteFile();
    copyBundle().deleteRecursively();
    watchMarker().deleteFile();
}
} // namespace services::store
