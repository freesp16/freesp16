#include "services/store/NativeCodec.h"

#include "services/store/AtomicWriter.h"
#include "services/store/NativeCodecSchema.h"

#include "ProjectIdentity.h"
#include "core/model/ModelOps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace services::store
{
namespace detail
{
int lockSlotOf(std::string_view name) noexcept
{
    for (std::size_t i = 0; i < kLockSlotNames.size(); ++i)
        if (kLockSlotNames[i] == name)
            return static_cast<int>(i);
    return -1;
}

const m::Project& defaultProject()
{
    // Header defaults, not makeDefaultProject(): sparse files omit members equal to these. Default-initialised
    // on the heap (no `{}` on the big arrays, see core/model/ProjectStorage.cpp).
    static const std::unique_ptr<const m::Project> p(new m::Project);
    return *p;
}

const m::Pattern& defaultPatternValue()
{
    static const std::unique_ptr<const m::Pattern> p(new m::Pattern);
    return *p;
}

namespace
{
constexpr std::string_view kNative = "native.";

std::string_view encodingName(m::Extras::Encoding e)
{
    switch (e)
    {
        case m::Extras::Encoding::Json: return "json";
        case m::Extras::Encoding::Text: return "text";
        case m::Extras::Encoding::Base64: return "base64";
    }
    return "text";
}

std::optional<m::Extras::Encoding> encodingOf(const juce::String& s)
{
    if (s == "json")
        return m::Extras::Encoding::Json;
    if (s == "text")
        return m::Extras::Encoding::Text;
    if (s == "base64")
        return m::Extras::Encoding::Base64;
    return std::nullopt;
}

// True when `text` is the canonical compact form of one JSON value (then it can be written back as a key).
bool isCanonicalJson(const std::string& text)
{
    juce::var wrapped;
    if (! juce::JSON::parse("[" + juce::String::fromUTF8(text.c_str()) + "]", wrapped).wasOk())
        return false;
    const auto* a = wrapped.getArray();
    return a != nullptr && a->size() == 1 && canonicalJson(a->getReference(0)) == text;
}

std::optional<std::int64_t> integerOf(const juce::var& v)
{
    if (v.isInt() || v.isInt64())
        return static_cast<juce::int64>(v);
    if (v.isDouble())
    {
        const double d = v;
        if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9.0e15)
            return static_cast<std::int64_t>(d);
    }
    return std::nullopt;
}

// "step[s].track[t].<key>" → s, t, key.
bool parseStepKey(std::string_view k, int& s, int& t, std::string_view& rest)
{
    int consumed = 0;
    const std::string copy(k);
    if (std::sscanf(copy.c_str(), "step[%d].track[%d].%n", &s, &t, &consumed) != 2 || consumed <= 0)
        return false;
    rest = k.substr(static_cast<std::size_t>(consumed));
    return ! rest.empty() && s >= 0 && s < m::kSteps && t >= 0 && t < m::kTracks;
}
} // namespace

// --------------------------------------------------------------------------------------------------------- Encoder
const m::Extras::Entry* Encoder::unknownEnum(std::string_view name)
{
    auto& f = frames_.back();
    if (f.extras == nullptr || f.extras->empty())
        return nullptr;
    for (const auto& e : f.extras->entries)
        if (e.encoding == m::Extras::Encoding::Text && e.key.size() == kNative.size() + name.size() &&
            e.key.compare(0, kNative.size(), kNative) == 0 && e.key.compare(kNative.size(), name.size(), name) == 0)
        {
            f.consumed.push_back(&e);
            return &e;
        }
    return nullptr;
}

void Encoder::end()
{
    auto& f = frames_.back();
    if (f.extras == nullptr || f.extras->empty())
        return;
    std::vector<const m::Extras::Entry*> foreign;
    for (const auto& e : f.extras->entries)
    {
        if (std::find(f.consumed.begin(), f.consumed.end(), &e) != f.consumed.end())
            continue;
        if (e.encoding == m::Extras::Encoding::Json && e.key.size() > kNative.size() &&
            e.key.compare(0, kNative.size(), kNative) == 0)
        {
            const std::string_view rest = std::string_view(e.key).substr(kNative.size());
            const bool clash = rest == "_extras" || rest.substr(0, 5) == "step[" ||
                               std::find(f.known.begin(), f.known.end(), rest) != f.known.end();
            if (! clash && isCanonicalJson(e.value))
            {
                w_.key(rest);
                w_.raw(e.value);
                f.known.push_back(rest);
                continue;
            }
        }
        foreign.push_back(&e);
    }
    if (foreign.empty())
        return;
    w_.key("_extras");
    w_.beginArray(Layout::Lines);
    for (const auto* e : foreign)
    {
        w_.beginObject(Layout::Inline);
        w_.key("key");
        w_.value(std::string_view(e->key));
        w_.key("encoding");
        w_.value(encodingName(e->encoding));
        w_.key("value");
        w_.value(std::string_view(e->value));
        w_.endObject();
    }
    w_.endArray();
}

void Encoder::effectMemory(const char* name, const std::vector<m::EffectSettings>& memory)
{
    known(name);
    if (! full_ && memory.empty())
        return;
    static const m::EffectSettings d {};
    w_.key(name);
    w_.beginObject(Layout::Inline);
    for (const auto& e : memory)
    {
        const auto stored = m::storedName(e.type);
        w_.key(stored.empty() ? std::string_view("Off") : stored);
        entity(e, d, Layout::Inline);
    }
    w_.endObject();
}

void Encoder::ribbonUser(const char* name, const std::array<std::array<m::RibbonItem, 8>, 2>& v,
                         const std::array<std::array<m::RibbonItem, 8>, 2>& d)
{
    known(name);
    if (! full_ && v == d)
        return;
    w_.key(name);
    w_.beginArray(Layout::Lines);
    for (std::size_t u = 0; u < v.size(); ++u)
    {
        w_.beginArray(Layout::Lines);
        for (std::size_t i = 0; i < v[u].size(); ++i)
            entity(v[u][i], d[u][i], Layout::Inline);
        w_.endArray();
    }
    w_.endArray();
}

void Encoder::location(const std::string& modelPath)
{
    known("location");
    known("path");
    if (modelPath.empty() && ! full_)
        return;
    auto loc = splitLocation(modelPath);
    if (joinLocation(loc) != modelPath)
        loc = {LocationKind::Absolute, {}, modelPath};
    w_.key("location");
    w_.value(std::string_view(locationTag(loc)));
    w_.key("path");
    w_.value(std::string_view(loc.path));
}

void Encoder::trackFlags(const char* name, const std::array<bool, m::kTracks>& flags)
{
    known(name);
    bool any = false;
    for (const bool b : flags)
        any = any || b;
    if (! any && ! full_)
        return;
    w_.key(name);
    w_.beginArray(Layout::Inline);
    for (int t = 0; t < m::kTracks; ++t)
        if (flags[static_cast<std::size_t>(t)])
            w_.value(t + 1);
    w_.endArray();
}

void Encoder::steps(const m::Pattern& p)
{
    known("steps");
    struct StepExtra
    {
        int s, t;
        std::string_view key;
        const m::Extras::Entry* entry;
    };
    std::vector<StepExtra> stepExtras;
    auto& f = frames_.back();
    if (f.extras != nullptr)
        for (const auto& e : f.extras->entries)
        {
            int s = 0, t = 0;
            std::string_view rest;
            if (e.encoding == m::Extras::Encoding::Json && e.key.compare(0, kNative.size(), kNative) == 0 &&
                parseStepKey(std::string_view(e.key).substr(kNative.size()), s, t, rest) && isCanonicalJson(e.value))
            {
                stepExtras.push_back({s, t, rest, &e});
                f.consumed.push_back(&e);
            }
        }

    static const m::Step empty {};
    bool any = ! stepExtras.empty();
    for (const auto& row : p.steps)
        for (const auto& cell : row)
            any = any || ! (cell == empty);
    if (! any)
        return;

    w_.key("steps");
    w_.beginArray(Layout::Lines);
    for (int s = 0; s < m::kSteps; ++s)
        for (int t = 0; t < m::kTracks; ++t)
        {
            const auto& cell = p.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            const bool hasExtras = std::any_of(stepExtras.begin(), stepExtras.end(),
                                               [&](const StepExtra& x) { return x.s == s && x.t == t; });
            if (cell == empty && ! hasExtras)
                continue;
            w_.beginObject(Layout::Inline);
            w_.key("track");
            w_.value(t + 1);
            w_.key("step");
            w_.value(s + 1);
            if (cell.trigger != m::Step::kEmpty)
            {
                w_.key("trigger");
                w_.value(static_cast<int>(cell.trigger));
            }
            if (cell.value2 != m::Step::kNoValue2)
            {
                w_.key("velocity");
                w_.value(static_cast<int>(cell.value2));
            }
            if (cell.triggerType != m::TriggerType::Full)
            {
                w_.key("type");
                put(cell.triggerType);
            }
            if (cell.reserved != 0)
            {
                w_.key("reserved");
                w_.value(static_cast<int>(cell.reserved));
            }
            if (cell.lockIndex != m::Step::kNoLocks && cell.lockIndex < p.locks.size())
            {
                const auto& l = p.locks[cell.lockIndex];
                w_.key("locks");
                w_.beginObject(Layout::Inline);
                bool changes = false;
                for (int slot = 0; slot < m::kLockSlots; ++slot)
                    if ((l.mask >> slot) & 1u)
                    {
                        w_.key(kLockSlotNames[static_cast<std::size_t>(slot)]);
                        w_.value(l.values[static_cast<std::size_t>(slot)]);
                        changes = changes || l.change[static_cast<std::size_t>(slot)] != m::LockChange::Revert;
                    }
                w_.endObject();
                if (changes)
                {
                    w_.key("lockChange");
                    w_.beginObject(Layout::Inline);
                    for (int slot = 0; slot < m::kLockSlots; ++slot)
                        if (((l.mask >> slot) & 1u) && l.change[static_cast<std::size_t>(slot)] != m::LockChange::Revert)
                        {
                            w_.key(kLockSlotNames[static_cast<std::size_t>(slot)]);
                            put(l.change[static_cast<std::size_t>(slot)]);
                        }
                    w_.endObject();
                }
            }
            for (const auto& x : stepExtras)
                if (x.s == s && x.t == t)
                {
                    w_.key(x.key);
                    w_.raw(x.entry->value);
                }
            w_.endObject();
        }
    w_.endArray();
}

// --------------------------------------------------------------------------------------------------------- Decoder
void DecodeContext::warn(std::string text)
{
    constexpr int kMaxWarnings = 200;
    if (++warnings <= kMaxWarnings)
        report.warnings.push_back(std::move(text));
    else if (warnings == kMaxWarnings + 1)
        report.warnings.emplace_back("(more warnings not listed)");
}

void DecodeContext::unmapped(std::string text)
{
    if (report.unmapped.size() < 200)
        report.unmapped.push_back(std::move(text));
}

bool Decoder::pushFrame(const juce::var& value, const std::string& where)
{
    const auto* object = value.getDynamicObject();
    if (object == nullptr)
    {
        if (! value.isVoid())
            ctx_.warn((path_.empty() ? where : path_ + "." + where) + ": expected an object");
        return false;
    }
    Frame f;
    f.object = object;
    f.pathLength = path_.size();
    frames_.push_back(std::move(f));
    if (! path_.empty())
        path_ += '.';
    path_ += where;
    return true;
}

void Decoder::popFrame()
{
    end();
    path_.resize(frames_.back().pathLength);
    frames_.pop_back();
}

const juce::var* Decoder::get(const char* name)
{
    auto& f = frames_.back();
    const auto& props = f.object->getProperties();
    for (int i = 0; i < props.size(); ++i)
    {
        const auto* nv = props.getVarPointerAt(i);
        if (props.getName(i) == juce::StringRef(name))
        {
            if (i < 64)
                f.usedLow |= std::uint64_t {1} << i;
            else
                f.usedHigh.push_back(i);
            return nv;
        }
    }
    return nullptr;
}

void Decoder::end()
{
    auto& f = frames_.back();
    if (f.object == nullptr)
        return;
    std::vector<m::Extras::Entry> unknownKeys, foreign;
    const auto& props = f.object->getProperties();
    for (int i = 0; i < props.size(); ++i)
    {
        const bool used = i < 64 ? ((f.usedLow >> i) & 1u) != 0
                                 : std::find(f.usedHigh.begin(), f.usedHigh.end(), i) != f.usedHigh.end();
        if (used)
            continue;
        const auto name = props.getName(i).toString().toStdString();
        const auto& value = *props.getVarPointerAt(i);
        if (f.extras == nullptr)
        {
            ctx_.unmapped(path_ + "." + name);
            continue;
        }
        if (name == "_extras")
        {
            if (const auto* a = value.getArray())
                for (const auto& item : *a)
                {
                    const auto encoding = encodingOf(item["encoding"].toString());
                    if (! item["key"].isString() || ! item["value"].isString() || ! encoding)
                    {
                        ctx_.warn(path_ + "._extras: malformed entry dropped");
                        continue;
                    }
                    foreign.push_back(
                        {item["key"].toString().toStdString(), *encoding, item["value"].toString().toStdString()});
                }
            continue;
        }
        unknownKeys.push_back({std::string(kNative) + name, m::Extras::Encoding::Json, canonicalJson(value)});
    }
    if (f.extras != nullptr)
    {
        for (auto* list : {&f.unknownEnums, &unknownKeys, &foreign})
            for (auto& e : *list)
                f.extras->entries.push_back(std::move(e));
    }
    f.object = nullptr; // end() runs once per frame
}

void Decoder::warn(const char* name, const std::string& what) { ctx_.warn(path_ + "." + name + ": " + what); }

void Decoder::unknownEnum(const char* name, const std::string& text)
{
    auto& f = frames_.back();
    if (f.extras == nullptr)
    {
        warn(name, "unknown value '" + text + "' (default kept)");
        return;
    }
    warn(name, "unknown value '" + text + "' kept for writing back (default used)");
    f.unknownEnums.push_back({std::string(kNative) + name, m::Extras::Encoding::Text, text});
}

const juce::Array<juce::var>* Decoder::array(const char* name, std::size_t expected)
{
    const auto* value = get(name);
    if (value == nullptr || value->isVoid())
        return nullptr;
    const auto* a = value->getArray();
    if (a == nullptr)
    {
        warn(name, "expected an array");
        return nullptr;
    }
    if (expected != 0 && static_cast<std::size_t>(a->size()) != expected)
        warn(name, "expected " + std::to_string(expected) + " entries, found " + std::to_string(a->size()));
    return a;
}

void Decoder::read(const char* name, const juce::var& value, bool& v)
{
    if (value.isBool())
        v = static_cast<bool>(value);
    else
        warn(name, "expected true or false");
}

void Decoder::read(const char* name, const juce::var& value, int& v)
{
    const auto i = integerOf(value);
    if (i && *i >= INT32_MIN && *i <= INT32_MAX)
        v = static_cast<int>(*i);
    else
        warn(name, "expected an integer");
}

void Decoder::read(const char* name, const juce::var& value, std::uint8_t& v)
{
    const auto i = integerOf(value);
    if (i && *i >= 0 && *i <= 255)
        v = static_cast<std::uint8_t>(*i);
    else
        warn(name, "expected an integer 0-255");
}

void Decoder::read(const char* name, const juce::var& value, std::int64_t& v)
{
    if (const auto i = integerOf(value))
        v = *i;
    else
        warn(name, "expected an integer");
}

void Decoder::read(const char* name, const juce::var& value, double& v)
{
    if (value.isDouble() || value.isInt() || value.isInt64())
        v = static_cast<double>(value);
    else
        warn(name, "expected a number");
}

void Decoder::read(const char* name, const juce::var& value, std::string& v)
{
    if (value.isString())
        v = value.toString().toStdString();
    else
        warn(name, "expected a string");
}

void Decoder::read(const char* name, const juce::var& value, std::optional<double>& v)
{
    if (value.isVoid())
        v.reset();
    else if (value.isDouble() || value.isInt() || value.isInt64())
    {
        const double d = value;
        if (d == 0.0)
            v.reset(); // data-model.md: 0 = no BPM
        else
            v = d;
    }
    else
        warn(name, "expected a number or null");
}

void Decoder::read(const char* name, const juce::var& value, std::vector<std::int64_t>& v)
{
    const auto* a = value.getArray();
    if (a == nullptr)
        return warn(name, "expected an array");
    v.clear();
    for (const auto& item : *a)
    {
        if (const auto i = integerOf(item))
            v.push_back(*i);
        else
            warn(name, "expected integers");
    }
}

void Decoder::effectMemory(const char* name, std::vector<m::EffectSettings>& memory)
{
    const auto* value = get(name);
    if (value == nullptr || value->isVoid())
        return;
    const auto* object = value->getDynamicObject();
    if (object == nullptr)
        return warn(name, "expected an object");
    static const m::EffectSettings d {};
    memory.clear();
    for (const auto& property : object->getProperties())
    {
        const auto typeName = property.name.toString().toStdString();
        const auto type = m::enumFromStored<m::EffectType>(typeName);
        if (! type)
        {
            warn(name, "unknown effect type '" + typeName + "' dropped");
            continue;
        }
        memory.emplace_back();
        memory.back().type = *type;
        entity(property.value, memory.back(), d, std::string(name) + "." + typeName);
    }
}

void Decoder::ribbonUser(const char* name, std::array<std::array<m::RibbonItem, 8>, 2>& v,
                         const std::array<std::array<m::RibbonItem, 8>, 2>& d)
{
    const auto* outer = array(name, 2);
    if (outer == nullptr)
        return;
    for (int u = 0; u < std::min(2, outer->size()); ++u)
    {
        const auto* inner = outer->getReference(u).getArray();
        if (inner == nullptr)
        {
            warn(name, "expected arrays of 8 items");
            continue;
        }
        for (int i = 0; i < std::min(8, inner->size()); ++i)
            entity(inner->getReference(i), v[static_cast<std::size_t>(u)][static_cast<std::size_t>(i)],
                   d[static_cast<std::size_t>(u)][static_cast<std::size_t>(i)],
                   std::string(name) + "[" + std::to_string(u) + "][" + std::to_string(i) + "]");
    }
}

void Decoder::location(std::string& modelPath)
{
    const auto* tag = get("location");
    const auto* path = get("path");
    if (path == nullptr)
        return;
    if (! path->isString())
        return warn("path", "expected a string");
    const auto text = path->toString().toStdString();
    const auto tagText = tag != nullptr && tag->isString() ? tag->toString().toStdString() : std::string("absolute");
    if (const auto loc = parseLocationTag(tagText, text))
        modelPath = joinLocation(*loc);
    else
    {
        warn("location", "unknown location '" + tagText + "' kept as a path");
        modelPath = tagText + "/" + text;
    }
}

void Decoder::trackFlags(const char* name, std::array<bool, m::kTracks>& flags)
{
    const auto* a = array(name, 0);
    if (a == nullptr)
        return;
    for (const auto& item : *a)
    {
        const auto t = integerOf(item);
        if (t && *t >= 1 && *t <= m::kTracks)
            flags[static_cast<std::size_t>(*t - 1)] = true;
        else
            warn(name, "expected track numbers 1-16");
    }
}

void Decoder::steps(m::Pattern& p)
{
    const auto* a = array("steps", 0);
    if (a == nullptr)
        return;
    static const juce::Identifier kTrack("track"), kStep("step"), kTrigger("trigger"), kVelocity("velocity"),
        kType("type"), kReserved("reserved"), kLocks("locks"), kLockChange("lockChange");
    auto& frame = frames_.back();
    for (int i = 0; i < a->size(); ++i)
    {
        const auto* cellObject = a->getReference(i).getDynamicObject();
        if (cellObject == nullptr)
        {
            warn("steps", "entry " + std::to_string(i + 1) + " is not an object");
            continue;
        }
        const juce::var *track = nullptr, *step = nullptr, *trigger = nullptr, *velocity = nullptr, *type = nullptr,
                        *reserved = nullptr, *locks = nullptr, *lockChange = nullptr;
        const auto& props = cellObject->getProperties();
        for (int k = 0; k < props.size(); ++k)
        {
            const auto& id = props.getName(k);
            const auto* value = props.getVarPointerAt(k);
            if (id == kTrack)
                track = value;
            else if (id == kStep)
                step = value;
            else if (id == kTrigger)
                trigger = value;
            else if (id == kVelocity)
                velocity = value;
            else if (id == kType)
                type = value;
            else if (id == kReserved)
                reserved = value;
            else if (id == kLocks)
                locks = value;
            else if (id == kLockChange)
                lockChange = value;
        }
        const auto t = track != nullptr ? integerOf(*track) : std::nullopt;
        const auto s = step != nullptr ? integerOf(*step) : std::nullopt;
        if (! t || ! s || *t < 1 || *t > m::kTracks || *s < 1 || *s > m::kSteps)
        {
            warn("steps", "entry " + std::to_string(i + 1) + " has no valid track 1-16 and step 1-64");
            continue;
        }
        const int si = static_cast<int>(*s - 1), ti = static_cast<int>(*t - 1);
        const std::string where = "step[" + std::to_string(si) + "].track[" + std::to_string(ti) + "]";
        auto& cell = p.steps[static_cast<std::size_t>(si)][static_cast<std::size_t>(ti)];
        if (! (cell == m::Step {}))
            warn("steps", where + " appears twice (the last one wins)");
        const auto int16 = [&](const juce::var* v, std::int16_t& out, const char* what)
        {
            const auto x = integerOf(*v);
            if (x && *x >= -32768 && *x <= 32767)
                out = static_cast<std::int16_t>(*x);
            else
                warn("steps", where + "." + what + ": expected an integer");
        };
        if (trigger != nullptr)
            int16(trigger, cell.trigger, "trigger");
        if (velocity != nullptr)
            int16(velocity, cell.value2, "velocity");
        if (type != nullptr)
        {
            if (const auto e = type->isString() ? m::enumFromStored<m::TriggerType>(type->toString().toStdString())
                                                : std::nullopt)
                cell.triggerType = *e;
            else
                warn("steps", where + ".type: unknown trigger type");
        }
        if (reserved != nullptr)
        {
            const auto x = integerOf(*reserved);
            if (x && *x >= 0 && *x <= 255)
                cell.reserved = static_cast<std::uint8_t>(*x);
        }
        if (locks != nullptr)
        {
            if (const auto* lockObject = locks->getDynamicObject())
            {
                if (cell.lockIndex == m::Step::kNoLocks || cell.lockIndex >= p.locks.size())
                {
                    cell.lockIndex = static_cast<std::uint16_t>(p.locks.size());
                    p.locks.emplace_back();
                }
                auto& l = p.locks[cell.lockIndex];
                l = {};
                for (const auto& property : lockObject->getProperties())
                {
                    const auto slotName = property.name.toString().toStdString();
                    const int slot = lockSlotOf(slotName);
                    if (slot < 0)
                    {
                        warn("steps", where + ".locks: unknown slot '" + slotName + "' dropped");
                        ctx_.unmapped(path_ + "." + where + ".locks." + slotName);
                        continue;
                    }
                    if (! (property.value.isDouble() || property.value.isInt() || property.value.isInt64()))
                    {
                        warn("steps", where + ".locks." + slotName + ": expected a number");
                        continue;
                    }
                    l.mask |= std::uint64_t {1} << slot;
                    l.values[static_cast<std::size_t>(slot)] = static_cast<float>(static_cast<double>(property.value));
                }
                if (lockChange != nullptr)
                    if (const auto* changeObject = lockChange->getDynamicObject())
                        for (const auto& property : changeObject->getProperties())
                        {
                            const int slot = lockSlotOf(property.name.toString().toStdString());
                            const auto e = property.value.isString()
                                               ? m::enumFromStored<m::LockChange>(property.value.toString().toStdString())
                                               : std::nullopt;
                            if (slot >= 0 && e)
                                l.change[static_cast<std::size_t>(slot)] = *e;
                            else
                                warn("steps", where + ".lockChange: unknown entry dropped");
                        }
            }
            else
                warn("steps", where + ".locks: expected an object");
        }
        for (int k = 0; k < props.size(); ++k)
        {
            const auto& id = props.getName(k);
            if (id == kTrack || id == kStep || id == kTrigger || id == kVelocity || id == kType || id == kReserved ||
                id == kLocks || id == kLockChange)
                continue;
            const auto key = std::string(kNative) + where + "." + id.toString().toStdString();
            if (frame.extras != nullptr)
                frame.unknownEnums.push_back({key, m::Extras::Encoding::Json, canonicalJson(*props.getVarPointerAt(k))});
            else
                ctx_.unmapped(path_ + "." + key);
        }
    }
}
} // namespace detail

