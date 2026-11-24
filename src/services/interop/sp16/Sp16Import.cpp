#include "services/interop/sp16/Sp16Import.h"

#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <type_traits>

namespace services::interop::sp16
{
namespace
{
namespace m = ::core::model;
using Kind = ImportLog::Kind;
using Names = std::initializer_list<std::string_view>;

// LfoData.destination is the reference's modulation id: 100–155 address an FX1 parameter, 200–255 the same table for
// FX2 (research effects/parameters.md "FxParamIndex", Insert manager slots 57/58, F135). Each id names an Insert effect
// and its internal index k (the catalogue's EffectParamDef::k); 154 is a gap. The LFO screen and the engine read the
// destination as its ParamSpec id in the Lfo's extras "native.destinationId" (a JSON string, ModelParamAccess).
struct ModTarget
{
    std::string_view effect; // the registry's effect id; empty = no parameter
    std::int8_t k;
};
constexpr ModTarget kModTargets[56] = {
    {"chorus", 0},     {"chorus", 1},     {"chorus", 2},     {"chorus", 3},     {"chorus", 5},     // 100–104
    {"flanger", 0},    {"flanger", 1},    {"flanger", 2},    {"flanger", 3},    {"flanger", 4},    // 105–109
    {"flanger", 5},    {"eq2", 2},        {"eq2", 0},        {"eq2", 1},        {"eq2", 5},        // 110–114
    {"eq2", 3},        {"eq2", 4},        {"filter", 0},     {"filter", 1},     {"filter", 2},     // 115–119
    {"filter", 3},     {"filter", 5},     {"delay", 0},      {"delay", 1},      {"delay", 2},      // 120–124
    {"delay", 5},      {"lofi", 0},       {"lofi", 1},       {"lofi", 2},       {"lofi", 3},       // 125–129
    {"lofi", 4},       {"lofi", 5},       {"compressor", 0}, {"compressor", 1}, {"compressor", 2}, // 130–134
    {"compressor", 3}, {"compressor", 4}, {"compressor", 5}, {"distortion", 0}, {"distortion", 1}, // 135–139
    {"distortion", 2}, {"distortion", 3}, {"distortion", 4}, {"distortion", 5}, {"phaser", 0},     // 140–144
    {"phaser", 1},     {"phaser", 2},     {"phaser", 3},     {"phaser", 4},     {"phaser", 5},     // 145–149
    {"ducker", 0},     {"ducker", 1},     {"ducker", 2},     {"ducker", 3},     {{}, 0},           // 150–154
    {"ducker", 5},                                                                                 // 155
};

// The ParamSpec id of a modulation id ("fx1.chorus.rate"), or nullopt (not an FX parameter the LFO can target).
std::optional<std::string> lfoDestinationId(int id)
{
    const int slot = id / 100, row = id % 100;
    if ((slot != 1 && slot != 2) || row > 55 || kModTargets[row].effect.empty())
        return std::nullopt;
    const auto& reg = ::core::builtinParams(); // registers the effects' parameter sets (core::fx::effectParams)
    const auto* set = ::core::fx::effectParams(kModTargets[row].effect);
    const auto* def = set != nullptr ? ::core::fx::paramByK(*set, kModTargets[row].k) : nullptr;
    if (def == nullptr)
        return std::nullopt;
    std::string pid = (slot == 1 ? "fx1." : "fx2.") + std::string(kModTargets[row].effect) + "." + std::string(def->key);
    const auto* spec = reg.find(pid);
    return spec != nullptr && spec->lfoTarget ? std::optional<std::string>(std::move(pid)) : std::nullopt;
}

std::string where(int scene, int track = -1)
{
    std::string s = "scene " + std::to_string(scene + 1);
    if (track >= 0)
        s += " track " + std::to_string(track + 1);
    return s;
}

// One node's properties: each is taken at most once; whatever is left at finish() is kept and reported.
// SET SCALE's scales (ScaleType): the reference's strings are not known, so a string names a scale when it equals one
// of the manual's names (core::model::ScaleType's stored or display name) with case, spaces, '_' and '-' ignored
// (`MAJOR_PENTATONIC`, `Raga Todi`).
bool looseEqual(std::string_view a, std::string_view b) noexcept
{
    auto skip = [](char c) { return c == ' ' || c == '_' || c == '-'; };
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    std::size_t i = 0, j = 0;
    for (;;)
    {
        while (i < a.size() && skip(a[i]))
            ++i;
        while (j < b.size() && skip(b[j]))
            ++j;
        if (i == a.size() || j == b.size())
            return i == a.size() && j == b.size();
        if (lower(a[i++]) != lower(b[j++]))
            return false;
    }
}
std::optional<m::ScaleType> scaleByName(std::string_view s) noexcept
{
    for (const auto& e : m::enumTable<m::ScaleType>().entries)
        if (looseEqual(s, e.stored) || looseEqual(s, e.display))
            return static_cast<m::ScaleType>(e.value);
    return std::nullopt;
}

class Props
{
public:
    Props(const Node& n, std::string_view base, m::Extras& extras, ImportLog& log, std::string at)
        : n_(n), base_(base), extras_(extras), log_(log), at_(std::move(at)), used_(n.props.size(), false)
    {
    }
    ~Props() { finish(); }
    Props(const Props&) = delete;
    Props& operator= (const Props&) = delete;

    struct Hit
    {
        const Var* v = nullptr;
        std::string_view name;
        explicit operator bool() const noexcept { return v != nullptr; }
    };

    // The first present name; `guessed` names ([G]) are reported when they match.
    Hit take(Names names, bool guessed = false)
    {
        for (const auto name : names)
            for (std::size_t k = 0; k < n_.props.size(); ++k)
                if (! used_[k] && n_.props[k].first == name)
                {
                    used_[k] = true;
                    if (guessed)
                        log_.issue(Kind::Guessed, key(name) + " (field name not documented)", at_);
                    return {&n_.props[k].second, n_.props[k].first};
                }
        return {};
    }
    Hit take(const std::string& name, bool guessed = false) { return take({std::string_view(name)}, guessed); }

    void keep(const Hit& h, Kind kind, std::string_view detail = {})
    {
        extras_.set("sp16." + key(h.name), varToJson(*h.v), m::Extras::Encoding::Json);
        log_.issue(kind, key(h.name) + (detail.empty() ? "" : " " + std::string(detail)), at_);
    }
    void keepText(const Hit& h, const std::string& text, Kind kind)
    {
        extras_.set("sp16." + key(h.name), text, m::Extras::Encoding::Text);
        log_.issue(kind, key(h.name) + "='" + text + "'", at_);
    }

    bool number(Names names, double& out, bool guessed = false)
    {
        auto h = take(names, guessed);
        if (! h || h.v->t == Var::T::Void)
            return false;
        if (h.v->isNumber())
        {
            out = h.v->number();
            if (std::isfinite(out))
                return true;
        }
        keep(h, Kind::TypeMismatch, "(" + std::string(typeName(h.v->t)) + ")");
        return false;
    }
    bool integer(Names names, std::int64_t& out, bool guessed = false)
    {
        auto h = take(names, guessed);
        if (! h || h.v->t == Var::T::Void)
            return false;
        if ((h.v->t == Var::T::Int || h.v->t == Var::T::Int64) ||
            (h.v->t == Var::T::Double && std::isfinite(h.v->d) && std::nearbyint(h.v->d) == h.v->d &&
             std::fabs(h.v->d) < 9e15))
        {
            out = h.v->t == Var::T::Double ? static_cast<std::int64_t>(h.v->d) : h.v->i;
            return true;
        }
        keep(h, Kind::TypeMismatch, "(" + std::string(typeName(h.v->t)) + ")");
        return false;
    }
    bool boolean(Names names, bool& out, bool guessed = false)
    {
        auto h = take(names, guessed);
        if (! h || h.v->t == Var::T::Void)
            return false;
        if (h.v->isBool())
        {
            out = h.v->t == Var::T::True;
            return true;
        }
        if (h.v->t == Var::T::Int && (h.v->i == 0 || h.v->i == 1))
        {
            out = h.v->i == 1;
            return true;
        }
        keep(h, Kind::TypeMismatch, "(" + std::string(typeName(h.v->t)) + ")");
        return false;
    }
    bool string(Names names, std::string& out, bool guessed = false)
    {
        auto h = take(names, guessed);
        if (! h || h.v->t == Var::T::Void)
            return false;
        if (h.v->t == Var::T::String && ! h.v->unterminated)
        {
            out = h.v->s;
            return true;
        }
        keep(h, Kind::TypeMismatch, "(" + std::string(typeName(h.v->t)) + ")");
        return false;
    }

    // An int field with the model's range: out-of-range values are clamped, the original kept.
    template <typename Int>
    void intIn(Names names, Int& field, std::int64_t lo, std::int64_t hi, bool guessed = false)
    {
        std::int64_t v = 0;
        auto h = peek(names);
        if (! integer(names, v, guessed))
            return;
        if (v < lo || v > hi)
        {
            keep(h, Kind::Clamped, std::to_string(v) + " not in " + std::to_string(lo) + ".." + std::to_string(hi));
            v = std::clamp(v, lo, hi);
        }
        field = static_cast<Int>(v);
    }
    // As intIn, with a "none" sentinel (observed −1 for chokeGroup, colourIndex, stepShiftArea, midimode): the model
    // keeps its default; the stored value is kept silently for the exporter.
    template <typename Int>
    void intOrNone(Names names, Int& field, std::int64_t lo, std::int64_t hi, std::int64_t none = -1)
    {
        if (auto h = peek(names); h && (h.v->t == Var::T::Int || h.v->t == Var::T::Int64) && h.v->i == none)
        {
            take(names);
            extras_.set("sp16." + key(h.name), varToJson(*h.v), m::Extras::Encoding::Json);
            return;
        }
        intIn(names, field, lo, hi);
    }
    // A packed int (hi16 << 16 | lo16, observed for retriggerSpeed and the quantise units): the ratio hi / lo, or
    // nullopt for a plain small int.
    static std::optional<std::pair<int, int>> packedRatio(const Var& v) noexcept
    {
        if ((v.t != Var::T::Int && v.t != Var::T::Int64) || v.i <= 0xFFFF || v.i > 0x7FFFFFFF)
            return std::nullopt;
        const int hi = static_cast<int>(v.i >> 16), lo = static_cast<int>(v.i & 0xFFFF);
        if (hi <= 0 || lo <= 0)
            return std::nullopt;
        return std::make_pair(hi, lo);
    }
    Hit peekHit(Names names) const { return peek(names); }
    void keepSilently(const Hit& h) { extras_.set("sp16." + key(h.name), varToJson(*h.v), m::Extras::Encoding::Json); }

