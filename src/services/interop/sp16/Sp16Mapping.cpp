#include "services/interop/sp16/Sp16Mapping.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace services::interop::sp16
{
namespace
{
struct Builtin
{
    const char* param;
    ValueClass cls;
    int high;
    double scale = 1.0;
    const char* basis = "guess";
};
// Classes: research "Value classes". Parameters: `observed` = inferred from the value ranges a real project holds
// (e.g. cent steps for ordinal 0, k/127 values for 5–7 as the envelope's own *Norm fields, a
// signed small value for 14, a 0–127 value for 15, the default retrigger count / speed index for 11 / 12); `guess` =
// this app's own ParamIndex order where nothing was seen. Ordinal 10 holds a small constant whose meaning is unknown:
// not imported (its locks are reported and kept raw).
constexpr Builtin kBuiltin[23] = {
    {"playback.pitch", ValueClass::Raw, -1, 0.01, "observed"}, // cents → semitones
    {"playback.start", ValueClass::Frames, 16, 1.0, "research"},
    {"playback.loopStart", ValueClass::Frames, 17, 1.0, "research"},
    {"playback.length", ValueClass::Frames, 18, 1.0, "research"},
    {"env.velocity", ValueClass::Raw, -1},
    {"env.attack", ValueClass::Norm, -1, 1.0, "observed"},
    {"env.hold", ValueClass::Norm, -1, 1.0, "observed"},
    {"env.release", ValueClass::Norm, -1, 1.0, "observed"},
    {"playback.loop", ValueClass::NormOrIndex, -1},
    {"track.triggerMode", ValueClass::Raw, -1},
    {"", ValueClass::Raw, -1, 1.0, "observed"},
    {"seq.retrigCount", ValueClass::Raw, -1, 1.0, "observed"},
    {"seq.retrigSpeed", ValueClass::Raw, -1, 1.0, "observed"},
    {"track.sendAmount", ValueClass::Raw, -1, 1.0, "observed"},
    {"track.pan", ValueClass::Raw, -1, 1.0, "observed"},
    {"track.volume", ValueClass::Raw, -1, 1.0, "observed"},
    {"seq.offset", ValueClass::Raw, -1},
    {"track.mute", ValueClass::Raw, -1},
    {"lfo.speed", ValueClass::Raw, -1},
    {"lfo.depth", ValueClass::Raw, -1},
    {"lfo.phase", ValueClass::Raw, -1},
    {"playback.reverse", ValueClass::Raw, -1},
    {"slice.number", ValueClass::Raw, -1},
};

std::mutex& userMutex()
{
    static std::mutex m;
    return m;
}
juce::String& userFile()
{
    static juce::String f;
    return f;
}

std::optional<ValueClass> classOf(const juce::String& s)
{
    for (auto c : {ValueClass::Raw, ValueClass::Norm, ValueClass::Frames, ValueClass::NormOrIndex})
        if (s == juce::String(className(c).data()))
            return c;
    return std::nullopt;
}

std::string jq(std::string_view s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}
} // namespace

std::string_view className(ValueClass c) noexcept
{
    switch (c)
    {
        case ValueClass::Raw: return "raw";
        case ValueClass::Norm: return "norm";
        case ValueClass::Frames: return "frames";
        case ValueClass::NormOrIndex: return "normOrIndex";
    }
    return "raw";
}

const ParamIndexMap& ParamIndexMap::builtin()
{
    static const ParamIndexMap map = []
    {
        ParamIndexMap m;
        for (int k = 0; k < 23; ++k)
            m.ordinals[static_cast<std::size_t>(k)] = {k, kBuiltin[k].param, kBuiltin[k].cls, kBuiltin[k].high,
                                                        kBuiltin[k].scale, true, kBuiltin[k].basis};
        m.pathAliases["%Smp"] = {"Samples"};                           // observed (every sample of a real project)
        m.pathAliases["%Tmp"] = {"Samples", "Samples/Tmp", "Tmp", ""}; // guess: the alias's folder is not documented
        m.volumes["$Int0"] = "";                                       // the chosen disk root (research)
        m.volumes["$Ext0"] = "";                                       // observed: samples found below the same root
        return m;
    }();
    return map;
}

std::string ParamIndexMap::basisSummary() const
{
    std::map<std::string, int> n;
    for (const auto& o : ordinals)
        ++n[o.param.empty() ? std::string("not imported") : o.basis];
    std::string out;
    for (const auto& [basis, count] : n)
        out += (out.empty() ? "" : ", ") + std::to_string(count) + " " + basis;
    return out;
}

int ParamIndexMap::guesses() const noexcept
{
    int n = 0;
    for (const auto& o : ordinals)
        n += o.guess ? 1 : 0;
    return n;
}

std::string ParamIndexMap::toJson() const
{
    std::string out = "{\n  \"version\": " + std::to_string(version) + ",\n";
    out += "  \"note\": \"Reference step-lock ordinal -> lockable parameter id. Classes are the documented value "
           "classes; the parameter names are guesses (OQ-01). Edit a copy and place it where the app reads it.\",\n";
    out += "  \"ordinals\": [\n";
    for (std::size_t k = 0; k < ordinals.size(); ++k)
    {
        const auto& o = ordinals[k];
        out += "    {\"ordinal\": " + std::to_string(o.ordinal) + ", \"param\": " + jq(o.param) +
               ", \"class\": " + jq(className(o.cls));
        if (o.highSlot >= 0)
            out += ", \"highSlot\": " + std::to_string(o.highSlot);
        if (o.scale != 1.0)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", o.scale);
            out += ", \"scale\": " + std::string(buf);
        }
        out += std::string(", \"guess\": ") + (o.guess ? "true" : "false") + ", \"basis\": " + jq(o.basis) + "}";
        out += k + 1 < ordinals.size() ? ",\n" : "\n";
    }
    out += "  ],\n  \"pathAliases\": {";
    bool first = true;
    for (const auto& [alias, dirs] : pathAliases)
    {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    " + jq(alias) + ": [";
        for (std::size_t k = 0; k < dirs.size(); ++k)
            out += (k ? ", " : "") + jq(dirs[k]);
        out += "]";
    }
    out += "\n  },\n  \"volumes\": {";
    first = true;
    for (const auto& [volume, dir] : volumes)
    {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    " + jq(volume) + ": " + jq(dir);
    }
    out += "\n  }\n}\n";
    return out;
}

Result<ParamIndexMap> ParamIndexMap::parse(std::string_view json)
{
    using R = Result<ParamIndexMap>;
    juce::var root;
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())), root);
    if (parsed.failed() || ! root.isObject())
        return R::failure("not a JSON object: " + parsed.getErrorMessage().toStdString());
    ParamIndexMap m = builtin();
    m.version = static_cast<int>(root.getProperty("version", 1));
    const auto* list = root.getProperty("ordinals", juce::var()).getArray();
    if (list == nullptr)
        return R::failure("\"ordinals\" is not an array");
    std::array<bool, 23> seen {};
    for (const auto& e : *list)
    {
        if (! e.isObject() || ! e.hasProperty("ordinal"))
            return R::failure("an \"ordinals\" entry has no \"ordinal\"");
        const int k = static_cast<int>(e.getProperty("ordinal", -1));
        if (k < 0 || k > 22)
            return R::failure("ordinal " + std::to_string(k) + " is not 0-22");
        if (seen[static_cast<std::size_t>(k)])
            return R::failure("ordinal " + std::to_string(k) + " is listed twice");
        seen[static_cast<std::size_t>(k)] = true;
        auto& o = m.ordinals[static_cast<std::size_t>(k)];
        o.ordinal = k;
        o.param = e.getProperty("param", "").toString().toStdString();
        const auto cls = classOf(e.getProperty("class", "raw").toString());
        if (! cls)
            return R::failure("ordinal " + std::to_string(k) + ": unknown class");
        o.cls = *cls;
        o.highSlot = static_cast<int>(e.getProperty("highSlot", -1));
        if (o.cls == ValueClass::Frames && (o.highSlot < 0 || o.highSlot > 22 || o.highSlot == k))
            return R::failure("ordinal " + std::to_string(k) + ": class frames needs a highSlot 0-22");
        o.scale = static_cast<double>(e.getProperty("scale", 1.0));
        if (! std::isfinite(o.scale) || o.scale == 0.0)
            return R::failure("ordinal " + std::to_string(k) + ": scale must be a non-zero number");
        o.guess = static_cast<bool>(e.getProperty("guess", true));
        o.basis = e.getProperty("basis", "guess").toString().toStdString();
    }
    if (const auto* aliases = root.getProperty("pathAliases", juce::var()).getDynamicObject())
    {
        m.pathAliases.clear();
        for (const auto& p : aliases->getProperties())
        {
            std::vector<std::string> dirs;
            if (const auto* a = p.value.getArray())
                for (const auto& d : *a)
                    dirs.push_back(d.toString().toStdString());
            m.pathAliases[p.name.toString().toStdString()] = std::move(dirs);
        }
    }
    if (const auto* volumes = root.getProperty("volumes", juce::var()).getDynamicObject())
    {
        m.volumes.clear();
        for (const auto& p : volumes->getProperties())
            m.volumes[p.name.toString().toStdString()] = p.value.toString().toStdString();
    }
    return m; // "stepValue2" (older files) is ignored: the record's second field is its step number
}