// ------------------------------------------------------------------------------------------------------ the codec
namespace
{
using detail::Encoder;
using detail::JsonWriter;
using Layout = JsonWriter::Layout;

juce::String sceneFileName(int scene) // 0-based scene → "patterns/scene-01.json"
{
    return "patterns/scene-" + juce::String(scene + 1).paddedLeft('0', 2) + ".json";
}

void writeFormat(JsonWriter& w, Encoder& enc)
{
    enc.known("format");
    w.key("format");
    w.beginObject(Layout::Inline);
    w.key("name");
    w.value(kNativeFormatName);
    w.key("version");
    w.value(kSchemaVersion);
    w.endObject();
}

std::vector<NativeCodec::EncodedFile> encodeFiles(const model::Project& p, const SaveOptions& options,
                                                  const std::string& created, const model::SamplePool& pool)
{
    std::vector<NativeCodec::EncodedFile> files;
    const auto& d = detail::defaultProject();
    const auto now = options.timestamp.empty() ? juce::Time::getCurrentTime().toISO8601(true).toStdString()
                                               : options.timestamp;
    {
        std::string text;
        JsonWriter w(text);
        Encoder enc(w, options.writeDefaults);
        w.beginObject(Layout::Lines);
        enc.pushFrame();
        writeFormat(w, enc);
        w.key("app");
        w.beginObject(Layout::Inline);
        w.key("name");
        w.value(std::string_view(options.appName.empty() ? identity::name : options.appName.c_str()));
        w.key("version");
        w.value(std::string_view(options.appVersion.empty() ? identity::version : options.appVersion.c_str()));
        w.endObject();
        w.key("created");
        w.value(std::string_view(created.empty() ? now : created));
        w.key("modified");
        w.value(std::string_view(now));
        enc.object("samplePool", pool, d.sampleRefs, Layout::Lines);
        enc.popFrame();
        w.endObject();
        text += '\n';
        files.push_back({"meta.json", std::move(text)});
    }
    {
        std::string text;
        text.reserve(64 * 1024);
        JsonWriter w(text);
        Encoder enc(w, options.writeDefaults);
        w.beginObject(Layout::Lines);
        enc.pushFrame();
        writeFormat(w, enc);
        detail::schema(enc, p, d);
        enc.popFrame();
        w.endObject();
        text += '\n';
        files.push_back({"project.json", std::move(text)});
    }
    const auto& dp = detail::defaultPatternValue();
    for (int s = 0; s < model::kScenes; ++s)
    {
        std::string text;
        JsonWriter w(text);
        Encoder enc(w, options.writeDefaults);
        w.beginObject(Layout::Lines);
        w.key("scene");
        w.value(s + 1);
        w.key("patterns");
        w.beginArray(Layout::Lines);
        const auto& scene = p.scenes[static_cast<std::size_t>(s)];
        for (int i = 0; i < model::kPatterns; ++i)
        {
            const auto& pattern = scene.patterns[static_cast<std::size_t>(i)];
            if (! options.writeDefaults && pattern == dp)
                continue;
            w.beginObject(Layout::Lines);
            enc.pushFrame();
            enc.known("index");
            w.key("index");
            w.value(i + 1);
            detail::schema(enc, pattern, dp);
            enc.popFrame();
            w.endObject();
        }
        w.endArray();
        w.endObject();
        text += '\n';
        files.push_back({sceneFileName(s).toStdString(), std::move(text)});
    }
    return files;
}

Result<juce::var> parseFile(const juce::File& f, bool required)
{
    if (! f.existsAsFile())
    {
        if (required)
            return Result<juce::var>::failure("The project is incomplete: " + f.getFullPathName().toStdString() +
                                              " is missing");
        return juce::var();
    }
    juce::MemoryBlock data;
    if (! f.loadFileAsData(data))
        return Result<juce::var>::failure("Cannot read " + f.getFullPathName().toStdString());
    juce::var parsed;
    const auto r = juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(data.getData()),
                                                            static_cast<int>(data.getSize())),
                                     parsed);
    if (r.failed() || ! parsed.isObject())
        return Result<juce::var>::failure("The project file " + f.getFullPathName().toStdString() + " is damaged",
                                          r.getErrorMessage().toStdString());
    return parsed;
}

