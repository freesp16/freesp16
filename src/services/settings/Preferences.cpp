#include "services/settings/Preferences.h"

#include "ProjectIdentity.h"
#include "services/store/AtomicWriter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <thread>

namespace services::settings
{
namespace
{
constexpr std::string_view kMachineIds[] = {"transport.metronomeMonitor", "transport.metronomeVolume",
                                            "transport.preCount"};

bool startsWith(std::string_view s, std::string_view p) noexcept { return s.substr(0, p.size()) == p; }

// "midi.portMode#2" → "midi.portMode"
std::string_view baseOf(std::string_view key) noexcept
{
    const auto hash = key.find('#');
    return hash == std::string_view::npos ? key : key.substr(0, hash);
}

std::string formatName() { return std::string(identity::name) + "-preferences"; }

Preferences*& installedSlot() noexcept
{
    static Preferences* p = nullptr;
    return p;
}
} // namespace

// --- PrefRegistry ----------------------------------------------------------------------------------------------------
PrefRegistry& PrefRegistry::builtin() noexcept
{
    static PrefRegistry registry;
    return registry;
}

void PrefRegistry::add(const PrefSpec& s)
{
    if (s.key.empty() || find(s.key) != nullptr)
        return; // duplicates keep the first registration (a test lists every key once)
    specs_.push_back(s);
    core::ParamSpec p;
    p.id = s.key;
    p.label = s.label;
    p.shortLabel = s.label;
    p.group = s.section;
    p.type = s.type;
    p.minValue = s.minValue;
    p.maxValue = s.maxValue;
    p.defaultValue = s.def;
    p.enumLabels = s.values;
    p.featureFlag = s.featureFlag;
    p.scope = core::ParamScope::Global;
    params_.push_back(p);
}

const PrefSpec* PrefRegistry::find(std::string_view key) const noexcept
{
    for (const auto& s : specs_)
        if (s.key == key)
            return &s;
    return nullptr;
}

const core::ParamSpec* PrefRegistry::paramOf(std::string_view key) const noexcept
{
    for (const auto& p : params_)
        if (p.id == key)
            return &p;
    return nullptr;
}

std::vector<const PrefSpec*> PrefRegistry::all() const
{
    std::vector<const PrefSpec*> out;
    for (const auto& s : specs_)
        out.push_back(&s);
    return out;
}

// --- keys ------------------------------------------------------------------------------------------------------------
std::span<const std::string_view> machineSettingIds() noexcept { return kMachineIds; }

bool persists(const core::ParamSpec& spec) noexcept
{
    if (std::find(std::begin(kMachineIds), std::end(kMachineIds), spec.id) != std::end(kMachineIds))
        return true;
    if (spec.scope != core::ParamScope::Global)
        return false;
    const auto key = spec.storageKey();
    if (startsWith(key, "session.") || startsWith(key, "internal."))
        return false;
    return spec.coarseStep != 0.0f || spec.fineStep != 0.0f; // a read-only display (browse.info) is never stored
}

std::string keyOf(const core::ParamSpec& spec, std::uint16_t scope)
{
    std::string key(spec.storageKey());
    if (scope != 0)
        key += "#" + std::to_string(scope);
    return key;
}

// --- Preferences -----------------------------------------------------------------------------------------------------
Preferences::Preferences(const core::ParamRegistry& params, Executor executor)
    : params_(params), executor_(std::move(executor))
{
}

Preferences::~Preferences()
{
    waitIdle();
    if (installedSlot() == this)
        installedSlot() = nullptr;
}

const core::ParamSpec* Preferences::specOf(std::string_view key) const
{
    const auto base = baseOf(key);
    if (const auto* p = PrefRegistry::builtin().paramOf(base))
        return p;
    if (const auto* p = params_.find(base); p != nullptr && persists(*p) && p->storageKey() == base)
        return p;
    // A ParamSpec stored under a different key (storage.key): search by the key.
    for (const auto& p : params_.all())
        if (p.storageKey() == base && persists(p))
            return &p;
    return nullptr;
}

float Preferences::get(std::string_view key) const
{
    if (auto it = values_.find(key); it != values_.end())
        return it->second;
    const auto* spec = specOf(key);
    return spec != nullptr ? spec->defaultValue : 0.0f;
}

bool Preferences::has(std::string_view key) const { return values_.find(key) != values_.end(); }

void Preferences::set(std::string_view key, float value)
{
    if (key.empty() || ! std::isfinite(value))
        return;
    if (const auto* spec = specOf(key))
    {
        value = spec->clamp(value);
        if (spec->type != core::ParamType::Continuous && spec->type != core::ParamType::Time)
            value = std::round(value);
    }
    if (auto it = values_.find(key); it != values_.end())
    {
        if (it->second == value)
            return;
        it->second = value;
    }
    else
    {
        if (get(key) == value && specOf(key) != nullptr)
        {
            values_.emplace(std::string(key), value); // the default, now explicit: written, but no observer
            if (! dirty_)
                firstDirtyMs_ = now_;
            dirty_ = true;
            lastChangeMs_ = now_;
            return;
        }
        values_.emplace(std::string(key), value);
    }
    changed(key);
}

void Preferences::reset(std::string_view key)
{
    if (auto it = values_.find(key); it != values_.end())
    {
        const float before = it->second;
        values_.erase(it);
        if (get(key) != before)
            changed(key);
        else
            dirty_ = true;
    }
}

std::string Preferences::getString(std::string_view key) const
{
    auto it = strings_.find(key);
    return it != strings_.end() ? it->second : std::string {};
}

void Preferences::setString(std::string_view key, std::string value)
{
    auto it = strings_.find(key);
    if (it != strings_.end() && it->second == value)
        return;
    strings_[std::string(key)] = std::move(value);
    changed(key);
}

std::vector<std::string> Preferences::getList(std::string_view key) const
{
    auto it = lists_.find(key);
    return it != lists_.end() ? it->second : std::vector<std::string> {};
}

void Preferences::setList(std::string_view key, std::vector<std::string> v)
{
    auto it = lists_.find(key);
    if (it != lists_.end() && it->second == v)
        return;
    lists_[std::string(key)] = std::move(v);
    changed(key);
}

core::Subscription Preferences::observe(std::string_view prefix, std::function<void(std::string_view)> fn)
{
    const int id = nextObserver_++;
    observers_.push_back({id, std::string(prefix), std::move(fn)});
    return core::Subscription([this, id] {
        observers_.erase(std::remove_if(observers_.begin(), observers_.end(), [id](const Observer& o) { return o.id == id; }),
                         observers_.end());
    });
}

void Preferences::changed(std::string_view key)
{
    if (! dirty_)
        firstDirtyMs_ = now_;
    dirty_ = true;
    lastChangeMs_ = now_;
    const std::string k(key); // an observer may change the maps
    const auto copy = observers_;
    for (const auto& o : copy)
        if (startsWith(k, o.prefix) && o.fn)
            o.fn(k);
}

void Preferences::tick(double nowMs)
{
    now_ = nowMs;
    if (! dirty_ || inFlight_->load() || file_ == juce::File())
        return;
    if (nowMs - lastChangeMs_ >= kDebounceMs || nowMs - firstDirtyMs_ >= kMaxDelayMs)
        startWrite();
}

void Preferences::startWrite()
{
    dirty_ = false;
    inFlight_->store(true);
    auto text = std::make_shared<std::string>(toJson());
    auto job = [text, target = file_, inFlight = inFlight_, writes = writes_, error = error_]
    {
        target.getParentDirectory().createDirectory();
        const auto r = store::AtomicWriter::writeFileAtomically(target, text->data(), text->size());
        {
            std::lock_guard<std::mutex> lock(error->mutex);
            error->text = r ? std::string {} : r.error().message;
        }
        if (r)
            writes->fetch_add(1);
        inFlight->store(false);
    };
    if (executor_)
        executor_(std::move(job));
    else
        job();
}

void Preferences::waitIdle() const
{
    while (inFlight_->load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void Preferences::flush()
{
    waitIdle();
    if (! dirty_ || file_ == juce::File())
        return;
    dirty_ = false;
    inFlight_->store(true);
    const auto text = toJson();
    file_.getParentDirectory().createDirectory();
    const auto r = store::AtomicWriter::writeFileAtomically(file_, text.data(), text.size());
    {
        std::lock_guard<std::mutex> lock(error_->mutex);
        error_->text = r ? std::string {} : r.error().message;
    }
    if (r)
        writes_->fetch_add(1);
    else
        dirty_ = true;
    inFlight_->store(false);
}

std::string Preferences::lastError() const
{
    std::lock_guard<std::mutex> lock(error_->mutex);
    return error_->text;
}

// --- JSON ------------------------------------------------------------------------------------------------------------
std::string Preferences::toJson() const
{
    auto* root = new juce::DynamicObject();
    juce::var rootVar(root);
    if (auto* extra = extra_.getDynamicObject())
        for (const auto& p : extra->getProperties())
            root->setProperty(p.name, p.value);
    root->setProperty("format", juce::String(formatName()));
    root->setProperty("version", kSchemaVersion);
    auto* values = new juce::DynamicObject();
    for (const auto& [k, v] : values_)
    {
        // Integers stay integers in the file (readable, and exact on the way back).
        if (std::floor(v) == v && std::fabs(v) < 1e9f)
            values->setProperty(juce::Identifier(juce::String(k)), static_cast<int>(v));
        else
            values->setProperty(juce::Identifier(juce::String(k)), static_cast<double>(v));
    }
    root->setProperty("values", juce::var(values));
    auto* strings = new juce::DynamicObject();
    for (const auto& [k, v] : strings_)
        strings->setProperty(juce::Identifier(juce::String(k)), juce::String(v));
    root->setProperty("strings", juce::var(strings));
    auto* lists = new juce::DynamicObject();
    for (const auto& [k, v] : lists_)
    {
        juce::Array<juce::var> items;
        for (const auto& s : v)
            items.add(juce::String(s));
        lists->setProperty(juce::Identifier(juce::String(k)), items);
    }
    root->setProperty("lists", juce::var(lists));
    return juce::JSON::toString(rootVar, juce::JSON::FormatOptions {}.withSpacing(juce::JSON::Spacing::multiLine).withMaxDecimalPlaces(6))
               .toStdString()
           + "\n";
}

Result<void> Preferences::fromJson(const juce::String& text)
{
    juce::var root;
    if (const auto parsed = juce::JSON::parse(text, root); parsed.failed() || ! root.isObject())
        return Result<void>::failure("The preferences file is damaged; the defaults are used.",
                                     parsed.failed() ? parsed.getErrorMessage().toStdString() : "not an object");
    auto* obj = root.getDynamicObject();
    int version = 0;
    if (obj->hasProperty("version"))
        version = static_cast<int>(obj->getProperty("version"));
    std::map<std::string, float, std::less<>> values;
    std::map<std::string, std::string, std::less<>> strings;
    std::map<std::string, std::vector<std::string>, std::less<>> lists;
    auto* extra = new juce::DynamicObject();
    juce::var extraVar(extra);
    auto number = [&](const juce::String& name, const juce::var& v)
    {
        if (! (v.isInt() || v.isInt64() || v.isDouble() || v.isBool()))
            return;
        const auto f = static_cast<float>(static_cast<double>(v));
        if (! std::isfinite(f))
            return;
        const std::string key = name.toStdString();
        const auto* spec = specOf(key);
        values[key] = spec != nullptr ? spec->clamp(f) : f;
    };
    if (version == 0)
    {
        // Schema 0 (pre-release): a flat {key: number | string} object. Migration: numbers → values, strings →
        // strings; nothing was renamed.
        for (const auto& p : obj->getProperties())
        {
            if (p.value.isString())
                strings[p.name.toString().toStdString()] = p.value.toString().toStdString();
            else
                number(p.name.toString(), p.value);
        }
    }
    else
    {
        // Schema 1 (and newer: what this build understands is read, the rest kept).
        for (const auto& p : obj->getProperties())
        {
            const auto name = p.name.toString();
            if (name == "values")
            {
                if (auto* v = p.value.getDynamicObject())
                    for (const auto& q : v->getProperties())
                        number(q.name.toString(), q.value);
            }
            else if (name == "strings")
            {
                if (auto* v = p.value.getDynamicObject())
                    for (const auto& q : v->getProperties())
                        if (q.value.isString())
                            strings[q.name.toString().toStdString()] = q.value.toString().toStdString();
            }
            else if (name == "lists")
            {
                if (auto* v = p.value.getDynamicObject())
                    for (const auto& q : v->getProperties())
                        if (const auto* arr = q.value.getArray())
                        {
                            auto& out = lists[q.name.toString().toStdString()];
                            for (const auto& item : *arr)
                                if (item.isString())
                                    out.push_back(item.toString().toStdString());
                        }
            }
            else if (name != "format" && name != "version")
                extra->setProperty(p.name, p.value);
        }
    }
    values_ = std::move(values);
    strings_ = std::move(strings);
    lists_ = std::move(lists);
    extra_ = extraVar;
    // A migrated (older) file is rewritten in the current schema on the next tick.
    dirty_ = version != kSchemaVersion;
    firstDirtyMs_ = lastChangeMs_ = now_;
    return {};
}

Result<void> Preferences::load(const juce::File& file)
{
    file_ = file;
    // A crash during a save may leave <file>.tmp-<pid>-<n> (the rename never happened): the target is intact.
    for (const auto& f : file.getParentDirectory().findChildFiles(juce::File::findFiles, false,
                                                                   file.getFileName() + ".tmp-*"))
        f.deleteFile();
    values_.clear();
    strings_.clear();
    lists_.clear();
    extra_ = juce::var();
    dirty_ = false;
    if (! file.existsAsFile())
        return {};
    auto r = fromJson(file.loadFileAsString());
    if (! r)
    {
        const auto aside = file.getSiblingFile(file.getFileName() + ".corrupt");
        aside.deleteFile();
        file.moveFileTo(aside);
        values_.clear();
        strings_.clear();
        lists_.clear();
        return Result<void>::failure(r.error().message, r.error().detail + " (moved to " +
                                                            aside.getFileName().toStdString() + ")");
    }
    return {};
}

// --- export / import (SET-012) ---------------------------------------------------------------------------------------
Result<void> Preferences::exportTo(const juce::File& target) const
{
    juce::var prefs;
    if (const auto parsed = juce::JSON::parse(juce::String(toJson()), prefs); parsed.failed())
        return Result<void>::failure("Could not export the preferences.", parsed.getErrorMessage().toStdString());
    auto* root = new juce::DynamicObject();
    juce::var rootVar(root);
    root->setProperty("format", juce::String(formatName() + "-export"));
    root->setProperty("version", kSchemaVersion);
    root->setProperty("preferences", prefs);
    auto* profiles = new juce::DynamicObject();
    if (profiles_.isDirectory())
    {
        auto files = profiles_.findChildFiles(juce::File::findFiles, false, "*.json");
        files.sort();
        for (const auto& f : files)
        {
            juce::var profile;
            if (juce::JSON::parse(f.loadFileAsString(), profile).wasOk())
                profiles->setProperty(juce::Identifier(f.getFileName()), profile);
        }
    }
    root->setProperty("controllers", juce::var(profiles));
    const auto text = juce::JSON::toString(rootVar, juce::JSON::FormatOptions {}.withSpacing(juce::JSON::Spacing::multiLine).withMaxDecimalPlaces(6))
                          .toStdString()
                      + "\n";
    target.getParentDirectory().createDirectory();
    return store::AtomicWriter::writeFileAtomically(target, text.data(), text.size());
}

Result<void> Preferences::importFrom(const juce::File& source)
{
    juce::var root;
    if (const auto parsed = juce::JSON::parse(source.loadFileAsString(), root); parsed.failed() || ! root.isObject())
        return Result<void>::failure("This file is not a preferences export.",
                                     parsed.failed() ? parsed.getErrorMessage().toStdString() : "not an object");
    if (! root.getProperty("format", {}).toString().endsWith("-preferences-export"))
        return Result<void>::failure("This file is not a preferences export.", "format");
    Preferences incoming(params_);
    if (auto r = incoming.fromJson(juce::JSON::toString(root.getProperty("preferences", {}))); ! r)
        return r;
    // Apply through set() so every observer sees the change (the engine, the theme, the session).
    std::vector<std::string> gone;
    for (const auto& [k, v] : values_)
        if (incoming.values_.find(k) == incoming.values_.end())
            gone.push_back(k);
    for (const auto& k : gone)
        reset(k);
    for (const auto& [k, v] : incoming.values_)
        set(k, v);
    for (const auto& [k, v] : incoming.strings_)
        setString(k, v);
    for (const auto& [k, v] : incoming.lists_)
        setList(k, v);
    if (auto* profiles = root.getProperty("controllers", {}).getDynamicObject(); profiles != nullptr && profiles_ != juce::File())
    {
        profiles_.createDirectory();
        for (const auto& p : profiles->getProperties())
        {
            const auto name = juce::File::createLegalFileName(p.name.toString());
            if (! name.endsWithIgnoreCase(".json"))
                continue;
            const auto text = juce::JSON::toString(p.value).toStdString() + "\n";
            if (auto r = store::AtomicWriter::writeFileAtomically(profiles_.getChildFile(name), text.data(), text.size()); ! r)
                return r;
        }
    }
    return {};
}

// --- FeatureFlags ----------------------------------------------------------------------------------------------------
bool FeatureFlags::isOn(std::string_view flag) const noexcept
{
    if (flag.empty())
        return true;
    if (prefs_.get(kMaster) < 0.5f)
        return false;
    if (flag == kMaster)
        return true;
    if (startsWith(flag, "ext."))
        return prefs_.get(flag) >= 0.5f;
    std::string key = "ext.";
    key.append(flag);
    return prefs_.get(key) >= 0.5f;
}

std::vector<std::string> FeatureFlags::enabled() const
{
    std::vector<std::string> out;
    if (prefs_.get(kMaster) < 0.5f)
        return out;
    for (const auto& [k, v] : prefs_.values())
        if (startsWith(k, "ext.") && k != kMaster && v >= 0.5f)
            out.push_back(k.substr(4));
    for (const auto* s : PrefRegistry::builtin().all())
        if (startsWith(s->key, "ext.") && s->key != kMaster && ! prefs_.has(s->key) && s->def >= 0.5f)
            out.emplace_back(s->key.substr(4));
    return out;
}

Preferences* installed() noexcept { return installedSlot(); }
void install(Preferences* p) noexcept { installedSlot() = p; }
} // namespace services::settings
