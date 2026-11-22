#include "services/store/Autosave.h"

#include "core/commands/CommandBusAccess.h"
#include "core/model/ModelOps.h"
#include "services/store/AtomicWriter.h"
#include "services/store/Migrations.h"
#include "services/store/NativeCodec.h"
#include "services/store/ProjectStore.h"

#include <algorithm>
#include <utility>

namespace services::store
{
namespace model = ::core::model;

namespace
{
constexpr const char* kAutosaveFormat = "sixteen-autosave";
constexpr std::int64_t kMaxAutosaveBytes = 64ll << 20; // a full project is a few MB; refuse anything absurd

juce::String headerLine(const juce::String& name, const juce::File& bundle, const std::string& timestamp)
{
    auto* format = new juce::DynamicObject();
    format->setProperty("name", kAutosaveFormat);
    format->setProperty("version", 1);
    auto* h = new juce::DynamicObject();
    h->setProperty("format", juce::var(format));
    h->setProperty("name", name);
    h->setProperty("project", bundle == juce::File() ? juce::String() : bundle.getFullPathName());
    h->setProperty("savedAt", juce::String(timestamp));
    return juce::JSON::toString(juce::var(h), true); // one line: readHeader reads only the first line
}
} // namespace

Autosave::Autosave(core::CommandBus& bus, services::library::Workers& workers, Config config)
    : bus_(bus), workers_(workers), config_(std::move(config))
{
    changesSub_ = bus_.observe("", [this](const core::ChangeSet&) { ++changes_; });
}

Autosave::~Autosave() { *alive_ = false; }

juce::File Autosave::fileFor(const juce::File& folder, const juce::String& name)
{
    return folder.getChildFile(juce::String::fromUTF8(safeFileName(name.toStdString()).c_str()) + ".json");
}

void Autosave::setTarget(const juce::String& name, const juce::File& bundle)
{
    name_ = name;
    bundle_ = bundle;
    core::commands::flushNotifications(bus_);
    writtenAt_ = changes_;
    pendingSinceMs_.reset();
}

bool Autosave::unsavedSinceWrite() const
{
    return core::commands::hasProject(bus_) && bus_.dirty() &&
           (changes_ != writtenAt_ || core::commands::notificationsPending(bus_));
}

void Autosave::tick(double nowMs, bool playing)
{
    lastNowMs_ = nowMs;
    if (! enabled_ || busy_)
        return;
    if (! unsavedSinceWrite())
    {
        pendingSinceMs_.reset();
        return;
    }
    if (! pendingSinceMs_)
        pendingSinceMs_ = nowMs; // the oldest change not autosaved yet
    const double interval = playing ? std::max(config_.playingIntervalMs, config_.idleIntervalMs) : config_.idleIntervalMs;
    if (nowMs - *pendingSinceMs_ >= interval)
        start();
}

void Autosave::onDeviceLost()
{
    if (busy_)
        againWhenDone_ = true; // the running write may miss the newest edits
    else if (unsavedSinceWrite())
        start();
}

bool Autosave::writeNow()
{
    if (busy_ || ! unsavedSinceWrite())
        return false;
    start();
    return true;
}

void Autosave::start()
{
    busy_ = true;
    staleAfterWrite_ = false;
    core::commands::flushNotifications(bus_);
    writtenAt_ = changes_;
    pendingSinceMs_.reset();
    if (! snapshot_)
        snapshot_ = std::make_shared<model::Project>();
    *snapshot_ = core::commands::project(bus_); // the message-thread cost (bench_project `snapshot`)

    const auto target = file();
    // PRJ-031: never the user's project (the folder is System/autosave; refuse a target inside the bundle anyway).
    if (bundle_ != juce::File() && (target.isAChildOf(bundle_) || target == bundle_))
    {
        busy_ = false;
        if (onWritten)
            onWritten(Result<void>::failure("Autosave refused: " + target.getFullPathName().toStdString() +
                                            " is inside the project"));
        return;
    }
    auto result = std::make_shared<Result<void>>();
    std::weak_ptr<bool> alive = alive_;
    workers_.run(
        [snapshot = snapshot_, target, name = name_, bundle = bundle_, folder = config_.folder, result]
        {
            const auto text = encode(*snapshot, name, bundle);
            if (! folder.createDirectory())
            {
                *result = Result<void>::failure("Autosave: cannot create " + folder.getFullPathName().toStdString());
                return;
            }
            *result = AtomicWriter::writeFileAtomically(target, text.data(), text.size());
        },
        [this, alive, result, target]
        {
            if (alive.expired())
                return;
            busy_ = false;
            if (*result)
                ++writes_;
            else
            {
                writtenAt_ = ~std::uint64_t {0}; // still unsaved: retry after another interval
                pendingSinceMs_ = lastNowMs_;
            }
            if (staleAfterWrite_) // the user saved while it was written
            {
                staleAfterWrite_ = false;
                target.deleteFile();
            }
            if (onWritten)
                onWritten(*result);
            if (std::exchange(againWhenDone_, false) && unsavedSinceWrite())
                start();
        });
}

void Autosave::onSaved()
{
    core::commands::flushNotifications(bus_);
    writtenAt_ = changes_;
    pendingSinceMs_.reset();
    if (busy_)
    {
        staleAfterWrite_ = true;
        return;
    }
    file().deleteFile();
}

std::string Autosave::encode(const model::Project& project, const juce::String& name, const juce::File& bundle,
                             const std::string& timestamp)
{
    SaveOptions so;
    so.timestamp = timestamp.empty() ? juce::Time::getCurrentTime().toISO8601(true).toStdString() : timestamp;
    const auto files = NativeCodec::encode(project, so);
    std::string out;
    std::size_t size = 64;
    for (const auto& f : files)
        size += f.text.size() + 4;
    out.reserve(size + 512);
    out += "{\"autosave\": ";
    out += headerLine(name, bundle, so.timestamp).toStdString();
    out += ",\n\"root\": {";
    const std::string* meta = nullptr;
    const std::string* proj = nullptr;
    std::array<const std::string*, model::kScenes> scenes {};
    for (const auto& f : files)
    {
        if (f.name == "meta.json")
            meta = &f.text;
        else if (f.name == "project.json")
            proj = &f.text;
        else if (f.name.rfind("patterns/scene-", 0) == 0)
        {
            const int s = std::atoi(f.name.c_str() + 15);
            if (s >= 1 && s <= model::kScenes)
                scenes[static_cast<std::size_t>(s - 1)] = &f.text;
        }
    }
    out += "\"meta\": ";
    out += meta != nullptr ? *meta : std::string("{}");
    out += ",\n\"project\": ";
    out += proj != nullptr ? *proj : std::string("{}");
    out += ",\n\"patterns\": [";
    for (int s = 0; s < model::kScenes; ++s)
    {
        if (s > 0)
            out += ",\n";
        const auto* t = scenes[static_cast<std::size_t>(s)];
        out += t != nullptr ? *t : std::string("null");
    }
    out += "]}}\n";
    return out;
}

std::optional<Autosave::Header> Autosave::readHeader(const juce::File& f)
{
    juce::FileInputStream in(f);
    if (! in.openedOk())
        return std::nullopt;
    std::string head(16384, '\0'); // the header line is short; never read a damaged file whole
    head.resize(static_cast<std::size_t>(std::max(0, in.read(head.data(), static_cast<int>(head.size())))));
    const auto nl = head.find('\n');
    if (nl == std::string::npos)
        return std::nullopt;
    const auto line = juce::String::fromUTF8(head.data(), static_cast<int>(nl)); // {"autosave": {...},
    if (! line.startsWith("{\"autosave\": ") || ! line.endsWithChar(',') || ! jsonNestingWithin(head.substr(0, nl)))
        return std::nullopt;
    juce::var v;
    if (juce::JSON::parse(line.dropLastCharacters(1) + "}", v).failed())
        return std::nullopt;
    const auto& h = v["autosave"];
    if (h["format"]["name"].toString() != kAutosaveFormat)
        return std::nullopt;
    Header header;
    header.name = h["name"].toString();
    const auto project = h["project"].toString();
    if (project.isNotEmpty() && juce::File::isAbsolutePath(project))
        header.project = juce::File(project);
    header.savedAt = juce::Time::fromISO8601(h["savedAt"].toString());
    return header;
}

std::optional<juce::File> Autosave::newerThan(const juce::File& folder, const juce::String& name,
                                              const juce::File& bundle)
{
    const auto f = fileFor(folder, name);
    if (! f.existsAsFile())
        return std::nullopt;
    const auto header = readHeader(f);
    if (! header || header->project != bundle)
        return std::nullopt;
    const auto meta = bundle.getChildFile("meta.json");
    if (meta.existsAsFile() && f.getLastModificationTime() <= meta.getLastModificationTime())
        return std::nullopt;
    return f;
}

Result<void> Autosave::decodeText(const juce::String& text, model::Project& out, ImportReport& report)
{
    if (! jsonNestingWithin(text.toStdString())) // NFR-011: the JSON parser is recursive
        return Result<void>::failure("The autosave copy is damaged", "nested too deeply");
    juce::var parsed;
    if (const auto r = juce::JSON::parse(text, parsed); r.failed() || ! parsed.isObject())
        return Result<void>::failure("The autosave copy is damaged", r.getErrorMessage().toStdString());
    if (parsed["autosave"]["format"]["name"].toString() != kAutosaveFormat)
        return Result<void>::failure("This is not an autosave copy");
    juce::var root = parsed["root"];
    if (! root.isObject() || ! root["project"].isObject() || ! root["meta"].isObject())
        return Result<void>::failure("The autosave copy is incomplete");
    if (root["meta"]["format"]["name"].toString().toStdString() != kNativeFormatName)
        return Result<void>::failure("The autosave copy is not in the native format");
    if (const auto* pats = root["patterns"].getArray(); pats == nullptr || pats->size() != model::kScenes)
        return Result<void>::failure("The autosave copy is incomplete");
    const int version = schemaVersionOf(root);
    const auto& runner = MigrationRunner::builtin();
    if (version > runner.currentVersion())
    {
        report.readOnly = true;
        report.warnings.push_back("This autosave was written by a newer version: it opens read-only.");
    }
    else if (auto migrated = runner.run(root, version); ! migrated)
        return migrated.error();
    NativeCodec::decode(root, out, report);
    std::vector<std::string> problems;
    if (! model::validate(out, problems, 50))
        for (auto& p : problems)
            report.warnings.push_back("invalid value: " + p);
    return {};
}

Result<void> Autosave::readInto(const juce::File& f, model::Project& out, ImportReport& report)
{
    if (! f.existsAsFile())
        return Result<void>::failure("The autosave copy " + f.getFullPathName().toStdString() + " is missing");
    if (f.getSize() > kMaxAutosaveBytes)
        return Result<void>::failure("The autosave copy " + f.getFullPathName().toStdString() + " is too large");
    return decodeText(f.loadFileAsString(), out, report);
}
} // namespace services::store