bool isGeneratedFile(const juce::String& rel)
{
    if (rel == "meta.json" || rel == "project.json")
        return true;
    for (int s = 0; s < model::kScenes; ++s)
        if (rel == sceneFileName(s))
            return true;
    return false;
}

std::string uniqueSampleName(const juce::File& folder, const std::string& wanted)
{
    const auto base = safeFileName(wanted);
    const auto dot = base.rfind('.');
    const auto stem = dot == std::string::npos ? base : base.substr(0, dot);
    const auto ext = dot == std::string::npos ? std::string() : base.substr(dot);
    auto name = base;
    for (int n = 2; folder.getChildFile(juce::String::fromUTF8(name.c_str())).exists(); ++n) // FAT: case-insensitive
        name = safeFileName(stem.substr(0, 50) + "-" + std::to_string(n) + ext);
    return name;
}
} // namespace

CodecInfo NativeCodec::info() const
{
    static constexpr std::string_view extensions[] = {kNativeExtension};
    return {"native", "Native project", extensions, true};
}

juce::File nativeBundleOf(const juce::File& f)
{
    if (f.existsAsFile() && (f.getFileName() == "meta.json" || f.getFileName() == "project.json"))
        return f.getParentDirectory();
    return f;
}

bool NativeCodec::canRead(const juce::File& file) const
{
    const auto b = nativeBundleOf(file);
    return b.isDirectory() && (b.getChildFile("meta.json").existsAsFile() || b.getChildFile("project.json").existsAsFile());
}