std::uint8_t envIndexFromReference(core::ampenv::EnvStage stage, double norm) noexcept
{
    if (! (norm > 0.0))
        return 0;
    if (norm >= 1.0)
        return core::ampenv::hasInfinity(stage) ? core::ampenv::kInfinityIndex : core::ampenv::kMaxFiniteIndex;
    const double k = std::floor(norm * 127.0 + 0.5);
    return static_cast<std::uint8_t>(std::clamp(k, 0.0, static_cast<double>(core::ampenv::kMaxFiniteIndex)));
}

double envReferenceFromIndex(core::ampenv::EnvStage stage, std::uint8_t index, bool* lossy) noexcept
{
    if (lossy != nullptr)
        *lossy = false;
    const bool inf = core::ampenv::hasInfinity(stage);
    if (inf && index >= core::ampenv::kInfinityIndex)
        return 1.0;
    int k = std::min<int>(index, core::ampenv::kMaxFiniteIndex);
    if (inf && k == core::ampenv::kMaxFiniteIndex)
    {
        k = core::ampenv::kMaxFiniteIndex - 1; // 127/127 = 1.0 is INFINITY there
        if (lossy != nullptr)
            *lossy = true;
    }
    // The reference's fields are float32 widened to double: k/127 rounded to float first (byte-identical re-export).
    return static_cast<double>(static_cast<float>(k) / 127.0f);
}