    void doubleIn(Names names, double& field, double lo, double hi, bool guessed = false)
    {
        double v = 0;
        auto h = peek(names);
        if (! number(names, v, guessed))
            return;
        if (v < lo || v > hi)
        {
            keep(h, Kind::Clamped, "out of range");
            v = std::clamp(v, lo, hi);
        }
        field = v;
    }
    // True when one of `names` was present (whatever its value).
    bool flag(Names names, bool& field, bool guessed = false)
    {
        const bool present = static_cast<bool>(peek(names));
        bool b = false;
        if (boolean(names, b, guessed))
            field = b;
        return present;
    }

    // An enum stored as a string (exact stored name, alias, one of `prefixes` + stored name, or `extra`) or as an int
    // (the list index: reported as a guess). Unknown strings are kept as Text (UI-501).
    template <typename E>
    bool enumeration(Names names, E& field, Names prefixes = {},
                     std::initializer_list<std::pair<std::string_view, E>> extra = {}, bool guessed = false)
    {
        auto h = take(names, guessed);
        if (! h || h.v->t == Var::T::Void)
            return false;
        if (h.v->t == Var::T::String)
        {
            const std::string_view s = h.v->s;
            for (const auto& [name, value] : extra)
                if (s == name)
                {
                    field = value;
                    return true;
                }
            if (auto e = m::enumFromStored<E>(s))
            {
                field = *e;
                return true;
            }
            for (const auto p : prefixes)
                if (s.substr(0, p.size()) == p)
                    if (auto e = m::enumFromStored<E>(s.substr(p.size())))
                    {
                        field = *e;
                        return true;
                    }
            if constexpr (std::is_same_v<E, m::ScaleType>)
            {
                auto e = scaleByName(s);
                for (const auto p : prefixes)
                    if (! e && s.substr(0, p.size()) == p)
                        e = scaleByName(s.substr(p.size()));
                if (e)
                {
                    field = *e;
                    return true;
                }
            }
            keepText(h, h.v->s, Kind::EnumUnmapped);
            return false;
        }
        if (h.v->t == Var::T::Int)
        {
            if (auto e = m::enumFromInt<E>(static_cast<int>(h.v->i)); e && h.v->i >= 0 && h.v->i < 256)
            {
                field = *e;
                log_.issue(Kind::Guessed, key(h.name) + " (int read as the list index)", at_);
                return true;
            }
            keep(h, Kind::EnumUnmapped, "(int " + std::to_string(h.v->i) + ")");
            return false;
        }
        keep(h, Kind::TypeMismatch, "(" + std::string(typeName(h.v->t)) + ")");
        return false;
    }
    // An int model field whose values an enum names (data-model.md: these are ints in the reference too): an int is
    // taken as is (range-checked), a string by the enum's stored names.
    template <typename E>
    void enumInt(Names names, int& field, Names prefixes = {}, bool guessed = false)
    {
        if (auto h = peek(names); h && h.v->t == Var::T::Int)
        {
            take(names, guessed);
            if (h.v->i == -1) // the observed "none" sentinel: the model's default stays
            {
                keepSilently(h);
                return;
            }
            if (m::enumFromInt<E>(static_cast<int>(h.v->i)) && h.v->i >= 0 && h.v->i < 256)
                field = static_cast<int>(h.v->i);
            else
                keep(h, Kind::EnumUnmapped, "(int " + std::to_string(h.v->i) + " not in the list)");
            return;
        }
        E e = static_cast<E>(0);
        if (enumeration<E>(names, e, prefixes, {}, guessed))
            field = static_cast<int>(e);
    }

    void finish()
    {
        if (done_)
            return;
        done_ = true;
        for (std::size_t k = 0; k < n_.props.size(); ++k)
            if (! used_[k])
            {
                used_[k] = true;
                keep({&n_.props[k].second, n_.props[k].first}, Kind::UnknownProp);
            }
    }

    std::string key(std::string_view name) const { return std::string(base_) + "." + std::string(name); }
    const std::string& at() const noexcept { return at_; }

private:
    Hit peek(Names names) const
    {
        for (const auto name : names)
            for (std::size_t k = 0; k < n_.props.size(); ++k)
                if (! used_[k] && n_.props[k].first == name)
                    return {&n_.props[k].second, n_.props[k].first};
        return {};
    }