Result<juce::var> NativeCodec::load(const juce::File& file)
{
    const auto bundle = nativeBundleOf(file);
    auto project = parseFile(bundle.getChildFile("project.json"), true);
    if (! project)
        return project.error();
    auto meta = parseFile(bundle.getChildFile("meta.json"), false);
    if (! meta)
        return meta.error();
    auto* root = new juce::DynamicObject();
    juce::var rootVar(root);
    // A bundle without meta.json (hand-made, or meta lost) takes its format from project.json.
    root->setProperty("meta", meta.value().isObject() ? meta.value() : juce::var(new juce::DynamicObject()));
    if (! meta.value().isObject())
        root->getProperty("meta").getDynamicObject()->setProperty("format", project.value()["format"]);
    root->setProperty("project", project.value());
    juce::Array<juce::var> patterns;
    for (int s = 0; s < model::kScenes; ++s)
    {
        auto scene = parseFile(bundle.getChildFile(sceneFileName(s)), false);
        if (! scene)
            return scene.error();
        patterns.add(scene.value());
    }
    root->setProperty("patterns", patterns);
    return rootVar;
}

void NativeCodec::decode(const juce::var& root, model::Project& out, ImportReport& report)
{
    const auto& d = detail::defaultProject();
    out = d;
    detail::DecodeContext ctx {report};
    detail::Decoder dec(ctx);

    if (dec.pushFrame(root["project"], "project.json"))
    {
        dec.get("format");
        detail::schema(dec, out, d);
        dec.popFrame();
    }
    if (dec.pushFrame(root["meta"], "meta.json"))
    {
        for (const char* key : {"format", "app", "created", "modified"})
            dec.get(key);
        dec.object("samplePool", out.sampleRefs, d.sampleRefs);
        dec.popFrame();
    }
    const auto& dp = detail::defaultPatternValue();
    const auto* files = root["patterns"].getArray();
    for (int s = 0; files != nullptr && s < std::min(model::kScenes, files->size()); ++s)
    {
        const auto& file = files->getReference(s);
        if (file.isVoid() || ! dec.pushFrame(file, sceneFileName(s).toStdString()))
            continue;
        if (const auto* scene = dec.get("scene"); scene != nullptr && static_cast<int>(*scene) != s + 1)
            ctx.warn(sceneFileName(s).toStdString() + ": names scene " + scene->toString().toStdString());
        if (const auto* list = dec.get("patterns"); list != nullptr && list->isArray())
            for (int i = 0; i < list->getArray()->size(); ++i)
            {
                const auto& entry = list->getArray()->getReference(i);
                if (! dec.pushFrame(entry, "patterns[" + std::to_string(i) + "]"))
                    continue;
                const auto* index = dec.get("index");
                const int idx = index != nullptr && (index->isInt() || index->isInt64()) ? static_cast<int>(*index) : 0;
                if (idx < 1 || idx > model::kPatterns)
                    ctx.warn(sceneFileName(s).toStdString() + ": pattern entry " + std::to_string(i + 1) +
                             " has no index 1-16 (skipped)");
                else
                    detail::schema(dec, out.scenes[static_cast<std::size_t>(s)].patterns[static_cast<std::size_t>(idx - 1)],
                                   dp);
                dec.popFrame();
            }
        dec.popFrame();
    }
}