namespace
{
// FxParamIndex per insert effect and sub-slot 0–5 (−1 = none); research docs/effects/parameters.md "FxParamIndex".
struct FxRow
{
    core::model::EffectType type;
    std::array<int, 6> idx;
};
constexpr std::array<int, 6> kGenericFx {11, 12, 13, -1, -1, 16};
constexpr FxRow kFxRows[] = {
    {core::model::EffectType::Chorus, {0, 1, 2, 3, -1, 4}}, // observed: sub-slot 5 is stored as 4
    {core::model::EffectType::Flanger, {5, 6, 7, 8, 9, 10}},
    {core::model::EffectType::LoFi, {23, 24, 25, 26, 27, 28}},
    {core::model::EffectType::ParametricEq2, {29, 30, 31, 32, 33, 34}},
    {core::model::EffectType::Distortion, {35, 36, 37, 38, 39, 40}},
    {core::model::EffectType::MultiModeFilter, {41, 42, 43, 44, -1, 45}},
    {core::model::EffectType::InsertDelay, kGenericFx}, // observed: sub-slot 5 is stored as 16 (research row 46–49)
    {core::model::EffectType::Compressor, {50, 51, 52, 53, 54, 55}},
    {core::model::EffectType::Phaser, {61, 62, 63, 64, 65, 66}},
    {core::model::EffectType::Ducker, {69, 70, 67, 68, -1, 71}},
};
} // namespace

int fxLockIndex(core::model::EffectType type, int sub) noexcept
{
    if (sub < 0 || sub > 5)
        return -1;
    for (const auto& r : kFxRows)
        if (r.type == type)
            return r.idx[static_cast<std::size_t>(sub)];
    return kGenericFx[static_cast<std::size_t>(sub)];
}