    const Node& n_;
    std::string_view base_;
    m::Extras& extras_;
    ImportLog& log_;
    std::string at_;
    std::vector<bool> used_;
    bool done_ = false;
};

// Children grouped by base type; index = numeric suffix, else the running position among siblings of that type.
struct Child
{
    const Node* node;
    std::string_view base;
    int index;
    bool suffixed;
};
std::vector<Child> children(const Node& n)
{
    std::vector<Child> out;
    std::map<std::string_view, int> running;
    for (const auto& c : n.children)
    {
        const auto [base, suffix] = splitSuffix(c.type);
        const int pos = running[base]++;
        out.push_back({&c, base, suffix ? *suffix : pos, suffix.has_value()});
    }
    return out;
}

void keepNode(const Node& c, m::Extras& extras, ImportLog& log, std::string_view at, Kind kind = Kind::UnknownNode)
{
    extras.set("sp16.node." + c.type, toBase64(encodeTree(c)), m::Extras::Encoding::Base64);
    log.issue(kind, c.type, at);
}

struct Ctx
{
    const ParamIndexMap& map;
    const MapOptions& opts;
    ImportLog& log;
    MapStats& stats;
    m::Project& project;
    std::map<std::string, std::string> urlToId;
    std::array<const core::ParamSpec*, 23> specs {}; // per ordinal (registry lookups are linear: done once)
    std::array<int, 23> slots {};
};

// ---------------------------------------------------------------------------------------------- lock values
const core::ParamSpec* specOf(const std::string& id)
{
    return id.empty() ? nullptr : core::builtinParams().find(id);
}
int appSlotOf(const core::ParamSpec* spec)
{
    return spec == nullptr ? -1 : core::commands::sequencer::lockSlotOf(*spec);
}
bool integral(const core::ParamSpec& s) noexcept
{
    return s.type == core::ParamType::Integer || s.type == core::ParamType::Enum || s.type == core::ParamType::Bool ||
           s.type == core::ParamType::Note;
}

// The envelope stages (ENV-001): their lock values are display indices 0–127 (+ INFINITY 128 for HOLD / RELEASE);
// a 0–127 reference value is that index directly, 127 being INFINITY where the stage has one (Sp16Mapping.h).
std::optional<core::ampenv::EnvStage> envStageOf(const core::ParamSpec& spec) noexcept
{
    if (spec.id == "env.attack")
        return core::ampenv::EnvStage::Attack;
    if (spec.id == "env.hold")
        return core::ampenv::EnvStage::Hold;
    if (spec.id == "env.release")
        return core::ampenv::EnvStage::Release;
    return std::nullopt;
}

// A lock word's value in the parameter's units, and back (the canonical writer).
double decodeValue(const OrdinalMap& o, const core::ParamSpec& spec, std::int32_t value22)
{
    const double lo = spec.minValue, hi = spec.maxValue;
    switch (o.cls)
    {
        case ValueClass::Raw: return value22 * o.scale;
        case ValueClass::NormOrIndex:
            if (value22 < 0 || value22 % kK127 != 0) // an index (12-step form), not a 0–127 value
                return value22;
            [[fallthrough]];
        case ValueClass::Norm:
            if (const auto stage = envStageOf(spec); stage && value22 >= 0)
                return envIndexFromReference(*stage, static_cast<double>(value22) / kValueFull);
            return lo + static_cast<double>(value22) / kValueFull * (hi - lo);
        case ValueClass::Frames: break;
    }
    return value22;
}
int encodeValue(const OrdinalMap& o, const core::ParamSpec& spec, float nativeF)
{
    const double native = nativeF, lo = spec.minValue, range = static_cast<double>(spec.maxValue) - lo;
    double v = 0;
    switch (o.cls)
    {
        case ValueClass::Raw: v = native / o.scale; break;
        case ValueClass::Norm:
        case ValueClass::NormOrIndex:
            if (const auto stage = envStageOf(spec))
            {
                const auto index = static_cast<std::uint8_t>(std::clamp(std::round(native), 0.0, 128.0));
                v = std::round(envReferenceFromIndex(*stage, index) * 127.0) * kK127;
                break;
            }
            v = range > 0 ? (native - lo) / range * kValueFull : 0.0;
            break;
        case ValueClass::Frames: v = native; break;
    }
    return static_cast<int>(std::clamp<double>(std::round(v), -(1 << 21), (1 << 21) - 1));
}

struct CellRaw
{
    std::optional<std::int32_t> trigger, value2, type;
    std::map<int, int> vct;
    std::map<int, std::uint32_t> words;
    bool empty() const noexcept { return ! trigger && ! value2 && ! type && vct.empty() && words.empty(); }
    std::string json() const
    {
        std::string s = "{";
        auto sep = [&] {
            if (s.size() > 1)
                s += ",";
        };
        if (trigger)
        {
            sep();
            s += "\"trigger\":" + std::to_string(*trigger);
        }
        if (value2)
        {
            sep();
            s += "\"value2\":" + std::to_string(*value2);
        }
        if (type)
        {
            sep();
            s += "\"type\":" + std::to_string(*type);
        }
        if (! vct.empty())
        {
            sep();
            s += "\"vct\":{";
            bool first = true;
            for (const auto& [k, v] : vct)
            {
                s += (first ? "\"" : ",\"") + std::to_string(k) + "\":" + std::to_string(v);
                first = false;
            }
            s += "}";
        }
        if (! words.empty())
        {
            sep();
            s += "\"words\":{";
            bool first = true;
            for (const auto& [k, v] : words)
            {
                s += (first ? "\"" : ",\"") + std::to_string(k) + "\":" + std::to_string(v);
                first = false;
            }
            s += "}";
        }
        return s + "}";
    }
    // The inverse of json() (a fixed, flat format: no general JSON parser needed on this hot path).
    static CellRaw parse(std::string_view text)
    {
        CellRaw r;
        auto number = [&](std::size_t& i, long long& v) {
            std::size_t j = i;
            bool neg = j < text.size() && text[j] == '-';
            if (neg)
                ++j;
            if (j >= text.size() || text[j] < '0' || text[j] > '9')
                return false;
            long long x = 0;
            while (j < text.size() && text[j] >= '0' && text[j] <= '9' && x < (1ll << 40))
                x = x * 10 + (text[j++] - '0');
            v = neg ? -x : x;
            i = j;
            return true;
        };
        auto scalar = [&](std::string_view key) -> std::optional<std::int32_t> {
            const auto at = text.find("\"" + std::string(key) + "\":");
            if (at == std::string_view::npos)
                return std::nullopt;
            std::size_t i = at + key.size() + 3;
            long long v = 0;
            if (! number(i, v))
                return std::nullopt;
            return static_cast<std::int32_t>(v);
        };
        auto object = [&](std::string_view key, auto&& put) {
            const auto at = text.find("\"" + std::string(key) + "\":{");
            if (at == std::string_view::npos)
                return;
            std::size_t i = at + key.size() + 4;
            while (i < text.size() && text[i] == '"')
            {
                long long k = 0, v = 0;
                ++i;
                if (! number(i, k) || i + 2 > text.size() || text[i] != '"' || text[i + 1] != ':')
                    return;
                i += 2;
                if (! number(i, v))
                    return;
                put(static_cast<int>(k), v);
                if (i < text.size() && text[i] == ',')
                    ++i;
            }
        };
        r.trigger = scalar("trigger");
        r.value2 = scalar("value2");
        r.type = scalar("type");
        object("vct", [&](int k, long long v) { r.vct[k] = static_cast<int>(v); });
        object("words", [&](int k, long long v) { r.words[k] = static_cast<std::uint32_t>(v); });
        return r;
    }
};

constexpr std::int16_t kImportVelocity = 127; // the reference step record holds no velocity: full

std::string cellKey(int s, int t)
{
    return "sp16.step[" + std::to_string(s) + "].track[" + std::to_string(t) + "]";
}

void mapSteps(const std::vector<StepRecord>& records, m::Pattern& pat, Ctx& c, const std::string& at,
              const FxLockTypes& fxTypes)
{
    pat.locks.clear();
    for (int s = 0; s < kStepCount; ++s)
        for (int t = 0; t < kTrackCount; ++t)
        {
            // Track-major (StepMatrix.h): the record of (s, t) is at t × 64 + s and names its own track and step.
            const auto& rec = records[recordIndex(s, t)];
            auto& cell = pat.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            CellRaw raw;
            if (const auto tt = m::triggerTypeFromReferenceCode(rec.typeFlags))
                cell.triggerType = *tt;
            else
            {
                cell.triggerType = m::TriggerType::Full;
                raw.type = rec.typeFlags;
                c.log.issue(Kind::EnumUnmapped, "step type code " + std::to_string(rec.typeFlags) + " (OQ-09)", at);
            }
            if (rec.programmed())
            {
                // The model's own form of a programmed cell (core/commands/sequencer setTrigger): the trigger code of
                // its type and a velocity — the record has none, so full.
                cell.trigger = static_cast<std::int16_t>(m::referenceTriggerCode(cell.triggerType));
                cell.value2 = kImportVelocity;
                ++c.stats.triggers;
                if (! rec.namesPosition(s, t))
                {
                    raw.trigger = rec.trigger; // kept for the exporter (written back while the cell stays programmed)
                    raw.value2 = rec.value2;
                    ++c.stats.coordinateMismatches;
                    c.log.issue(Kind::Structure, "step record names another track / step (fields kept)", at);
                }
            }
            else
            {
                cell.trigger = m::Step::kEmpty;
                cell.value2 = m::Step::kNoValue2;
                if (rec.value2 != kEmptyValue2)
                {
                    raw.value2 = rec.value2;
                    c.log.issue(Kind::Opaque, "empty step record with a second field (kept)", at);
                }
            }

            m::StepLocks locks;
            std::array<bool, kLockWords> consumed {};
            std::array<bool, m::kLockSlots> taken {};
            // Empty words with an unexpected index byte, and every vct bit, are kept for the round trip.
            for (int k = 0; k < kLockWords; ++k)
            {
                const auto& w = rec.slots[static_cast<std::size_t>(k)];
                if (w.empty && w.idx != noneIndex(k))
                    raw.words[k] = w.pack();
                if (w.vct != 0)
                    raw.vct[k] = w.vct;
            }
            if (! raw.vct.empty())
                c.log.issue(Kind::Opaque, "lock vct bits (imported as Revert, raw kept)", at);
            auto put = [&](int appSlot, float value, int word) {
                if (appSlot < 0 || appSlot >= m::kLockSlots || taken[static_cast<std::size_t>(appSlot)])
                {
                    raw.words[word] = rec.slots[static_cast<std::size_t>(word)].pack();
                    c.log.issue(Kind::Structure, "two lock words map to app slot " + std::to_string(appSlot), at);
                    return;
                }
                taken[static_cast<std::size_t>(appSlot)] = true;
                locks.mask |= std::uint64_t {1} << appSlot;
                locks.values[static_cast<std::size_t>(appSlot)] = value;
                locks.change[static_cast<std::size_t>(appSlot)] = m::LockChange::Revert;
            };
            // Frames first: they consume their high-part slot.
            for (const auto& o : c.map.ordinals)
            {
                const auto& w = rec.slots[static_cast<std::size_t>(o.ordinal)];
                if (o.cls != ValueClass::Frames || w.empty || consumed[static_cast<std::size_t>(o.ordinal)])
                    continue;
                consumed[static_cast<std::size_t>(o.ordinal)] = true;
                std::int64_t frames = w.value22;
                if (o.highSlot >= 0 && o.highSlot < 23)
                {
                    const auto& hw = rec.slots[static_cast<std::size_t>(o.highSlot)];
                    consumed[static_cast<std::size_t>(o.highSlot)] = true;
                    if (! hw.empty)
                        frames += static_cast<std::int64_t>(hw.value22) * kFrameBase;
                }
                const int slot = c.slots[static_cast<std::size_t>(o.ordinal)];
                if (w.idx != o.ordinal)
                    raw.words[o.ordinal] = w.pack();
                if (slot < 0 || frames < 0)
                {
                    raw.words[o.ordinal] = w.pack();
                    if (o.highSlot >= 0)
                        raw.words[o.highSlot] = rec.slots[static_cast<std::size_t>(o.highSlot)].pack();
                    c.log.issue(Kind::OrdinalUnmapped, "ordinal " + std::to_string(o.ordinal), at);
                    continue;
                }
                put(slot, static_cast<float>(frames), o.ordinal);
            }
            for (const auto& o : c.map.ordinals)
            {
                const int k = o.ordinal;
                const auto& w = rec.slots[static_cast<std::size_t>(k)];
                if (w.empty || consumed[static_cast<std::size_t>(k)])
                    continue;
                const auto* spec = c.specs[static_cast<std::size_t>(o.ordinal)];
                const int slot = c.slots[static_cast<std::size_t>(o.ordinal)];
                if (slot < 0 || o.cls == ValueClass::Frames)
                {
                    raw.words[k] = w.pack();
                    c.log.issue(Kind::OrdinalUnmapped, "ordinal " + std::to_string(k), at);
                    continue;
                }
                if (w.idx != k)
                    raw.words[k] = w.pack();
                double native = decodeValue(o, *spec, w.value22);
                if (native < static_cast<double>(spec->minValue) || native > static_cast<double>(spec->maxValue))
                {
                    c.log.issue(Kind::Clamped, "lock ordinal " + std::to_string(k) + " (" + o.param + ")", at);
                    native = static_cast<double>(spec->clamp(static_cast<float>(native)));
                }
                if (integral(*spec))
                    native = std::round(native);
                if (encodeValue(o, *spec, static_cast<float>(native)) != w.value22)
                    raw.words[k] = w.pack(); // not reproducible from the model value: keep the word
                put(slot, static_cast<float>(native), k);
            }
            // FX1 / FX2 internal parameters 0–5: 0–1 fractions. The index byte is the FxParamIndex (Sp16Mapping.h): the
            // word is kept when it is not the one the exporter writes for the track's effect.
            for (int k = 23; k < kLockWords; ++k)
            {
                const auto& w = rec.slots[static_cast<std::size_t>(k)];
                if (w.empty)
                    continue;
                double v = static_cast<double>(w.value22) / kValueFull;
                if (v < 0.0 || v > 1.0)
                {
                    c.log.issue(Kind::Clamped, "FX lock value outside 0-1", at);
                    v = std::clamp(v, 0.0, 1.0);
                    raw.words[k] = w.pack();
                }
                const int unit = (k - 23) / 6, sub = (k - 23) % 6;
                const int expected =
                    fxLockIndex(fxTypes[static_cast<std::size_t>(t)][static_cast<std::size_t>(unit)], sub);
                if (w.idx != expected)
                {
                    raw.words[k] = w.pack();
                    if (! knownFxLockIndex(w.idx, sub))
                        c.log.issue(Kind::Opaque, "FX lock index byte " + std::to_string(w.idx) + " (raw kept)", at);
                }
                put(k, static_cast<float>(v), k);
            }
            if (locks.mask != 0)
            {
                cell.lockIndex = static_cast<std::uint16_t>(pat.locks.size());
                pat.locks.push_back(locks);
                ++c.stats.lockCells;
                c.stats.locks += static_cast<int>(std::popcount(locks.mask));
            }
            else
                cell.lockIndex = m::Step::kNoLocks;
            if (! raw.empty()) // keys are unique per cell: no search
                pat.extras.entries.push_back({cellKey(s, t), m::Extras::Encoding::Json, raw.json()});
        }
}

// The first difference between two record lists ("step 3 track 2 slot 7"), "" when equal.
std::string firstDifference(const std::vector<StepRecord>& a, const std::vector<StepRecord>& b)
{
    for (std::size_t k = 0; k < std::min(a.size(), b.size()); ++k)
    {
        if (a[k] == b[k])
            continue;
        std::string at = "step " + std::to_string(stepOfRecord(k) + 1) + " track " + std::to_string(trackOfRecord(k) + 1);
        if (a[k].trigger != b[k].trigger || a[k].value2 != b[k].value2 || a[k].typeFlags != b[k].typeFlags)
            return at + " header";
        for (int s = 0; s < kLockWords; ++s)
            if (a[k].slots[static_cast<std::size_t>(s)] != b[k].slots[static_cast<std::size_t>(s)])
                return at + " slot " + std::to_string(s);
        return at;
    }
    return a.size() == b.size() ? std::string() : "size";
}

// -------------------------------------------------------------------------------------------- node mappers
// Reference effect option strings (InsertFx/SendFx/MasterFx bType1 / bType2, observed families "FilterType<X>",
// "FilterOrder<N>", "FilterPosition<Pre|Post>", "SourceTrack<N>") → the effect's type1 / type2 index in its catalogue
// option order (core/params/catalogue/fx_*.cpp). nullopt: the string means nothing for this effect — the
// reference keeps the options of a type the slot held before (carry-over), so they are kept, not mapped.
std::optional<int> optionIndex(m::EffectType t, int which, std::string_view s)
{
    using E = m::EffectType;
    auto after = [&](std::string_view prefix) -> std::optional<std::string_view> {
        if (s.substr(0, prefix.size()) != prefix)
            return std::nullopt;
        return s.substr(prefix.size());
    };
    auto pick = [](std::string_view v, std::initializer_list<std::pair<std::string_view, int>> table) -> std::optional<int> {
        for (const auto& [name, index] : table)
            if (v == name)
                return index;
        return std::nullopt;
    };
    switch (t)
    {
        case E::MultiModeFilter: // type1 LPF, HPF, BPF, OFF; type2 12 / 24 dB/Oct (order 2 = 12 dB, 4 = 24 dB)
            if (which == 1)
                if (auto v = after("FilterType"))
                    return pick(*v, {{"LowPass", 0}, {"HighPass", 1}, {"BandPass", 2}, {"Off", 3}});
            if (which == 2)
                if (auto v = after("FilterOrder"))
                    return pick(*v, {{"2", 0}, {"4", 1}});
            return std::nullopt;
        case E::LoFi: // type1 HP, LP, BP, OFF; type2 PRE, POST
            if (which == 1)
                if (auto v = after("FilterType"))
                    return pick(*v, {{"HighPass", 0}, {"LowPass", 1}, {"BandPass", 2}, {"Off", 3}});
            if (which == 2)
                if (auto v = after("FilterPosition"))
                    return pick(*v, {{"Pre", 0}, {"Post", 1}});
            return std::nullopt;
        case E::ParametricEq2: // band types HPF, LPF, PEAK, HI-SHELF, LO-SHELF
            if (auto v = after("FilterType"))
                return pick(*v, {{"HighPass", 0}, {"LowPass", 1}, {"Peaking", 2}, {"Peak", 2}, {"HighShelving", 3},
                                 {"LowShelving", 4}});
            return std::nullopt;
        case E::Ducker: // type1 SOURCE TRACK 1 … 16
            if (which == 1)
                if (auto v = after("SourceTrack"))
                {
                    int n = 0;
                    for (const char ch : *v)
                        n = ch >= '0' && ch <= '9' && n < 100 ? n * 10 + (ch - '0') : 1000;
                    if (! v->empty() && n >= 1 && n <= 16)
                        return n - 1;
                }
            return std::nullopt;
        default: break; // Compressor's option strings were never seen; the others have none
    }
    return std::nullopt;
}

void mapEffect(const Node& n, std::string_view base, m::EffectSlot& fx, Ctx& c, const std::string& at, bool insert)
{
    Props p(n, base, fx.extras, c.log, at);
    // [D] effectType, params; [S] sync, lookAhead, bypass, type1, type2 (data-model.md "EffectSlot"); [G] variants.
    // effectType: the stored names, also with the observed prefix "FxType_" (FxType_InsertDelay, FxType_LoFi, …).
    if (p.enumeration<m::EffectType>({"effectType"}, fx.type, {"FxType_", "EffectType_"}, {{"None", m::EffectType::Off}}))
    {
        const bool allowed = insert ? m::effectAllowedOnInsert(fx.type) : m::effectAllowedOnBus(fx.type);
        if (! allowed)
            c.log.issue(Kind::Structure, std::string(base) + " effect " + std::string(m::storedName(fx.type)) +
                                             " not offered on this slot (kept)",
                        at);
    }
    if (auto h = p.take({"params"}))
    {
        bool ok = h.v->t == Var::T::Array && h.v->a.size() == 6;
        std::vector<std::string> names;
        for (std::size_t k = 0; ok && k < 6; ++k)
        {
            const auto& e = h.v->a[k];
            ok = e.t == Var::T::Array && e.a.size() == 2 && e.a[0].t == Var::T::String && e.a[1].isNumber();
            if (! ok)
                break;
            names.push_back(e.a[0].s);
            double v = e.a[1].number();
            if (! std::isfinite(v) || v < 0.0 || v > 1.0)
            {
                c.log.issue(Kind::Clamped, std::string(base) + ".params value outside 0-1", at);
                v = std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
                ok = false; // keep the original array
            }
            fx.params[k] = v;
        }
        if (! ok)
            p.keep(h, Kind::TypeMismatch, "(not 6 x [string, number 0-1]: kept)");
        else if (std::any_of(names.begin(), names.end(), [](const std::string& s) { return s != "None"; }))
            p.keep(h, Kind::Opaque, "(slot names other than None: array kept, OQ-05)");
    }
    p.flag({"sync", "bSync"}, fx.sync);
    p.flag({"lookAhead", "bLookAhead"}, fx.lookAhead);
    if (insert)
        p.flag({"bypass", "bBypass"}, fx.bypass);
    else if (const auto h = p.peekHit({"bypass", "bBypass"}))
    {
        // FX-046: the reference stores a Send/Master bypass but never reads it, so the slot loads unbypassed (it
        // sounds the same); a set flag is kept silently for the exporter, under the name it writes.
        bool stored = false;
        p.flag({"bypass", "bBypass"}, stored);
        if (stored)
            p.keepSilently({h.v, "bBypass"});
    }
    for (int which = 1; which <= 2; ++which)
    {
        int& field = which == 1 ? fx.type1 : fx.type2;
        if (auto h = p.take({which == 1 ? std::string_view("bType1") : std::string_view("bType2")})) // observed
        {
            if (h.v->t != Var::T::String)
                p.keep(h, Kind::TypeMismatch);
            else if (auto index = optionIndex(fx.type, which, h.v->s))
            {
                field = *index;
                if (h.v->s.rfind("FilterOrder", 0) == 0)
                    c.log.issue(Kind::Guessed, std::string(base) + " FilterOrder<N> read as N = 2 → 12 dB/Oct", at);
            }
            else if (h.v->s != "FilterTypeOff") // FilterTypeOff = no option: nothing to keep
                p.keep(h, Kind::Opaque, "(an option of another effect type: carried over, kept)");
            continue;
        }
        std::int64_t v = 0;
        if (p.integer({which == 1 ? std::string_view("type1") : std::string_view("type2")}, v))
            field = static_cast<int>(std::clamp<std::int64_t>(v, 0, 255));
    }
    for (const auto& ch : n.children)
        keepNode(ch, fx.extras, c.log, at);
}

void mapTrack(const Node& n, m::Track& tr, Ctx& c, int scene, int track)
{
    const std::string at = where(scene, track);
    {
        Props p(n, "TrackData", tr.extras, c.log, at);
        // [D] all nine.
        p.enumeration<m::TrackMode>({"trackMode"}, tr.mode, {"TrackMode_"});
        p.enumeration<m::TriggerMode>({"triggerMode"}, tr.triggerMode, {"TriggerMode_"});
        p.intOrNone({"chokeGroup"}, tr.chokeGroup, 0, m::kChokeGroups);
        p.intIn({"volume"}, tr.volume, 0, 127);
        p.intIn({"pan"}, tr.pan, -64, 64);
        p.flag({"soloSetting"}, tr.solo);
        p.flag({"muteSetting"}, tr.mute);
        p.intOrNone({"colourIndex"}, tr.colour, 0, m::kPaletteColours - 1);
        if (auto h = p.take({"instrument"}))
        {
            if (h.v->t == Var::T::String && h.v->s == "Off")
                tr.instrument = 0;
            else if (h.v->t == Var::T::Int && h.v->i >= 0 && h.v->i < 65536)
                tr.instrument = static_cast<int>(h.v->i);
            else if (h.v->t == Var::T::String)
                p.keepText(h, h.v->s, Kind::EnumUnmapped); // the model holds a profile id (int), not a name
            else
                p.keep(h, Kind::TypeMismatch);
        }
    }
    int inserts = 0;
    for (const auto& ch : children(n))
    {
        const auto& cn = *ch.node;
        if (ch.base == "PlaybackData")
        {
            auto& pb = tr.playback;
            Props p(cn, "PlaybackData", pb.extras, c.log, at);
            // [D] all nine.
            constexpr std::int64_t kMaxFrames = std::int64_t {1} << 40;
            p.intIn({"startSample"}, pb.start, 0, kMaxFrames);
            p.intIn({"loopStartSample"}, pb.loopStart, 0, kMaxFrames);
            p.intIn({"lengthSample"}, pb.length, 0, kMaxFrames);
            p.flag({"bLoop"}, pb.loop);
            p.flag({"bReverse"}, pb.reverse);
            p.intIn({"pitchCent"}, pb.pitchCents, -9600, 9600);
            // Observed codes: "Off", "MT" (MASTER TEMPO); "RS" for RESAMPLE by analogy [G].
            p.enumeration<m::StretchMode>({"stretchMode"}, pb.stretchMode, {"StretchMode_", "TimeStretchMode_"},
                                          {{"MT", m::StretchMode::MasterTempo}, {"RS", m::StretchMode::Resample}});
            p.flag({"bBypass"}, pb.bypass);
            std::string url;
            if (p.string({"audioSourceUrl"}, url) && ! url.empty())
            {
                auto it = c.urlToId.find(url);
                if (it == c.urlToId.end())
                {
                    m::SampleRef ref;
                    if (c.opts.samples)
                        ref = c.opts.samples(url);
                    else
                    {
                        ref.id = "sp16:" + url;
                        ref.path = url;
                        ref.missing = true;
                    }
                    if (ref.displayName.empty())
                    {
                        const auto slash = url.find_last_of('/');
                        ref.displayName = slash == std::string::npos ? url : url.substr(slash + 1);
                    }
                    ++c.stats.samples;
                    if (ref.missing)
                    {
                        ++c.stats.samplesMissing;
                        c.log.issue(Kind::SampleMissing, url, at);
                    }
                    it = c.urlToId.emplace(url, ref.id).first;
                    if (c.project.sampleRefs.find(m::SampleKey {ref.id}) == nullptr)
                        c.project.sampleRefs.entries.push_back(std::move(ref));
                }
                pb.sample.id = it->second;
            }
            for (const auto& g : cn.children)
                keepNode(g, pb.extras, c.log, at);
        }
        else if (ch.base == "AmpEnvelopeData")
        {
            auto& e = tr.ampEnv;
            Props p(cn, "AmpEnvelopeData", e.extras, c.log, at);
            // Observed names first (attackNorm …, bVelocityEffective, bRetriggerMode, *CurveType, bSync, bBypass), then
            // the spec data model's [S] names.
            // The reference's k/127 → the model's k/128 form (Sp16Mapping.h envIndexFromReference; ENV-001).
            auto envNorm = [&p](Names names, double& field, core::ampenv::EnvStage stage) {
                double norm = std::numeric_limits<double>::quiet_NaN();
                p.doubleIn(names, norm, 0.0, 1.0);
                if (! std::isnan(norm))
                    field = core::ampenv::storedFromIndex(envIndexFromReference(stage, norm));
            };
            envNorm({"attackNorm", "attack"}, e.attack, core::ampenv::EnvStage::Attack);
            envNorm({"holdNorm", "hold"}, e.hold, core::ampenv::EnvStage::Hold);
            envNorm({"releaseNorm", "release"}, e.release, core::ampenv::EnvStage::Release);
            p.intIn({"volumeIndex"}, e.volumeIndex, 0, 127);
            p.flag({"bSync", "sync"}, e.sync);
            p.flag({"bVelocityEffective", "velocitySensitive"}, e.velocitySensitive);
            p.flag({"bRetriggerMode", "retrigger"}, e.retrigger);
            p.enumeration<m::EnvelopeCurve>({"attackCurveType", "attackCurve"}, e.attackCurve, {"EnvelopeCurve_", "Curve_"});
            p.enumeration<m::EnvelopeCurve>({"releaseCurveType", "releaseCurve"}, e.releaseCurve,
                                            {"EnvelopeCurve_", "Curve_"});
            p.flag({"bBypass", "bypass"}, e.bypass);
        }
        else if (ch.base == "SequencerData")
        {
            auto& s = tr.seq;
            Props p(cn, "SequencerData", s.extras, c.log, at);
            // [S] data-model.md "TrackSequencerSettings" (ints).
            p.intIn({"retriggerCount"}, s.retriggerCount, 0, m::kMaxRetriggerCount);
            if (auto h = p.peekHit({"retriggerSpeed"}); h && Props::packedRatio(*h.v))
            {
                // Packed hi / lo, read as the fraction of a step (our reading; the model's list: 1/8 … 1 step).
                p.take({"retriggerSpeed"});
                p.keepSilently(h);
                const auto [hi, lo] = *Props::packedRatio(*h.v);
                bool found = false;
                for (int k = 0; k < static_cast<int>(m::enumCount<m::RetriggerSpeed>()); ++k)
                    if (std::fabs(m::retriggerStepFraction(static_cast<m::RetriggerSpeed>(k)) -
                                  static_cast<double>(hi) / lo) < 1e-9)
                    {
                        s.retriggerSpeed = k;
                        found = true;
                    }
                c.log.issue(found ? Kind::Guessed : Kind::EnumUnmapped,
                            "SequencerData.retriggerSpeed packed hi/lo " + std::to_string(hi) + "/" + std::to_string(lo) +
                                (found ? " read as that fraction of a step" : " (no such retrigger speed)"),
                            at);
            }
            else
                p.enumInt<m::RetriggerSpeed>({"retriggerSpeed"}, s.retriggerSpeed);
            p.intIn({"offset"}, s.offset, -24, 24);
            p.enumInt<m::StepShiftArea>({"stepShiftArea"}, s.stepShiftArea, {"StepShiftArea_", "ShiftArea_"});
            p.intIn({"transpose"}, s.transpose, -48, 48);
        }
        else if (ch.base == "SliceData")
        {
            auto& s = tr.slices;
            Props p(cn, "SliceData", s.extras, c.log, at);
            // [D] grids, nGrids, windowOffset. The reference's grids layout is unknown (OQ-04): empty imports as empty
            // (the only documented case). A non-empty array shaped like the model's own encoding (SlicePoints.h: flat
            // (tag 0–64, frame ≥ 0) int pairs) is read as such [G]; the original is kept verbatim either way.
            if (auto h = p.take({"grids"}))
            {
                if (h.v->t == Var::T::Array && h.v->a.empty())
                    s.grids.clear();
                else
                {
                    bool pairs = h.v->t == Var::T::Array && h.v->a.size() % 2 == 0 && h.v->a.size() <= 2 * 64;
                    for (std::size_t k = 0; pairs && k < h.v->a.size(); ++k)
                    {
                        const auto& e = h.v->a[k];
                        pairs = (e.t == Var::T::Int || e.t == Var::T::Int64) && e.i >= 0 && (k % 2 == 1 || e.i <= 64);
                    }
                    if (pairs)
                    {
                        s.grids.clear();
                        for (const auto& e : h.v->a)
                            s.grids.push_back(e.i);
                        c.log.issue(Kind::Guessed, "SliceData.grids read as (tag, frame) pairs (layout unknown, OQ-04)",
                                    at);
                    }
                    p.keep(h, Kind::Opaque, pairs ? "(original kept, OQ-04)" : "(slice grids layout unknown, OQ-04)");
                }
            }
            p.intIn({"nGrids"}, s.gridCount, 0, 64);
            p.intIn({"windowOffset"}, s.windowOffset, -100000, 100000);
        }
        else if (ch.base == "ScaleSetting")
        {
            auto& s = tr.padScale;
            Props p(cn, "ScaleSetting", s.extras, c.log, at);
            p.enumInt<m::ScaleType>({"scaleType"}, s.scaleType, {"ScaleType_", "Scale_"}); // [S]
            p.intIn({"keyShift"}, s.keyShift, -127, 127);
            p.intIn({"windowOffset"}, s.windowOffset, -100000, 100000);
        }
        else if (ch.base == "ScaleSettingMidi")
        {
            auto& s = tr.midiScale;
            Props p(cn, "ScaleSettingMidi", s.extras, c.log, at);
            p.enumInt<m::ScaleType>({"scaleTypeMidi"}, s.scaleTypeMidi, {"ScaleType_", "Scale_"}); // [S]
            p.intIn({"scalePosition"}, s.scalePosition, -100000, 100000);
            p.intIn({"rootNote"}, s.rootNote, 0, 119); // C0…B9
        }
        else if (ch.base == "InsertFx")
        {
            const int k = ch.suffixed ? ch.index : inserts;
            ++inserts;
            if (k < 0 || k > 1)
                keepNode(cn, tr.extras, c.log, at, Kind::Structure);
            else
                mapEffect(cn, "InsertFx", tr.insertFx[static_cast<std::size_t>(k)], c, at, true);
        }
        else if (ch.base == "SendData")
        {
            Props p(cn, "SendData", tr.send.extras, c.log, at);
            p.intIn({"sendChannel"}, tr.send.channel, 0, 255); // [D]
            p.intIn({"sendAmount"}, tr.send.amount, 0, 127);   // [D]
        }
        else if (ch.base == "LfoData")
        {
            auto& l = tr.lfo;
            Props p(cn, "LfoData", l.extras, c.log, at);
            // [D] all ten. The model's LFO enums differ from the spec: strings they lack are kept.
            p.doubleIn({"speed"}, l.speed, -1e9, 1e9);
            p.intIn({"depth"}, l.depth, -100000, 100000);
            p.intIn({"phaseOffset"}, l.phaseOffset, -100000, 100000);
            p.intIn({"destination"}, l.destination, -100000, 100000); // the raw id stays for the export
            if (const auto pid = lfoDestinationId(l.destination))
                l.extras.set("native.destinationId", "\"" + *pid + "\"");
            else if (l.destination != 0) // 0 = no destination (every observed and synthetic default)
                c.log.issue(Kind::EnumUnmapped,
                            "LfoData.destination=" + std::to_string(l.destination) + " (no FX parameter; NONE)", at);
            p.enumeration<m::LfoShape>({"type"}, l.shape, {"LfoType_", "LfoShape_"});
            p.enumeration<m::LfoTriggerMode>({"triggerMode"}, l.triggerMode, {"LfoTriggerMode_"});
            // Observed "Full" (the 1.60 list FULL / FULL & HALF): the model's enum lacks them — nearest model value
            // (FULL → Step, FULL & HALF → Both) and the string kept.
            if (auto h = p.peekHit({"triggerOn"}); h && h.v->t == Var::T::String &&
                                                   (h.v->s == "Full" || h.v->s == "FullAndHalf" || h.v->s == "FullHalf"))
            {
                p.take({"triggerOn"});
                l.triggerOn = h.v->s == "Full" ? m::LfoTriggerOn::Step : m::LfoTriggerOn::Both;
                l.extras.set("sp16.LfoData.triggerOn", h.v->s, m::Extras::Encoding::Text);
                c.log.issue(Kind::ModelLacks, "LfoData.triggerOn='" + h.v->s + "' (nearest model value, string kept)",
                            at);
            }
            else
                p.enumeration<m::LfoTriggerOn>({"triggerOn"}, l.triggerOn, {"LfoTriggerOn_"});
            p.flag({"sync"}, l.sync);
            p.intIn({"randStartNum"}, l.randomSeedCount, 0, 127); // the model's 0–127; larger stored values kept
            p.flag({"bypass"}, l.bypass);
        }
        else if (ch.base == "MidiTrackData")
        {
            auto& mt = tr.midi;
            {
                Props p(cn, "MidiTrackData", mt.extras, c.log, at);
            }
            for (const auto& g : children(cn))
            {
                const auto& gn = *g.node;
                if (g.base == "MidiNoteData")
                {
                    Props p(gn, "MidiNoteData", mt.extras, c.log, at);
                    // [D] note1-4, velocity, length (research tree); [S] MidiBypassFlg. note1 is the base note,
                    // note2–4 the chord's other notes as semitones from it, −127 ~ OFF (0) ~ +127 (the manual's MIDI
                    // NOTE settings; the observed 0 = off), stored as they are.
                    for (int k = 0; k < 4; ++k)
                    {
                        const std::string_view name = k == 0 ? "note1" : k == 1 ? "note2" : k == 2 ? "note3" : "note4";
                        p.intIn({name}, mt.notes[static_cast<std::size_t>(k)], k == 0 ? 0 : -127, k == 0 ? 119 : 127);
                    }
                    p.intIn({"velocity"}, mt.velocity, 0, 127);
                    if (auto h = p.peekHit({"length"}); h && h.v->t == Var::T::Int)
                    {
                        // An int (observed): quarter steps (our reading; the model's 0.25–64 steps).
                        p.take({"length"});
                        p.keepSilently(h);
                        mt.length = std::clamp(static_cast<double>(h.v->i) / 4.0, 0.25, 64.0);
                        c.log.issue(Kind::Guessed, "MidiNoteData.length int read as quarter steps", at);
                    }
                    else
                        p.doubleIn({"length"}, mt.length, 0.0, 64.0);
                    p.flag({"MidiBypassFlg"}, mt.noteBypass);
                }
                else if (g.base == "MidiCc1Data" || g.base == "MidiCc2Data")
                {
                    // Observed names: pitchBend / afterTouch Value + Active (Cc1), param<k>CcNo / Value / Active (Cc1: 3–6
                    // → cc1[0..3], Cc2: 1–6 → cc2[0..5]), MidiBypassFlg; Active / bypass stored as ints 0 / 1.
                    Props p(gn, g.base, mt.extras, c.log, at);
                    const bool one = g.base == "MidiCc1Data";
                    p.flag({"MidiBypassFlg"}, one ? mt.cc1Bypass : mt.cc2Bypass);
                    if (one)
                    {
                        p.intIn({"pitchBendValue"}, mt.pitchBendValue, -8192, 16383);
                        p.flag({"pitchBendActive"}, mt.pitchBendActive);
                        p.intIn({"afterTouchValue"}, mt.aftertouchValue, 0, 127);
                        p.flag({"afterTouchActive"}, mt.aftertouchActive);
                    }
                    const int first = one ? 3 : 1, count = one ? 4 : 6;
                    for (int k = 0; k < count; ++k)
                    {
                        auto& cc = one ? mt.cc1[static_cast<std::size_t>(k)] : mt.cc2[static_cast<std::size_t>(k)];
                        const std::string stem = "param" + std::to_string(first + k);
                        const std::string ccNo = stem + "CcNo", value = stem + "Value", active = stem + "Active";
                        p.intIn({std::string_view(ccNo)}, cc.cc, 0, 127);
                        p.intIn({std::string_view(value)}, cc.value, 0, 127);
                        p.flag({std::string_view(active)}, cc.active);
                    }
                }
                else if (g.base == "MidiOtherData")
                {
                    Props p(gn, "MidiOtherData", mt.extras, c.log, at);
                    // Observed names bankselmsb / banksellsb / programchange, then the [S] ones. Values as stored.
                    p.intIn({"bankselmsb", "bankMsb"}, mt.bankMsb, -1, 127);
                    p.intIn({"banksellsb", "bankLsb"}, mt.bankLsb, -1, 127);
                    p.intIn({"programchange", "program"}, mt.program, -1, 127);
                }
                else if (g.base == "MidiModeData")
                {
                    Props p(gn, "MidiModeData", mt.extras, c.log, at);
                    p.intOrNone({"midimode"}, mt.mode, 0, 255); // [S] `midimode` int; −1 = none
                }
                else
                    keepNode(gn, mt.extras, c.log, at);
            }
        }
        else
            keepNode(cn, tr.extras, c.log, at);
    }
}

void mapPattern(const Node& n, m::Pattern& pat, Ctx& c, int scene, int index, const FxLockTypes& fxTypes)
{
    const std::string at = where(scene) + " pattern " + std::to_string(index + 1);
    Props p(n, "PatternData", pat.extras, c.log, at);
    // [D] Steps, Length, TrackMutes, TrackSolos.
    p.intIn({"Length"}, pat.length, 1, m::kSteps);
    for (const auto* name : {"TrackMutes", "TrackSolos"})
        if (auto h = p.take({std::string_view(name)}))
        {
            auto& dst = std::string_view(name) == "TrackMutes" ? pat.trackMutes : pat.trackSolos;
            bool ok = h.v->t == Var::T::Array && h.v->a.size() == 16;
            for (std::size_t k = 0; ok && k < 16; ++k)
                ok = h.v->a[k].isBool();
            if (! ok)
                p.keep(h, Kind::TypeMismatch, "(not 16 bools)");
            else
                for (std::size_t k = 0; k < 16; ++k)
                    dst[k] = h.v->a[k].t == Var::T::True;
        }
    if (auto h = p.take({"Steps"}))
    {
        if (h.v->t != Var::T::Binary)
        {
            p.keep(h, Kind::TypeMismatch, "(not binary)");
            return;
        }
        auto records = decodeSteps(h.v->bin);
        if (! records)
        {
            p.keep(h, Kind::Opaque, "(unreadable: " + records.error().message + ")");
            return;
        }
        mapSteps(records.value(), pat, c, at, fxTypes);
        const auto again = encodePattern(pat, c.map, &fxTypes);
        if (const auto diff = firstDifference(again, records.value()); ! diff.empty())
        {
            pat.extras.set("sp16.PatternData.Steps", toBase64(h.v->bin), m::Extras::Encoding::Base64);
            c.log.issue(Kind::Opaque, "PatternData.Steps kept verbatim (not reproducible from the model at " + diff + ")",
                        at);
            ++c.stats.verbatimPatterns;
        }
    }
    for (const auto& ch : n.children)
        keepNode(ch, pat.extras, c.log, at);
}

void mapRouting(const Node& n, m::Scene& sc, Ctx& c, int scene)
{
    const std::string at = where(scene);
    {
        Props p(n, "RoutingData", sc.routing.extras, c.log, at);
        // [S] assignMode, headphoneOutCh, effectMode (sp16-interop.md node table); AFS field names [G].
        p.enumeration<m::AssignMode>({"assignMode"}, sc.routing.assignMode, {"AssignMode_"});
        p.enumInt<m::HeadphoneCue>({"headphoneOutCh"}, sc.routing.headphoneSource, {"HeadphoneOutCh_"});
        p.enumeration<m::FxMode>({"effectMode"}, sc.routing.fxMode, {"EffectMode"});
        p.intIn({"daveSmithAssignTrack", "afsAssignment"}, sc.routing.afsAssignment, -1, 15, true);
        // Observed: a per-scene copy of the project's AFS fields; the model holds them per project only.
        for (const auto name : {"bDaveSmithFilter", "daveSmithRouteMode", "daveSmithSendMode", "daveSmithSendTrack"})
            if (auto h = p.take({std::string_view(name)}))
                p.keep(h, Kind::ModelLacks, "(per-scene AFS copy, kept)");
    }
    int running = 0;
    for (const auto& ch : children(n))
    {
        if (ch.base != "TrackRoutingData")
        {
            keepNode(*ch.node, sc.routing.extras, c.log, at);
            continue;
        }
        const int t = ch.suffixed ? ch.index : running;
        ++running;
        if (t < 0 || t > 15)
        {
            keepNode(*ch.node, sc.routing.extras, c.log, at, Kind::Structure);
            continue;
        }
        auto& tr = sc.tracks[static_cast<std::size_t>(t)].routing;
        Props p(*ch.node, "TrackRoutingData", tr.extras, c.log, where(scene, t));
        // Observed assignableOutCh "AssignToNull" (--), "AssignTo34" …; mono "AssignTo1" … by analogy [G].
        p.enumeration<m::OutputAssign>({"assignableOutCh"}, tr.output, {},
                                       {{"AssignToNull", m::OutputAssign::Master}, {"AssignTo12", m::OutputAssign::Out12},
                                        {"AssignTo34", m::OutputAssign::Out34}, {"AssignTo56", m::OutputAssign::Out56},
                                        {"AssignTo78", m::OutputAssign::Out78}, {"AssignTo1", m::OutputAssign::Mono1},
                                        {"AssignTo2", m::OutputAssign::Mono2}, {"AssignTo3", m::OutputAssign::Mono3},
                                        {"AssignTo4", m::OutputAssign::Mono4}, {"AssignTo5", m::OutputAssign::Mono5},
                                        {"AssignTo6", m::OutputAssign::Mono6}, {"AssignTo7", m::OutputAssign::Mono7},
                                        {"AssignTo8", m::OutputAssign::Mono8}});
        p.enumeration<m::OutputAssign>({"outputAssign", "outAssign", "assign"}, tr.output, {"OutputAssign_"}, {},
                                       true);
    }
}

void mapScene(const Node& n, m::Scene& sc, Ctx& c, int scene)
{
    const std::string at = where(scene);
    {
        Props p(n, "SceneData", sc.extras, c.log, at);
        p.intOrNone({"colourIndex"}, sc.colour, 0, m::kPaletteColours - 1); // [D]; −1 = none (default colour)
        p.string({"name"}, sc.name);                                    // [D]
        double bpm = 0;
        auto h = p.take({"bpm"});                                       // [D]; 0 = no scene BPM (data-model.md)
        if (h && h.v->isNumber())
        {
            bpm = h.v->number();
            if (bpm <= 0.0)
                sc.bpm.reset();
            else if (bpm < m::kMinTempo || bpm > m::kMaxTempo || ! std::isfinite(bpm))
            {
                p.keep(h, Kind::Clamped, "out of tempo range");
                sc.bpm = std::isfinite(bpm) ? std::clamp(bpm, m::kMinTempo, m::kMaxTempo) : 120.0;
            }
            else
                sc.bpm = bpm;
        }
        else if (h && h.v->t != Var::T::Void)
            p.keep(h, Kind::TypeMismatch);
    }
    std::array<bool, 16> trackSeen {}, patternSeen {};
    int trackRunning = 0, patternRunning = 0;
    bool send = false, master = false, routing = false;
    for (const auto& ch : children(n))
    {
        const auto& cn = *ch.node;
        if (ch.base == "TrackData")
        {
            const int t = ch.suffixed ? ch.index : trackRunning;
            ++trackRunning;
            if (t < 0 || t > 15 || trackSeen[static_cast<std::size_t>(t)])
            {
                keepNode(cn, sc.extras, c.log, at, Kind::Structure);
                continue;
            }
            trackSeen[static_cast<std::size_t>(t)] = true;
            ++c.stats.tracks;
            mapTrack(cn, sc.tracks[static_cast<std::size_t>(t)], c, scene, t);
        }
        else if (ch.base == "PatternData")
        {
            // OQ-03: the suffix is the pattern index; a suffix 16–255 whose high part is this scene is read as a
            // project-wide number (scene × 16 + pattern).
            int q = ch.suffixed ? ch.index : patternRunning;
            ++patternRunning;
            if (q >= 16 && q < 256 && q / 16 == scene)
            {
                c.log.issue(Kind::Guessed, "PatternData suffix read as scene*16+pattern", at);
                q %= 16;
            }
            if (q < 0 || q > 15 || patternSeen[static_cast<std::size_t>(q)])
            {
                keepNode(cn, sc.extras, c.log, at, Kind::Structure);
                continue;
            }
            if (! ch.suffixed)
                c.log.issue(Kind::Structure, "PatternData without a suffix (placed by position)", at);
            patternSeen[static_cast<std::size_t>(q)] = true;
            ++c.stats.patterns;
            c.stats.patternList.push_back(std::to_string(scene + 1) + "." + std::to_string(q + 1));
            mapPattern(cn, sc.patterns[static_cast<std::size_t>(q)], c, scene, q, insertTypesOf(sc));
        }
        else if (ch.base == "SendFx" && ! send)
        {
            send = true;
            mapEffect(cn, "SendFx", sc.sendFx, c, at, false);
        }
        else if (ch.base == "MasterFx" && ! master)
        {
            master = true;
            mapEffect(cn, "MasterFx", sc.masterFx, c, at, false);
        }
        else if (ch.base == "RoutingData" && ! routing)
        {
            routing = true;
            mapRouting(cn, sc, c, scene);
        }
        else
            keepNode(cn, sc.extras, c.log, at, ch.base == "SendFx" || ch.base == "MasterFx" || ch.base == "RoutingData"
                                                   ? Kind::Structure
                                                   : Kind::UnknownNode);
    }
    const int missingTracks = static_cast<int>(std::count(trackSeen.begin(), trackSeen.end(), false));
    if (missingTracks > 0)
        c.log.issue(Kind::Structure, "TrackData absent (defaults)", at);
}

void mapArrangement(const Node& n, m::Arrangement& arr, Ctx& c)
{
    Props p(n, "ArrangementData", arr.extras, c.log, "arrangement");
    // [D] length, Sections. Section fields unknown (OQ-02): all-default → empty arrangement; otherwise kept verbatim.
    if (auto h = p.take({"length"}))
    {
        if (h.v->t == Var::T::Int)
            arr.extras.set("sp16.ArrangementData.length", varToJson(*h.v), m::Extras::Encoding::Json);
        else
            p.keep(h, Kind::TypeMismatch);
    }
    if (auto h = p.take({"Sections"}))
    {
        if (h.v->t != Var::T::Binary)
            p.keep(h, Kind::TypeMismatch);
        else if (auto rec = decodeSections(h.v->bin); ! rec)
            p.keep(h, Kind::Opaque, "(unreadable: " + rec.error().message + ")");
        else
        {
            const auto& r = rec.value();
            const bool allDefault =
                std::all_of(r.begin(), r.end(), [](const SectionRecord& s) { return s.isDefault(); });
            if (r.size() != static_cast<std::size_t>(kSectionCount))
                c.log.issue(Kind::Structure, "Sections has " + std::to_string(r.size()) + " records, expected 999",
                            "arrangement");
            if (! allDefault || r.size() != static_cast<std::size_t>(kSectionCount) ||
                encodeSections(r) != h.v->bin)
            {
                arr.extras.set("sp16.ArrangementData.Sections", toBase64(h.v->bin), m::Extras::Encoding::Base64);
                c.log.issue(Kind::Opaque, allDefault ? "ArrangementData.Sections (non-canonical bytes kept)"
                                                     : "ArrangementData.Sections: arrangement not imported (OQ-02)",
                            "arrangement");
            }
        }
    }
    arr.sections.clear();
    arr.length = 0;
    for (const auto& ch : n.children)
        keepNode(ch, arr.extras, c.log, "arrangement");
}

void mapRibbon(const Node& n, m::RibbonSettings& rib, Ctx& c)
{
    {
        Props p(n, "RibbonData", rib.extras, c.log, "ribbon");
    }
    int users = 0;
    for (const auto& ch : children(n))
    {
        if (ch.base == "UserSetting")
        {
            const int u = ch.suffixed ? ch.index : users;
            ++users;
            if (u < 0 || u > 1)
            {
                keepNode(*ch.node, rib.extras, c.log, "ribbon", Kind::Structure);
                continue;
            }
            {
                Props p(*ch.node, "UserSetting", rib.extras, c.log, "ribbon user " + std::to_string(u + 1));
            }
            int items = 0;
            for (const auto& it : children(*ch.node))
            {
                const int k = it.suffixed ? it.index : items;
                ++items;
                if (it.base != "Item" || k < 0 || k > 7)
                {
                    keepNode(*it.node, rib.extras, c.log, "ribbon", Kind::Structure);
                    continue;
                }
                auto& item = rib.user[static_cast<std::size_t>(u)][static_cast<std::size_t>(k)];
                const std::string at = "ribbon user " + std::to_string(u + 1) + " item " + std::to_string(k + 1);
                Props p(*it.node, "Item", rib.extras, c.log, at);
                p.doubleIn({"topValue"}, item.top, 0.0, 1.0); // [D]; void = unset (default kept)
                p.doubleIn({"bottomValue"}, item.bottom, 0.0, 1.0);
                std::string target;
                auto h = p.take({"target"}); // observed: an int, 255 = none
                if (! h)
                    h = p.take({"paramName", "paramId"}, true);
                if (h && h.v->t == Var::T::Int && h.v->i == 255)
                    item.target.clear();
                else if (h && h.v->t == Var::T::String)
                {
                    if (core::builtinParams().find(h.v->s) != nullptr || h.v->s.empty())
                        item.target = h.v->s;
                    else
                        p.keepText(h, h.v->s, Kind::EnumUnmapped);
                }
                else if (h && h.v->t != Var::T::Void)
                    p.keep(h, Kind::EnumUnmapped, "(target id format unknown)");
            }
        }
        else if (ch.base == "PitchSetting")
        {
            Props p(*ch.node, "PitchSetting", rib.extras, c.log, "ribbon pitch");
            p.intIn({"topCent"}, rib.pitchTopCents, -4800, 4800); // observed
            p.intIn({"bottomCent"}, rib.pitchBottomCents, -4800, 4800);
            p.intIn({"topValue"}, rib.pitchTopCents, -4800, 4800, true);
            p.intIn({"bottomValue"}, rib.pitchBottomCents, -4800, 4800, true);
        }
        else
            keepNode(*ch.node, rib.extras, c.log, "ribbon");
    }
}

void mapMidi(const Node& n, m::MidiSettings& midi, Ctx& c)
{
    {
        Props p(n, "MidiSettingData", midi.extras, c.log, "midi");
    }
    for (const auto& ch : children(n))
    {
        if (ch.base != "Input" && ch.base != "Output")
        {
            keepNode(*ch.node, midi.extras, c.log, "midi");
            continue;
        }
        auto& port = ch.base == "Input" ? midi.input : midi.output;
        Props p(*ch.node, ch.base, midi.extras, c.log, "midi " + std::string(ch.base));
        // [D] all five; channelNo = 16 ints, default 0..15 (0-based) → the model's 1–16.
        p.flag({"bUseClock"}, port.clock);
        p.flag({"bUseStartStop"}, port.startStop);
        p.flag({"bUseNoteCC"}, port.noteCc);
        p.flag({"bUseSongPosition"}, port.songPosition);
        if (auto h = p.take({"channelNo"}))
        {
            bool ok = h.v->t == Var::T::Array && h.v->a.size() == 16;
            for (std::size_t k = 0; ok && k < 16; ++k)
                ok = h.v->a[k].t == Var::T::Int && h.v->a[k].i >= 0 && h.v->a[k].i <= 15;
            if (! ok)
                p.keep(h, Kind::TypeMismatch, "(not 16 channels 0-15)");
            else
                for (std::size_t k = 0; k < 16; ++k)
                    port.channels[k] = static_cast<int>(h.v->a[k].i) + 1;
        }
    }
}

void mapSettings(const Node& n, m::ProjectSettings& s, Ctx& c)
{
    Props p(n, "ProjectData", s.extras, c.log, "project");
    // [D] the research's 16 named fields; [S] daveSmithSendTrack, bSoftLimitter, bPlayArrangement, padSequenceStart.
    p.intIn({"masterVolume"}, s.masterVolume, 0, 127);
    p.intIn({"headphoneVolume"}, s.headphoneVolume, 0, 127);
    p.intIn({"sendTrackVolume"}, s.sendTrackVolume, 0, 127);
    p.intIn({"monitorVolume"}, s.monitorVolume, 0, 127);
    p.flag({"bDaveSmithFilter"}, s.afsEnabled);
    p.enumeration<m::AfsRouteMode>({"daveSmithRouteMode"}, s.afsRouteMode, {"DaveSmithRouteMode_"});
    {
        m::AfsSendMode mode = static_cast<m::AfsSendMode>(s.afsSendMode);
        if (p.enumeration<m::AfsSendMode>({"daveSmithSendMode"}, mode, {"DaveSmithSendMode_"},
                                          {{"DaveSmithSendMode_Pre", m::AfsSendMode::PreFx},
                                           {"DaveSmithSendMode_Post", m::AfsSendMode::PostFx}}))
            s.afsSendMode = static_cast<int>(mode);
    }
    p.intIn({"daveSmithSendTrack"}, s.afsSendTrack, -1, 15);
    p.flag({"bSoftLimitter"}, s.softLimiter);
    p.flag({"bPlayArrangement"}, s.playArrangement);
    {
        double swing = 0;
        auto h = p.take({"swingRate"});
        if (h && h.v->isNumber())
        {
            // Observed: an int, 0 = no swing — the shown 0–50 %; the model stores 50–75 (shown = 2 × (stored − 50)).
            swing = h.v->number();
            if (std::isfinite(swing) && swing >= 0.0 && swing <= 50.0)
            {
                if (swing != 0.0)
                {
                    p.keepSilently(h);
                    c.log.issue(Kind::Guessed, "ProjectData.swingRate read as the shown 0-50 %", "project");
                }
                s.swing = static_cast<int>(std::lround(50.0 + swing / 2.0));
            }
            else
            {
                p.keep(h, Kind::Clamped, "not in 0..50 (the shown swing %)");
                s.swing = m::kMinSwing;
            }
        }
        else if (h && h.v->t != Var::T::Void)
            p.keep(h, Kind::TypeMismatch);
    }
    p.doubleIn({"projectBpm"}, s.tempo, m::kMinTempo, m::kMaxTempo);
    for (const auto name : {"patternQuantizeUnit", "padQuantizeUnit"})
    {
        auto& field = std::string_view(name) == "patternQuantizeUnit" ? s.patternQuantize : s.padQuantize;
        if (auto h = p.peekHit({std::string_view(name)}); h && Props::packedRatio(*h.v))
        {
            // Observed packed hi / lo ints: read as a note length hi / lo: 1/16 STEP, 1/4 BEAT, 1/1 BAR, longer
            // PATTERN. The stored value is kept.
            p.take({std::string_view(name)});
            p.keepSilently(h);
            const auto [hi, lo] = *Props::packedRatio(*h.v);
            const double r = static_cast<double>(hi) / lo;
            std::optional<m::QuantizeUnit> q;
            if (std::fabs(r - 1.0 / 16) < 1e-9)
                q = m::QuantizeUnit::Step;
            else if (std::fabs(r - 0.25) < 1e-9)
                q = m::QuantizeUnit::Beat;
            else if (std::fabs(r - 1.0) < 1e-9)
                q = m::QuantizeUnit::Bar;
            else if (r > 1.0)
                q = m::QuantizeUnit::Pattern;
            if (q)
                field = *q;
            c.log.issue(q ? Kind::Guessed : Kind::EnumUnmapped,
                        "ProjectData." + std::string(name) + " packed hi/lo " + std::to_string(hi) + "/" +
                            std::to_string(lo) + (q ? " read as a note length" : " (no such quantise unit)"),
                        "project");
        }
        else if (auto h0 = p.peekHit({std::string_view(name)}); h0 && h0.v->t == Var::T::Int && h0.v->i == 0)
        {
            p.take({std::string_view(name)});
            field = m::QuantizeUnit::Off;
        }
        else
            p.enumeration<m::QuantizeUnit>({std::string_view(name)}, field, {"QuantizeUnit_", "Quantize_"});
    }
    p.flag({"padSequenceStart"}, s.padStartsSequence);
    p.intIn({"sceneIndex"}, s.currentScene, 0, 15);
    p.intIn({"patternIndex"}, s.currentPattern, 0, 15);
    if (auto h = p.take({"projectFilePath"}))
    {
        if (h.v->t == Var::T::String)
            s.extras.set("sp16.ProjectData.projectFilePath", h.v->s, m::Extras::Encoding::Text);
        else
            p.keep(h, Kind::TypeMismatch);
        c.log.issue(Kind::ModelLacks, "ProjectData.projectFilePath (kept)", "project");
    }
    p.intIn({"versionNo"}, s.formatVersion, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    p.intIn({"minorVersionNo"}, s.formatMinorVersion, std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max());
}
} // namespace

Result<void> mapProject(const Node& root, m::Project& out, const MapOptions& opts, ImportLog& log, MapStats* statsOut)
{
    const auto [base, suffix] = splitSuffix(root.type);
    if (base != "ProjectData")
        return Result<void>::failure("Not a project tree (the root node is \"" + root.type + "\")");
    m::resetToDefaults(out);
    MapStats stats;
    const ParamIndexMap& map = opts.map != nullptr ? *opts.map : ParamIndexMap::builtin();
    Ctx c {map, opts, log, stats, out, {}, {}, {}};
    for (const auto& o : map.ordinals)
    {
        c.specs[static_cast<std::size_t>(o.ordinal)] = specOf(o.param);
        c.slots[static_cast<std::size_t>(o.ordinal)] = appSlotOf(c.specs[static_cast<std::size_t>(o.ordinal)]);
    }
    mapSettings(root, out.settings, c);
    std::array<bool, 16> sceneSeen {};
    int running = 0;
    bool arrangement = false, ribbon = false, midi = false;
    for (const auto& ch : children(root))
    {
        const auto& cn = *ch.node;
        if (ch.base == "SceneData")
        {
            const int s = ch.suffixed ? ch.index : running;
            ++running;
            if (s < 0 || s > 15 || sceneSeen[static_cast<std::size_t>(s)])
            {
                keepNode(cn, out.extras, log, "project", Kind::Structure);
                continue;
            }
            sceneSeen[static_cast<std::size_t>(s)] = true;
            ++stats.scenes;
            mapScene(cn, out.scenes[static_cast<std::size_t>(s)], c, s);
        }
        else if (ch.base == "ArrangementData" && ! arrangement)
        {
            arrangement = true;
            mapArrangement(cn, out.arrangement, c);
        }
        else if (ch.base == "RibbonData" && ! ribbon)
        {
            ribbon = true;
            mapRibbon(cn, out.ribbon, c);
        }
        else if (ch.base == "MidiSettingData" && ! midi)
        {
            midi = true;
            mapMidi(cn, out.midi, c);
        }
        else
            keepNode(cn, out.extras, log, "project",
                     ch.base == "ArrangementData" || ch.base == "RibbonData" || ch.base == "MidiSettingData"
                         ? Kind::Structure
                         : Kind::UnknownNode);
    }
    for (int s = 0; s < 16; ++s)
        if (! sceneSeen[static_cast<std::size_t>(s)])
            log.issue(Kind::Structure, "SceneData absent (defaults)", where(s));
    std::vector<std::string> problems;
    if (! m::validate(out, problems, 20))
        for (const auto& pr : problems)
            log.issue(Kind::Structure, "validate: " + pr, "project");
    // Provenance (FMT-046): the format, the source's version fields and this importer's version; the file name and
    // content id are the caller's (it has the file).
    out.origin.format = "sp16-prj";
    out.origin.sourceVersion = out.settings.formatVersion;
    out.origin.sourceMinorVersion = out.settings.formatMinorVersion;
    out.origin.importer = kImporterVersion;
    if (statsOut != nullptr)
        *statsOut = std::move(stats);
    return {};
}

// The insert effect types of a scene's tracks: the FX lock words' index bytes depend on them (fxLockIndex).
FxLockTypes insertTypesOf(const core::model::Scene& sc)
{
    FxLockTypes out {};
    for (int t = 0; t < kTrackCount; ++t)
        for (int u = 0; u < 2; ++u)
            out[static_cast<std::size_t>(t)][static_cast<std::size_t>(u)] =
                sc.tracks[static_cast<std::size_t>(t)].insertFx[static_cast<std::size_t>(u)].type;
    return out;
}

std::vector<StepRecord> encodePattern(const m::Pattern& pat, const ParamIndexMap& map, const FxLockTypes* fxTypes)
{
    std::vector<StepRecord> out(static_cast<std::size_t>(kStepCount * kTrackCount), StepRecord::emptyRecord());
    std::array<const core::ParamSpec*, 23> specs {};
    std::array<int, 23> slots {};
    for (const auto& o : map.ordinals)
    {
        specs[static_cast<std::size_t>(o.ordinal)] = specOf(o.param);
        slots[static_cast<std::size_t>(o.ordinal)] = appSlotOf(specs[static_cast<std::size_t>(o.ordinal)]);
    }
    auto word = [](int value, int idx) {
        LockWord w;
        w.vct = 0;
        w.value22 = std::clamp(value, -(1 << 21), (1 << 21) - 1);
        w.idx = static_cast<std::uint8_t>(idx);
        return LockWord::unpack(w.pack());
    };
    for (int s = 0; s < kStepCount; ++s)
        for (int t = 0; t < kTrackCount; ++t)
        {
            const auto& cell = pat.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            auto& r = out[recordIndex(s, t)];
            // A programmed record names its own track and step; the velocity has no place in the record.
            const bool programmed = cell.trigger != m::Step::kEmpty;
            r.trigger = programmed ? t : kEmptyTrigger;
            r.value2 = programmed ? s : kEmptyValue2;
            r.typeFlags = m::referenceTriggerCode(cell.triggerType);
            if (cell.lockIndex == m::Step::kNoLocks || cell.lockIndex >= pat.locks.size())
                continue;
            const auto& l = pat.locks[cell.lockIndex];
            for (const auto& o : map.ordinals)
            {
                const int slot = slots[static_cast<std::size_t>(o.ordinal)];
                const auto* spec = specs[static_cast<std::size_t>(o.ordinal)];
                if (slot < 0 || spec == nullptr || (l.mask >> slot & 1u) == 0)
                    continue;
                const double native = l.values[static_cast<std::size_t>(slot)];
                switch (o.cls)
                {
                    case ValueClass::Raw:
                    case ValueClass::Norm:
                    case ValueClass::NormOrIndex:
                        r.slots[static_cast<std::size_t>(o.ordinal)] =
                            word(encodeValue(o, *spec, static_cast<float>(native)), o.ordinal);
                        break;
                    case ValueClass::Frames:
                    {
                        const std::int64_t f = std::max<std::int64_t>(0, std::llround(native));
                        r.slots[static_cast<std::size_t>(o.ordinal)] = word(static_cast<int>(f % kFrameBase), o.ordinal);
                        if (f >= kFrameBase && o.highSlot >= 0 && o.highSlot < 23)
                            r.slots[static_cast<std::size_t>(o.highSlot)] =
                                word(static_cast<int>(f / kFrameBase), o.highSlot);
                        break;
                    }
                }
            }
            for (int k = 23; k < kLockWords; ++k)
            {
                if ((l.mask >> k & 1u) == 0)
                    continue;
                // The index byte is the FxParamIndex of the track's effect (Sp16Mapping.h); without one the lock cannot
                // be written (the exporter reports it, fxLockExportable).
                const int unit = (k - 23) / 6, sub = (k - 23) % 6;
                const auto type = fxTypes != nullptr ? (*fxTypes)[static_cast<std::size_t>(t)][static_cast<std::size_t>(unit)]
                                                     : m::EffectType::Off;
                const int idx = fxLockIndex(type, sub);
                if (idx < 0)
                    continue;
                r.slots[static_cast<std::size_t>(k)] = word(
                    static_cast<int>(std::lround(
                        std::clamp(static_cast<double>(l.values[static_cast<std::size_t>(k)]), 0.0, 1.0) * kValueFull)),
                    idx);
            }
        }
    // Cell overrides kept by the import (sp16.step[s].track[t]).
    constexpr std::string_view kPrefix = "sp16.step[";
    for (const auto& e : pat.extras.entries)
    {
        if (e.key.compare(0, kPrefix.size(), kPrefix) != 0)
            continue;
        int s = -1, t = -1;
        if (std::sscanf(e.key.c_str(), "sp16.step[%d].track[%d]", &s, &t) != 2 || s < 0 || s >= kStepCount || t < 0 ||
            t >= kTrackCount)
            continue;
        auto& r = out[recordIndex(s, t)];
        const auto& cell = pat.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
        const m::StepLocks* l =
            cell.lockIndex != m::Step::kNoLocks && cell.lockIndex < pat.locks.size() ? &pat.locks[cell.lockIndex] : nullptr;
        const auto raw = CellRaw::parse(e.value);
        // An override applies only while the model still holds what the import made of it (an edited cell is
        // written from the model): the odd first / second fields of a programmed record while the cell is programmed,
        // the odd second field of an empty record while the cell is empty.
        const bool programmed = cell.trigger != m::Step::kEmpty;
        if (raw.trigger && programmed)
        {
            r.trigger = *raw.trigger;
            if (raw.value2)
                r.value2 = *raw.value2;
        }
        else if (! raw.trigger && raw.value2 && ! programmed)
            r.value2 = *raw.value2;
        if (raw.type && cell.triggerType == m::TriggerType::Full)
            r.typeFlags = *raw.type;
        for (const auto& [k, w] : raw.words)
        {
            if (k < 0 || k >= kLockWords)
                continue;
            const auto lw = LockWord::unpack(w);
            if (! lw.empty && k < 23 && slots[static_cast<std::size_t>(k)] >= 0 &&
                map.ordinals[static_cast<std::size_t>(k)].cls != ValueClass::Frames)
            {
                const int slot = slots[static_cast<std::size_t>(k)];
                const auto& spec = *specs[static_cast<std::size_t>(k)];
                double v = static_cast<double>(spec.clamp(static_cast<float>(
                    decodeValue(map.ordinals[static_cast<std::size_t>(k)], spec, lw.value22))));
                if (integral(spec))
                    v = std::round(v);
                if (l == nullptr || (l->mask >> slot & 1u) == 0 ||
                    static_cast<float>(v) != l->values[static_cast<std::size_t>(slot)])
                    continue;
            }
            else if (! lw.empty && k >= 23)
            {
                const float v = static_cast<float>(std::clamp(static_cast<double>(lw.value22) / kValueFull, 0.0, 1.0));
                if (l == nullptr || (l->mask >> k & 1u) == 0 || v != l->values[static_cast<std::size_t>(k)])
                    continue;
            }
            r.slots[static_cast<std::size_t>(k)] = lw;
        }
        for (const auto& [k, v] : raw.vct)
            if (k >= 0 && k < kLockWords && ! r.slots[static_cast<std::size_t>(k)].empty)
            {
                auto& w = r.slots[static_cast<std::size_t>(k)];
                w.vct = static_cast<std::uint8_t>(v & 3);
                w = LockWord::unpack(w.pack());
            }
    }
    return out;
}
} // namespace services::interop::sp16