Result<void> NativeCodec::readInto(const juce::File& file, model::Project& out, ImportReport& report)
{
    auto loaded = load(file);
    if (! loaded)
        return loaded.error();
    auto& root = loaded.value();
    const auto& format = root["meta"]["format"];
    if (format["name"].toString().toStdString() != kNativeFormatName)
        return Result<void>::failure("This is not a project in the native format: " +
                                         nativeBundleOf(file).getFullPathName().toStdString(),
                                     "format.name = '" + format["name"].toString().toStdString() + "'");
    const int version = schemaVersionOf(root);
    const auto& runner = MigrationRunner::builtin();
    if (version > runner.currentVersion())
    {
        report.readOnly = true;
        report.warnings.push_back("This project was saved by a newer version (format " + std::to_string(version) +
                                  "): it opens read-only.");
    }
    else
    {
        std::vector<std::string> applied;
        auto migrated = runner.run(root, version, &applied);
        if (! migrated)
            return migrated.error();
        for (const auto& summary : applied)
            report.warnings.push_back("converted from an older format: " + summary);
    }
    decode(root, out, report);
    std::vector<std::string> problems;
    if (! model::validate(out, problems, 50))
        for (auto& p : problems)
            report.warnings.push_back("invalid value: " + p);
    return {};
}