bool knownFxLockIndex(int idx, int sub) noexcept
{
    if (sub < 0 || sub > 5 || idx < 0)
        return false;
    if (kGenericFx[static_cast<std::size_t>(sub)] == idx)
        return true;
    for (const auto& r : kFxRows)
        if (r.idx[static_cast<std::size_t>(sub)] == idx)
            return true;
    return idx == 46 + (sub == 5 ? 3 : sub) && sub != 3 && sub != 4; // the research's delay row (46–49)
}

void setUserMapFile(const juce::File& f)
{
    std::lock_guard<std::mutex> lock(userMutex());
    userFile() = f.getFullPathName();
}

ParamIndexMap activeParamIndexMap(std::string& note)
{
    juce::String path;
    {
        std::lock_guard<std::mutex> lock(userMutex());
        path = userFile();
    }
    const juce::File f = path.isNotEmpty() && juce::File::isAbsolutePath(path) ? juce::File(path) : juce::File();
    if (f == juce::File() || ! f.existsAsFile())
    {
        note = "param-index map: built-in (" + ParamIndexMap::builtin().basisSummary() + ")";
        return ParamIndexMap::builtin();
    }
    if (f.getSize() > (1 << 20))
    {
        note = "param-index map: " + f.getFullPathName().toStdString() + " is too large, built-in used";
        return ParamIndexMap::builtin();
    }
    auto r = ParamIndexMap::parse(f.loadFileAsString().toStdString());
    if (! r)
    {
        note = "param-index map: " + f.getFullPathName().toStdString() + " ignored (" + r.error().message +
               "), built-in used";
        return ParamIndexMap::builtin();
    }
    auto m = std::move(r.value());
    m.source = f.getFullPathName().toStdString();
    note = "param-index map: " + m.source + " (" + std::to_string(m.guesses()) + " of 23 flagged guess)";
    return m;
}

std::string_view ImportLog::kindName(Kind k) noexcept
{
    switch (k)
    {
        case Kind::EnumUnmapped: return "enum-unmapped";
        case Kind::OrdinalUnmapped: return "ordinal-unmapped";
        case Kind::Opaque: return "opaque-kept";
        case Kind::SampleMissing: return "sample-missing";
        case Kind::ModelLacks: return "model-lacks";
        case Kind::UnknownNode: return "unknown-node";
        case Kind::UnknownProp: return "unknown-prop";
        case Kind::TypeMismatch: return "type-mismatch";
        case Kind::Clamped: return "clamped";
        case Kind::Guessed: return "guessed";
        case Kind::Structure: return "structure";
    }
    return "?";
}

void ImportLog::issue(Kind kind, std::string key, std::string_view where)
{
    const auto k = std::make_pair(static_cast<int>(kind), key);
    if (auto it = index_.find(k); it != index_.end())
    {
        ++issues_[it->second].count;
        return;
    }
    index_.emplace(k, issues_.size());
    issues_.push_back({kind, std::move(key), std::string(where), 1});
}

std::size_t ImportLog::count(Kind kind) const noexcept
{
    std::size_t n = 0;
    for (const auto& i : issues_)
        n += i.kind == kind ? i.count : 0;
    return n;
}

std::size_t ImportLog::distinct(Kind kind) const noexcept
{
    std::size_t n = 0;
    for (const auto& i : issues_)
        n += i.kind == kind ? 1 : 0;
    return n;
}

bool ImportLog::has(Kind kind, std::string_view prefix) const noexcept
{
    for (const auto& i : issues_)
        if (i.kind == kind && std::string_view(i.key).substr(0, prefix.size()) == prefix)
            return true;
    return false;
}

std::vector<std::string> ImportLog::lines() const
{
    std::vector<std::string> out = lines_;
    for (const auto& i : issues_)
    {
        std::string s = std::string(kindName(i.kind)) + " " + i.key + " x" + std::to_string(i.count);
        if (! i.first.empty())
            s += " (first: " + i.first + ")";
        out.push_back(std::move(s));
    }
    return out;
}
} // namespace services::interop::sp16