Result<std::unique_ptr<model::Project>> NativeCodec::read(const juce::File& file, ImportReport& report) const
{
    auto p = std::make_unique<model::Project>();
    if (auto r = readInto(file, *p, report); ! r)
        return r.error();
    return p;
}

std::vector<NativeCodec::EncodedFile> NativeCodec::encode(const model::Project& p, const SaveOptions& options)
{
    return encodeFiles(p, options, options.timestamp, p.sampleRefs);
}

Result<void> NativeCodec::save(const model::Project& p, const juce::File& bundle, const SaveOptions& options,
                               SaveReport* report)
{
    SaveReport local;
    auto& rep = report != nullptr ? *report : local;
    rep.pool = p.sampleRefs;
    std::string created;
    if (const auto meta = bundle.getChildFile("meta.json"); meta.existsAsFile())
        if (auto parsed = parseFile(meta, false); parsed && parsed.value()["created"].isString())
            created = parsed.value()["created"].toString().toStdString();

    AtomicWriter writer(bundle);
    if (auto r = writer.carryOver(bundle, isGeneratedFile); ! r)
        return r;

    if (options.collectSamples) // FMT-012: copies, never moves (PRJ-014)
    {
        const auto samples = writer.stagingFolder().getChildFile("samples");
        for (auto& entry : rep.pool.entries)
        {
            const auto loc = splitLocation(entry.path);
            if (loc.kind == LocationKind::Bundle)
                continue;
            const auto source = resolveLocation(loc, options.roots);
            if (! source || ! source->existsAsFile())
            {
                rep.warnings.push_back("Collect & Save: " + entry.path + " not found (reference kept)");
                continue;
            }
            samples.createDirectory();
            const auto name = uniqueSampleName(samples, source->getFileName().toStdString());
            if (! source->copyFileTo(samples.getChildFile(juce::String::fromUTF8(name.c_str()))))
                return Result<void>::failure("Could not save: cannot copy " + source->getFullPathName().toStdString() +
                                             " into the project");
            entry.path = "$Bundle/" + name;
        }
    }

    for (const auto& f : encodeFiles(p, options, created, rep.pool))
    {
        if (auto r = writer.writeFile(juce::String::fromUTF8(f.name.c_str()), f.text.data(), f.text.size()); ! r)
            return r;
        rep.bytesWritten += f.text.size();
    }
    return writer.commit();
}

Result<void> NativeCodec::write(const model::Project& p, const juce::File& bundle) const { return save(p, bundle); }
} // namespace services::store

APP_REGISTER_CODEC(::services::store::NativeCodec)
