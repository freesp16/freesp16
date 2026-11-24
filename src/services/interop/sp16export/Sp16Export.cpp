#include "services/interop/sp16export/Sp16Export.h"

#include "core/commands/sequencer/SequencerCommands.h"
#include "core/model/ModelOps.h"
#include "core/model/enums/EnumStrings.h"
#include "core/params/ParamRegistry.h"
#include "services/ext/ExtFlags.h"
#include "services/interop/sp16/Sp16Import.h"
#include "services/interop/sp16/Sp16ProjectCodec.h"
#include "services/interop/sp16/StepMatrix.h"
#include "services/library/AudioFile.h"
#include "services/library/Decoder.h"
#include "services/render/AudioWriter.h"


#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>

namespace services::interop::sp16
{
namespace
{
namespace m = ::core::model;
using T = Var::T;

constexpr const char* kRootFolder = "PIONEER DJ SAMPLER"; // research storage.md §1
constexpr const char* kDiskRootNames[] = {"PIONEER DJ SAMPLER", "TORAIZ"};

const m::Project& defaults()
{
    static const auto p = [] {
        auto d = std::make_unique<m::Project>();
        m::resetToDefaults(*d);
        return d;
    }();
    return *p;
}

std::string where(int scene, int track = -1)
{
    std::string s = "scene " + std::to_string(scene + 1);
    if (track >= 0)
        s += " track " + std::to_string(track + 1);
    return s;
}

// ------------------------------------------------------------------------------- typed JSON (varToJson) → Var
// The importer's Extras values are varToJson text ({"int":5}, {"string":"x"}, {"array":[…]}, …). A small parser for
// exactly that form; numbers through strtod (correctly rounded: %.17g text gives back the same double).
class JsonVar
{
public:
    explicit JsonVar(std::string_view s) : s_(s) {}
    std::optional<Var> parse()
    {
        auto v = value();
        ws();
        if (! v || i_ != s_.size())
            return std::nullopt;
        return v;
    }

private:
    void ws()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\t' || s_[i_] == '\r'))
            ++i_;
    }
    bool eat(char c)
    {
        ws();
        if (i_ < s_.size() && s_[i_] == c)
        {
            ++i_;
            return true;
        }
        return false;
    }
    std::optional<std::string> str()
    {
        if (! eat('"'))
            return std::nullopt;
        std::string out;
        while (i_ < s_.size() && s_[i_] != '"')
        {
            char c = s_[i_++];
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                return std::nullopt;
            c = s_[i_++];
            switch (c)
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    if (i_ + 4 > s_.size())
                        return std::nullopt;
                    const unsigned long cp = std::strtoul(std::string(s_.substr(i_, 4)).c_str(), nullptr, 16);
                    i_ += 4;
                    if (cp < 0x80)
                        out += static_cast<char>(cp);
                    else if (cp < 0x800)
                    {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    else
                    {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += c; break; // \" \\ \/
            }
        }
        if (i_ >= s_.size())
            return std::nullopt;
        ++i_;
        return out;
    }
    std::optional<std::string> numberText()
    {
        ws();
        const std::size_t b = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' ||
                                  s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
            ++i_;
        if (i_ == b)
            return std::nullopt;
        return std::string(s_.substr(b, i_ - b));
    }
    bool literal(std::string_view word)
    {
        ws();
        if (s_.substr(i_, word.size()) != word)
            return false;
        i_ += word.size();
        return true;
    }
    std::optional<Var> value()
    {
        if (! eat('{'))
            return std::nullopt;
        auto key = str();
        if (! key || ! eat(':'))
            return std::nullopt;
        Var v;
        const std::string& k = *key;
        if (k == "void" || k == "undefined")
        {
            if (! literal("null"))
                return std::nullopt;
            v.t = k == "void" ? T::Void : T::Undefined;
        }
        else if (k == "int")
        {
            auto n = numberText();
            if (! n)
                return std::nullopt;
            v.t = T::Int;
            v.i = std::strtoll(n->c_str(), nullptr, 10);
        }
        else if (k == "int64")
        {
            auto n = str();
            if (! n)
                return std::nullopt;
            v.t = T::Int64;
            v.i = std::strtoll(n->c_str(), nullptr, 10);
        }
        else if (k == "bool")
        {
            if (literal("true"))
                v.t = T::True;
            else if (literal("false"))
                v.t = T::False;
            else
                return std::nullopt;
        }
        else if (k == "double")
        {
            ws();
            std::optional<std::string> n = i_ < s_.size() && s_[i_] == '"' ? str() : numberText();
            if (! n)
                return std::nullopt;
            v.t = T::Double;
            v.d = std::strtod(n->c_str(), nullptr); // also "inf" / "nan"
        }
        else if (k == "string" || k == "rawString")
        {
            auto s = str();
            if (! s)
                return std::nullopt;
            v.t = T::String;
            v.s = std::move(*s);
            v.unterminated = k == "rawString";
        }
        else if (k == "binary")
        {
            auto s = str();
            if (! s)
                return std::nullopt;
            auto bytes = fromBase64(*s);
            if (! bytes)
                return std::nullopt;
            v.t = T::Binary;
            v.bin = std::move(*bytes);
        }
        else if (k == "array")
        {
            if (! eat('['))
                return std::nullopt;
            v.t = T::Array;
            if (! eat(']'))
            {
                do
                {
                    auto e = value();
                    if (! e)
                        return std::nullopt;
                    v.a.push_back(std::move(*e));
                } while (eat(','));
                if (! eat(']'))
                    return std::nullopt;
            }
        }
        else
            return std::nullopt;
        if (! eat('}'))
            return std::nullopt;
        return v;
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

// ------------------------------------------------------------------------------------------------ var helpers
Var I(std::int64_t v)
{
    return v >= std::numeric_limits<std::int32_t>::min() && v <= std::numeric_limits<std::int32_t>::max()
               ? Var::ofInt(static_cast<std::int32_t>(v))
               : Var::ofInt64(v);
}
Var B(bool v) { return Var::ofBool(v); }
Var D(double v) { return Var::ofDouble(v); }
Var S(std::string_view v) { return Var::ofString(std::string(v)); }
Var I01(bool v) { return Var::ofInt(v ? 1 : 0); }
std::int32_t packed(int hi, int lo) { return (hi << 16) | lo; }

bool isInt(const Var& v) noexcept { return v.t == T::Int || v.t == T::Int64; }
bool isIntegral(const Var& v) noexcept
{
    return isInt(v) || (v.t == T::Double && std::isfinite(v.d) && std::nearbyint(v.d) == v.d && std::fabs(v.d) < 9e15);
}
std::int64_t intOf(const Var& v) noexcept { return v.t == T::Double ? static_cast<std::int64_t>(v.d) : v.i; }

// The importer's packed hi16/lo16 reading (Props::packedRatio).
std::optional<std::pair<int, int>> packedRatio(const Var& v) noexcept
{
    if (! isInt(v) || v.i <= 0xFFFF || v.i > 0x7FFFFFFF)
        return std::nullopt;
    const int hi = static_cast<int>(v.i >> 16), lo = static_cast<int>(v.i & 0xFFFF);
    if (hi <= 0 || lo <= 0)
        return std::nullopt;
    return std::make_pair(hi, lo);
}

// What Props::intIn would make of `kept` for a field with range [lo, hi], or `def` when it would not take it.
template <typename Int>
bool sameInt(const Var& kept, Int field, std::int64_t lo, std::int64_t hi, Int def)
{
    if (kept.t == T::Void || ! isIntegral(kept))
        return field == def;
    return static_cast<std::int64_t>(field) == std::clamp(intOf(kept), lo, hi);
}
// Props::intOrNone: a stored −1 ("none") leaves the model's default.
template <typename Int>
bool sameIntOrNone(const Var& kept, Int field, std::int64_t lo, std::int64_t hi, Int def)
{
    if (isInt(kept) && kept.i == -1)
        return field == def;
    return sameInt(kept, field, lo, hi, def);
}
bool sameDouble(const Var& kept, double field, double lo, double hi, double def)
{
    if (! kept.isNumber() || ! std::isfinite(kept.number()))
        return field == def;
    return field == std::clamp(kept.number(), lo, hi);
}
template <typename E>
bool sameEnum(const Var& kept, E field, E def, std::string_view prefix = {})
{
    if (kept.t == T::String)
    {
        std::string_view s = kept.s;
        if (auto e = m::enumFromStored<E>(s))
            return field == *e;
        if (! prefix.empty() && s.substr(0, prefix.size()) == prefix)
            if (auto e = m::enumFromStored<E>(s.substr(prefix.size())))
                return field == *e;
    }
    return field == def; // an unmapped string / a type mismatch: the import left the default
}

// ------------------------------------------------------------------------------------------- node builder
// One node's properties: the model's value, or the value the import kept when the model still holds what the import
// made of it (`same`), then every other kept property of this node ("sp16.<base>.<prop>", unknown to the mapper) in
// the order the import kept them.
class Props
{
public:
    Props(Node& n, std::string base, const m::Extras* extras, ExportReport& report, std::string at)
        : n_(n), base_(std::move(base)), extras_(extras), report_(report), at_(std::move(at))
    {
    }
    ~Props() { finish(); }
    Props(const Props&) = delete;
    Props& operator= (const Props&) = delete;

    std::optional<Var> kept(std::string_view name)
    {
        if (extras_ == nullptr)
            return std::nullopt;
        const auto* e = extras_->find(key(name));
        if (e == nullptr)
            return std::nullopt;
        if (e->encoding == m::Extras::Encoding::Text)
            return Var::ofString(e->value);
        if (e->encoding == m::Extras::Encoding::Json)
            return JsonVar(e->value).parse();
        return std::nullopt;
    }
    void put(std::string_view name, Var v)
    {
        used_.insert(std::string(name));
        n_.props.emplace_back(std::string(name), std::move(v));
    }
    template <typename Same>
    void put(std::string_view name, Var model, Same same)
    {
        if (auto k = kept(name); k && same(*k))
            put(name, std::move(*k));
        else
            put(name, std::move(model));
    }
    void ignore(std::string_view name) { used_.insert(std::string(name)); }
    void noUnknowns() { unknowns_ = false; }

    void finish()
    {
        if (done_)
            return;
        done_ = true;
        if (extras_ == nullptr || ! unknowns_)
            return;
        const std::string prefix = "sp16." + base_ + ".";
        for (const auto& e : extras_->entries)
        {
            if (e.key.compare(0, prefix.size(), prefix) != 0)
                continue;
            const std::string name = e.key.substr(prefix.size());
            if (name.empty() || used_.count(name) != 0)
                continue;
            used_.insert(name);
            if (e.encoding == m::Extras::Encoding::Text)
                n_.props.emplace_back(name, Var::ofString(e.value));
            else if (e.encoding == m::Extras::Encoding::Json)
            {
                if (auto v = JsonVar(e.value).parse())
                    n_.props.emplace_back(name, std::move(*v));
                else
                    report_.drop("unreadable kept value " + e.key, at_);
            }
            else
                report_.drop("kept value " + e.key + " (not a property)", at_);
        }
    }

private:
    std::string key(std::string_view name) const { return "sp16." + base_ + "." + std::string(name); }

    Node& n_;
    std::string base_;
    const m::Extras* extras_;
    ExportReport& report_;
    std::string at_;
    std::set<std::string> used_;
    bool unknowns_ = true, done_ = false;
};

// Unknown subtrees the import kept ("sp16.node.<type>", base64 tree bytes) back as children; foreign extras (native
// codec, extensions) reported as dropped.
void restoreNodes(Node& n, const m::Extras& extras, ExportReport& report, const std::string& at)
{
    constexpr std::string_view kNode = "sp16.node.";
    for (const auto& e : extras.entries)
    {
        if (e.key.compare(0, kNode.size(), kNode) == 0 && e.encoding == m::Extras::Encoding::Base64)
        {
            auto bytes = fromBase64(e.value);
            auto tree = bytes ? decodeTree(*bytes) : Result<Node>::failure("base64");
            if (tree)
                n.children.push_back(std::move(tree.value()));
            else
                report.drop("unreadable kept node " + e.key, at);
        }
        else if (e.key.compare(0, 5, "sp16.") != 0)
            report.drop(e.key.compare(0, 11, "native.ext.") == 0
                            ? "extension data " + e.key.substr(7) + " (not importable by a reference device)"
                            : "data the format cannot hold: " + e.key,
                        at);
    }
}

std::string urlFor(const m::Project& p, const m::SampleKey& key, const std::map<std::string, ExportSample>& samples)
{
    if (key.empty())
        return {};
    if (auto it = samples.find(key.id); it != samples.end())
        return it->second.url;
    (void) p;
    return {};
}

// ---------------------------------------------------------------------------------------------- node writers
struct Ctx
{
    const m::Project& project;
    const ParamIndexMap& map;
    ExportReport& report;
    std::map<std::string, ExportSample> samples; // by pool id
    // An unused pattern's step matrix (the usual case: 16 × 16 patterns, a few used): deflated once (155 KB
    // at zlib level 6 per pattern is the export's main cost on the A9).
    std::vector<StepRecord> emptyRecords;
    std::vector<std::byte> emptyBlob;
};

std::string stretchCode(m::StretchMode s)
{
    switch (s)
    {
        case m::StretchMode::Off: return "Off";
        case m::StretchMode::MasterTempo: return "MT"; // observed
        case m::StretchMode::Resample: return "RS";    // by analogy [G] (the importer's reading)
    }
    return "Off";
}

// Inverse of the importer's optionIndex (Sp16Import.cpp): the option string of type1 / type2, or nullopt when the
// effect has no option string family for it.
std::optional<std::string> optionString(m::EffectType t, int which, int index)
{
    using E = m::EffectType;
    auto pick = [&](std::initializer_list<std::string_view> names) -> std::optional<std::string> {
        if (index < 0 || index >= static_cast<int>(names.size()))
            return std::nullopt;
        return std::string(*(names.begin() + index));
    };
    switch (t)
    {
        case E::MultiModeFilter:
            return which == 1 ? pick({"FilterTypeLowPass", "FilterTypeHighPass", "FilterTypeBandPass", "FilterTypeOff"})
                              : pick({"FilterOrder2", "FilterOrder4"});
        case E::LoFi:
            return which == 1 ? pick({"FilterTypeHighPass", "FilterTypeLowPass", "FilterTypeBandPass", "FilterTypeOff"})
                              : pick({"FilterPositionPre", "FilterPositionPost"});
        case E::ParametricEq2:
            return pick({"FilterTypeHighPass", "FilterTypeLowPass", "FilterTypePeaking", "FilterTypeHighShelving",
                         "FilterTypeLowShelving"});
        case E::Ducker:
            if (which == 1 && index >= 0 && index < 16)
                return "SourceTrack" + std::to_string(index + 1);
            return std::nullopt;
        default: return std::nullopt;
    }
}
bool hasOptions(m::EffectType t, int which)
{
    using E = m::EffectType;
    return t == E::MultiModeFilter || t == E::LoFi || t == E::ParametricEq2 || (t == E::Ducker && which == 1);
}

void writeEffect(Node& n, std::string_view base, const m::EffectSlot& in, Ctx& c, const std::string& at)
{
    // Our own effects have no SP-16 type: the slot is written as Off (its parameters stay as stored).
    m::EffectSlot fx = in;
    if (fx.type == m::EffectType::VintagePitch)
    {
        c.report.drop("VINTAGE PITCH (not an SP-16 effect: written as Off)", at);
        fx.type = m::EffectType::Off;
    }
    Props p(n, std::string(base), &fx.extras, c.report, at);
    p.put("effectType", S("FxType_" + std::string(m::storedName(fx.type))),
          [&](const Var& k) { return sameEnum<m::EffectType>(k, fx.type, m::EffectType::Off, "FxType_"); });
    std::vector<Var> params;
    const auto keptParams = p.kept("params");
    for (std::size_t k = 0; k < 6; ++k)
    {
        std::string name = "None";
        if (keptParams && keptParams->t == T::Array && keptParams->a.size() == 6 && keptParams->a[k].t == T::Array &&
            keptParams->a[k].a.size() == 2 && keptParams->a[k].a[0].t == T::String)
            name = keptParams->a[k].a[0].s; // OQ-05: the slot names the import kept
        params.push_back(Var::ofArray({S(name), D(fx.params[k])}));
    }
    p.put("params", Var::ofArray(std::move(params)));
    p.put("bSync", B(fx.sync));
    // A Send/Master flag the import kept (FX-046: loaded off) goes back as stored while the slot stays unbypassed.
    p.put("bBypass", B(fx.bypass), [&](const Var& k) { return ! fx.bypass && k.t == T::True; });
    for (int which = 1; which <= 2; ++which)
    {
        const int field = which == 1 ? fx.type1 : fx.type2;
        const char* name = which == 1 ? "bType1" : "bType2";
        const auto s = optionString(fx.type, which, field);
        if (! s && field != 0)
            c.report.drop("effect option values of " + std::string(m::storedName(fx.type)) + " (no reference string)",
                          at);
        // A string the import kept (an option of another type, carried over) stays while the field is the default.
        p.put(name, S(s ? *s : "FilterTypeOff"), [&](const Var& k) {
            return k.t == T::String && field == 0 && (! hasOptions(fx.type, which) || ! s || k.s != *s);
        });
    }
    if (fx.lookAhead)
        p.put("bLookAhead", B(true));
    if (! fx.typeMemory.empty())
        c.report.drop("effect type memory (FX-007)", at);
    p.finish();
    restoreNodes(n, fx.extras, c.report, at);
}

Node trackNode(const m::Track& tr, const m::Track& def, Ctx& c, int scene, int t)
{
    const std::string at = where(scene, t);
    Node n;
    n.type = "TrackData" + std::to_string(t);
    {
        Props p(n, "TrackData", &tr.extras, c.report, at);
        const auto exportMode = tr.mode == m::TrackMode::Plugin ? m::TrackMode::Sample : tr.mode;
        if (tr.mode == m::TrackMode::Plugin)
            c.report.drop("plugin track type", at);
        p.put("trackMode", S(m::storedName(exportMode)),
              [&](const Var& k) { return sameEnum<m::TrackMode>(k, exportMode, def.mode, "TrackMode_"); });
        p.put("triggerMode", S(m::storedName(tr.triggerMode)), [&](const Var& k) {
            return sameEnum<m::TriggerMode>(k, tr.triggerMode, def.triggerMode, "TriggerMode_");
        });
        p.put("chokeGroup", I(tr.chokeGroup == 0 ? -1 : tr.chokeGroup),
              [&](const Var& k) { return sameIntOrNone(k, tr.chokeGroup, 0, m::kChokeGroups, def.chokeGroup); });
        p.put("volume", I(tr.volume), [&](const Var& k) { return sameInt(k, tr.volume, 0, 127, def.volume); });
        p.put("pan", I(tr.pan), [&](const Var& k) { return sameInt(k, tr.pan, -64, 64, def.pan); });
        p.put("soloSetting", B(tr.solo));
        p.put("muteSetting", B(tr.mute));
        p.put("colourIndex", I(tr.colour),
              [&](const Var& k) { return sameIntOrNone(k, tr.colour, 0, m::kPaletteColours - 1, def.colour); });
        p.put("instrument", tr.instrument == 0 ? S("Off") : I(tr.instrument),
              [&](const Var& k) { return k.t == T::String && k.s != "Off" && tr.instrument == def.instrument; });
        if (! tr.name.empty())
            c.report.drop("track names", at);
        if (! tr.plugin.path.empty() || ! tr.plugin.pluginId.empty())
            c.report.drop("plugin assignment", at);
    }
    // PlaybackData
    {
        const auto& pb = tr.playback;
        const auto& d = def.playback;
        Node ch;
        ch.type = "PlaybackData";
        {
            Props p(ch, "PlaybackData", &pb.extras, c.report, at);
            constexpr std::int64_t kMax = std::int64_t {1} << 40;
            p.put("startSample", I(pb.start), [&](const Var& k) { return sameInt(k, pb.start, 0, kMax, d.start); });
            p.put("loopStartSample", I(pb.loopStart),
                  [&](const Var& k) { return sameInt(k, pb.loopStart, 0, kMax, d.loopStart); });
            p.put("lengthSample", I(pb.length), [&](const Var& k) { return sameInt(k, pb.length, 0, kMax, d.length); });
            p.put("bLoop", B(pb.loop));
            p.put("bReverse", B(pb.reverse));
            p.put("pitchCent", I(pb.pitchCents),
                  [&](const Var& k) { return sameInt(k, pb.pitchCents, -9600, 9600, d.pitchCents); });
            p.put("stretchMode", S(stretchCode(pb.stretchMode)), [&](const Var& k) {
                return k.t == T::String && k.s != "MT" && k.s != "RS" && k.s != "Off" &&
                       sameEnum<m::StretchMode>(k, pb.stretchMode, d.stretchMode, "StretchMode_");
            });
            p.put("audioSourceUrl", S(urlFor(c.project, pb.sample, c.samples)));
            p.put("bBypass", B(pb.bypass));
        }
        restoreNodes(ch, pb.extras, c.report, at);
        n.children.push_back(std::move(ch));
    }
    // AmpEnvelopeData
    {
        const auto& e = tr.ampEnv;
        const auto& d = def.ampEnv;
        Node ch;
        ch.type = "AmpEnvelopeData";
        Props p(ch, "AmpEnvelopeData", &e.extras, c.report, at);
        // The model's k/128 → the reference's k/127 (Sp16Mapping.h); a kept original that reads back as the same
        // model value is written verbatim.
        auto env = [&](const char* name, double field, double def, core::ampenv::EnvStage stage) {
            bool lossy = false;
            const auto index = core::ampenv::indexFromStored(static_cast<float>(field));
            const double ref = envReferenceFromIndex(stage, stage == core::ampenv::EnvStage::Attack
                                                                ? std::min(index, core::ampenv::kMaxFiniteIndex)
                                                                : index,
                                                     &lossy);
            if (lossy)
                c.report.drop("HOLD / RELEASE 127 (written as 126: the format's 127 is INFINITY)", at);
            p.put(name, D(ref), [&](const Var& k) {
                if (! k.isNumber() || ! std::isfinite(k.number()))
                    return field == def;
                return static_cast<double>(core::ampenv::storedFromIndex(
                           envIndexFromReference(stage, std::clamp(k.number(), 0.0, 1.0)))) == field;
            });
        };
        env("attackNorm", e.attack, d.attack, core::ampenv::EnvStage::Attack);
        env("holdNorm", e.hold, d.hold, core::ampenv::EnvStage::Hold);
        env("releaseNorm", e.release, d.release, core::ampenv::EnvStage::Release);
        p.put("volumeIndex", I(e.volumeIndex),
              [&](const Var& k) { return sameInt(k, e.volumeIndex, 0, 127, d.volumeIndex); });
        p.put("bSync", B(e.sync));
        p.put("bVelocityEffective", B(e.velocitySensitive));
        p.put("bRetriggerMode", B(e.retrigger));
        p.put("attackCurveType", S(m::storedName(e.attackCurve)), [&](const Var& k) {
            return sameEnum<m::EnvelopeCurve>(k, e.attackCurve, d.attackCurve, "EnvelopeCurve_");
        });
        p.put("releaseCurveType", S(m::storedName(e.releaseCurve)), [&](const Var& k) {
            return sameEnum<m::EnvelopeCurve>(k, e.releaseCurve, d.releaseCurve, "EnvelopeCurve_");
        });
        p.put("bBypass", B(e.bypass));
        p.finish();
        n.children.push_back(std::move(ch));
    }
    // SequencerData
    {
        const auto& s = tr.seq;
        const auto& d = def.seq;
        Node ch;
        ch.type = "SequencerData";
        Props p(ch, "SequencerData", &s.extras, c.report, at);
        p.put("retriggerCount", I(s.retriggerCount),
              [&](const Var& k) { return sameInt(k, s.retriggerCount, 0, m::kMaxRetriggerCount, d.retriggerCount); });
        int lo = 1;
        if (auto e = m::enumFromInt<m::RetriggerSpeed>(s.retriggerSpeed))
            lo = std::max(1, static_cast<int>(std::lround(1.0 / m::retriggerStepFraction(*e))));
        p.put("retriggerSpeed", I(packed(1, lo)), [&](const Var& k) {
            if (auto r = packedRatio(k))
            {
                for (int i = 0; i < static_cast<int>(m::enumCount<m::RetriggerSpeed>()); ++i)
                    if (std::fabs(m::retriggerStepFraction(static_cast<m::RetriggerSpeed>(i)) -
                                  static_cast<double>(r->first) / r->second) < 1e-9)
                        return s.retriggerSpeed == i;
                return s.retriggerSpeed == d.retriggerSpeed;
            }
            return false; // a plain int: written from the model
        });
        p.put("offset", I(s.offset), [&](const Var& k) { return sameInt(k, s.offset, -24, 24, d.offset); });
        p.put("stepShiftArea", I(s.stepShiftArea), [&](const Var& k) {
            return isInt(k) && k.i == -1 ? s.stepShiftArea == d.stepShiftArea : false;
        });
        p.put("transpose", I(s.transpose), [&](const Var& k) { return sameInt(k, s.transpose, -48, 48, d.transpose); });
        p.finish();
        n.children.push_back(std::move(ch));
    }
    // SliceData
    {
        const auto& s = tr.slices;
        const auto& d = def.slices;
        Node ch;
        ch.type = "SliceData";
        Props p(ch, "SliceData", &s.extras, c.report, at);
        std::vector<Var> grids;
        for (const auto g : s.grids)
            grids.push_back(I(g));
        p.put("grids", Var::ofArray(std::move(grids)), [&](const Var& k) {
            // The kept original (layout unknown, OQ-04) stays while the model holds what the import read from it.
            if (k.t != T::Array)
                return s.grids == d.grids;
            std::vector<std::int64_t> pairs;
            for (const auto& e : k.a)
            {
                if (! isInt(e))
                    return s.grids == d.grids;
                pairs.push_back(e.i);
            }
            return s.grids == pairs || s.grids == d.grids;
        });
        p.put("nGrids", I(s.gridCount), [&](const Var& k) { return sameInt(k, s.gridCount, 0, 64, d.gridCount); });
        p.put("windowOffset", I(s.windowOffset),
              [&](const Var& k) { return sameInt(k, s.windowOffset, -100000, 100000, d.windowOffset); });
        p.finish();
        n.children.push_back(std::move(ch));
    }
    // ScaleSetting, ScaleSettingMidi
    {
        const auto& s = tr.padScale;
        Node ch;
        ch.type = "ScaleSetting";
        Props p(ch, "ScaleSetting", &s.extras, c.report, at);
        const auto st = m::enumFromInt<m::ScaleType>(s.scaleType).value_or(m::ScaleType {});
        p.put("scaleType", S(m::storedName(st)),
              [&](const Var& k) { return k.t != T::Int && s.scaleType == def.padScale.scaleType; });
        p.put("keyShift", I(s.keyShift),
              [&](const Var& k) { return sameInt(k, s.keyShift, -127, 127, def.padScale.keyShift); });
        p.put("windowOffset", I(s.windowOffset),
              [&](const Var& k) { return sameInt(k, s.windowOffset, -100000, 100000, def.padScale.windowOffset); });
        p.finish();
        n.children.push_back(std::move(ch));
    }
    {
        const auto& s = tr.midiScale;
        Node ch;
        ch.type = "ScaleSettingMidi";
        Props p(ch, "ScaleSettingMidi", &s.extras, c.report, at);
        const auto st = m::enumFromInt<m::ScaleType>(s.scaleTypeMidi).value_or(m::ScaleType {});
        p.put("scaleTypeMidi", S(m::storedName(st)),
              [&](const Var& k) { return k.t != T::Int && s.scaleTypeMidi == def.midiScale.scaleTypeMidi; });
        p.put("scalePosition", I(s.scalePosition), [&](const Var& k) {
            return sameInt(k, s.scalePosition, -100000, 100000, def.midiScale.scalePosition);
        });
        p.put("rootNote", I(s.rootNote), [&](const Var& k) { return sameInt(k, s.rootNote, 0, 127, def.midiScale.rootNote); });
        p.finish();
        n.children.push_back(std::move(ch));
    }
    for (int k = 0; k < 2; ++k)
    {
        Node ch;
        ch.type = "InsertFx" + std::to_string(k);
        writeEffect(ch, "InsertFx", tr.insertFx[static_cast<std::size_t>(k)], c, at);
        n.children.push_back(std::move(ch));
    }
    {
        Node ch;
        ch.type = "SendData";
        Props p(ch, "SendData", &tr.send.extras, c.report, at);
        p.put("sendChannel", I(tr.send.channel),
              [&](const Var& k) { return sameInt(k, tr.send.channel, 0, 255, def.send.channel); });
        p.put("sendAmount", I(tr.send.amount),
              [&](const Var& k) { return sameInt(k, tr.send.amount, 0, 127, def.send.amount); });
        p.finish();
        n.children.push_back(std::move(ch));
    }
    // LfoData
    {
        const auto& l = tr.lfo;
        const auto& d = def.lfo;
        Node ch;
        ch.type = "LfoData";
        Props p(ch, "LfoData", &l.extras, c.report, at);
        p.put("speed", D(l.speed), [&](const Var& k) { return sameDouble(k, l.speed, -1e9, 1e9, d.speed); });
        p.put("depth", I(l.depth), [&](const Var& k) { return sameInt(k, l.depth, -100000, 100000, d.depth); });
        p.put("phaseOffset", I(l.phaseOffset),
              [&](const Var& k) { return sameInt(k, l.phaseOffset, -100000, 100000, d.phaseOffset); });
        p.put("destination", I(l.destination),
              [&](const Var& k) { return sameInt(k, l.destination, -100000, 100000, d.destination); });
        p.put("type", S(m::storedName(l.shape)),
              [&](const Var& k) { return sameEnum<m::LfoShape>(k, l.shape, d.shape, "LfoType_"); });
        p.put("triggerMode", S(m::storedName(l.triggerMode)), [&](const Var& k) {
            return sameEnum<m::LfoTriggerMode>(k, l.triggerMode, d.triggerMode, "LfoTriggerMode_");
        });
        // 1.60 strings FULL / FULL & HALF for the model's nearest values (the importer's reading).
        std::string on = l.triggerOn == m::LfoTriggerOn::Step   ? "Full"
                         : l.triggerOn == m::LfoTriggerOn::Both ? "FullAndHalf"
                                                                : std::string(m::storedName(l.triggerOn));
        p.put("triggerOn", S(on), [&](const Var& k) {
            if (k.t != T::String)
                return false;
            if (k.s == "Full")
                return l.triggerOn == m::LfoTriggerOn::Step;
            if (k.s == "FullAndHalf" || k.s == "FullHalf")
                return l.triggerOn == m::LfoTriggerOn::Both;
            return sameEnum<m::LfoTriggerOn>(k, l.triggerOn, d.triggerOn, "LfoTriggerOn_");
        });
        p.put("sync", B(l.sync));
        p.put("randStartNum", I(l.randomSeedCount),
              [&](const Var& k) { return sameInt(k, l.randomSeedCount, 0, 127, d.randomSeedCount); });
        p.put("bypass", B(l.bypass));
        p.finish();
        n.children.push_back(std::move(ch));
    }
    // MidiTrackData
    {
        const auto& mt = tr.midi;
        const auto& d = def.midi;
        Node ch;
        ch.type = "MidiTrackData";
        {
            Props p(ch, "MidiTrackData", &mt.extras, c.report, at);
        }
        {
            Node g;
            g.type = "MidiNoteData";
            Props p(g, "MidiNoteData", &mt.extras, c.report, at);
            for (int k = 0; k < 4; ++k)
            {
                const std::string name = "note" + std::to_string(k + 1);
                // note1 the base note; note2–4 semitones from it, 0 = off (the model's own encoding).
                const int note = mt.notes[static_cast<std::size_t>(k)];
                p.put(name, I(note), [&](const Var& kv) {
                    return sameInt(kv, note, k == 0 ? 0 : -127, 127, d.notes[static_cast<std::size_t>(k)]);
                });
            }
            p.put("velocity", I(mt.velocity), [&](const Var& k) { return sameInt(k, mt.velocity, 0, 127, d.velocity); });
            p.put("length", I(std::lround(mt.length * 4.0)), [&](const Var& k) {
                return isInt(k) && std::clamp(static_cast<double>(k.i) / 4.0, 0.25, 64.0) == mt.length;
            });
            if (mt.noteBypass)
                p.put("MidiBypassFlg", B(true));
            p.finish();
            ch.children.push_back(std::move(g));
        }
        for (int which = 1; which <= 2; ++which)
        {
            const bool one = which == 1;
            Node g;
            g.type = one ? "MidiCc1Data" : "MidiCc2Data";
            Props p(g, g.type, &mt.extras, c.report, at);
            p.put("MidiBypassFlg", I01(one ? mt.cc1Bypass : mt.cc2Bypass));
            if (one)
            {
                p.put("pitchBendValue", I(mt.pitchBendValue),
                      [&](const Var& k) { return sameInt(k, mt.pitchBendValue, -8192, 16383, d.pitchBendValue); });
                p.put("pitchBendActive", I01(mt.pitchBendActive));
                p.put("afterTouchValue", I(mt.aftertouchValue),
                      [&](const Var& k) { return sameInt(k, mt.aftertouchValue, 0, 127, d.aftertouchValue); });
                p.put("afterTouchActive", I01(mt.aftertouchActive));
            }
            const int first = one ? 3 : 1, count = one ? 4 : 6;
            for (int k = 0; k < count; ++k)
            {
                const auto& cc = one ? mt.cc1[static_cast<std::size_t>(k)] : mt.cc2[static_cast<std::size_t>(k)];
                const auto& dc = one ? d.cc1[static_cast<std::size_t>(k)] : d.cc2[static_cast<std::size_t>(k)];
                const std::string stem = "param" + std::to_string(first + k);
                p.put(stem + "CcNo", I(cc.cc), [&](const Var& kv) { return sameInt(kv, cc.cc, 0, 127, dc.cc); });
                p.put(stem + "Value", I(cc.value), [&](const Var& kv) { return sameInt(kv, cc.value, 0, 127, dc.value); });
                p.put(stem + "Active", I01(cc.active));
            }
            p.finish();
            ch.children.push_back(std::move(g));
        }
        {
            Node g;
            g.type = "MidiOtherData";
            Props p(g, "MidiOtherData", &mt.extras, c.report, at);
            p.put("bankselmsb", I(mt.bankMsb), [&](const Var& k) { return sameInt(k, mt.bankMsb, -1, 127, d.bankMsb); });
            p.put("banksellsb", I(mt.bankLsb), [&](const Var& k) { return sameInt(k, mt.bankLsb, -1, 127, d.bankLsb); });
            p.put("programchange", I(mt.program),
                  [&](const Var& k) { return sameInt(k, mt.program, -1, 127, d.program); });
            p.finish();
            ch.children.push_back(std::move(g));
        }
        {
            Node g;
            g.type = "MidiModeData";
            Props p(g, "MidiModeData", &mt.extras, c.report, at);
            p.put("midimode", I(mt.mode), [&](const Var& k) { return sameIntOrNone(k, mt.mode, 0, 255, d.mode); });
            p.finish();
            ch.children.push_back(std::move(g));
        }
        restoreNodes(ch, mt.extras, c.report, at);
        n.children.push_back(std::move(ch));
    }
    restoreNodes(n, tr.extras, c.report, at);
    return n;
}

Node patternNode(const m::Pattern& pat, const m::Scene& sc, Ctx& c, int scene, int k)
{
    const std::string at = where(scene) + " pattern " + std::to_string(k + 1);
    Node n;
    n.type = "PatternData" + std::to_string(k);
    Props p(n, "PatternData", &pat.extras, c.report, at);
    p.ignore("Steps");
    p.ignore("value2");
    if (pat.extras.find("sp16.PatternData.Steps") != nullptr)
        c.report.drop("step matrix kept verbatim at import (rebuilt from the model)", at);
    // Locks the format cannot carry: Hold / Glide changes (the vct bits' meaning is unknown) and app slots outside
    // the ordinal map.
    std::uint64_t exported = 0;
    for (const auto& o : c.map.ordinals)
        if (const auto* spec = o.param.empty() ? nullptr : core::builtinParams().find(o.param))
            if (const int slot = core::commands::sequencer::lockSlotOf(*spec); slot >= 0 && slot < 64)
                exported |= std::uint64_t {1} << slot;
    for (int s = 23; s < kLockWords; ++s)
        exported |= std::uint64_t {1} << s;
    for (const auto& l : pat.locks)
    {
        if ((l.mask & ~exported) != 0)
            c.report.drop("locks on parameters the lock-ordinal map does not export", at);
        for (int s = 0; s < m::kLockSlots; ++s)
            if ((l.mask >> s & 1u) != 0 && l.change[static_cast<std::size_t>(s)] != m::LockChange::Revert)
            {
                c.report.drop("lock HOLD / GLIDE changes (written as revert)", at);
                break;
            }
    }
    // FX lock index bytes follow the track's insert effects (Sp16Mapping.h fxLockIndex); a sub-slot without an index
    // for that effect cannot be written.
    FxLockTypes fxTypes {};
    for (int t = 0; t < 16; ++t)
        for (int u = 0; u < 2; ++u)
            fxTypes[static_cast<std::size_t>(t)][static_cast<std::size_t>(u)] =
                sc.tracks[static_cast<std::size_t>(t)].insertFx[static_cast<std::size_t>(u)].type;
    for (int s = 0; s < m::kSteps; ++s)
        for (int t = 0; t < 16; ++t)
        {
            const auto& cell = pat.steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t)];
            if (cell.lockIndex == m::Step::kNoLocks || cell.lockIndex >= pat.locks.size())
                continue;
            const auto& l = pat.locks[cell.lockIndex];
            for (int slot = 23; slot < kLockWords; ++slot)
                if ((l.mask >> slot & 1u) != 0 &&
                    fxLockIndex(fxTypes[static_cast<std::size_t>(t)][static_cast<std::size_t>((slot - 23) / 6)],
                                (slot - 23) % 6) < 0)
                    c.report.drop("FX locks on an effect parameter the format has no index for", at);
        }
    auto records = encodePattern(pat, c.map, &fxTypes);
    if (c.emptyRecords.empty())
    {
        c.emptyRecords.assign(static_cast<std::size_t>(kStepCount * kTrackCount), StepRecord::emptyRecord());
        c.emptyBlob = encodeSteps(c.emptyRecords);
    }
    p.put("Steps", Var::ofBinary(records == c.emptyRecords ? c.emptyBlob : encodeSteps(records)));
    p.put("Length", I(pat.length));
    std::vector<Var> mutes, solos;
    for (int t = 0; t < 16; ++t)
    {
        mutes.push_back(B(pat.trackMutes[static_cast<std::size_t>(t)]));
        solos.push_back(B(pat.trackSolos[static_cast<std::size_t>(t)]));
    }
    p.put("TrackMutes", Var::ofArray(std::move(mutes)));
    p.put("TrackSolos", Var::ofArray(std::move(solos)));
    // Children before the kept unknown properties: the import keeps nodes first, properties at the node's end.
    {
        Node tmp;
        restoreNodes(tmp, pat.extras, c.report, at);
        for (auto& ch : tmp.children)
            n.children.push_back(std::move(ch));
    }
    p.finish();
    return n;
}

Node routingNode(const m::Scene& sc, const m::Scene& def, Ctx& c, int scene)
{
    const std::string at = where(scene);
    const auto& r = sc.routing;
    const auto& s = c.project.settings;
    Node n;
    n.type = "RoutingData";
    {
        Props p(n, "RoutingData", &r.extras, c.report, at);
        p.put("assignMode", S(m::storedName(r.assignMode)), [&](const Var& k) {
            return sameEnum<m::AssignMode>(k, r.assignMode, def.routing.assignMode, "AssignMode_");
        });
        // The per-scene AFS copy (observed; the model holds the project's values only): kept, else the project's.
        p.put("bDaveSmithFilter", B(s.afsEnabled), [](const Var&) { return true; });
        p.put("daveSmithRouteMode", S("DaveSmithRouteMode_" + std::string(m::storedName(s.afsRouteMode))),
              [](const Var&) { return true; });
        const auto sendMode = m::enumFromInt<m::AfsSendMode>(s.afsSendMode).value_or(m::AfsSendMode::PreFx);
        p.put("daveSmithSendMode", S("DaveSmithSendMode_" + std::string(m::storedName(sendMode))),
              [](const Var&) { return true; });
        p.put("daveSmithSendTrack", I(s.afsSendTrack), [](const Var&) { return true; });
        p.put("effectMode", S("EffectMode" + std::string(m::storedName(r.fxMode))),
              [&](const Var& k) { return sameEnum<m::FxMode>(k, r.fxMode, def.routing.fxMode, "EffectMode"); });
        const auto cue = m::enumFromInt<m::HeadphoneCue>(r.headphoneSource).value_or(m::HeadphoneCue {});
        p.put("headphoneOutCh", S(m::storedName(cue)), [&](const Var& k) {
            return k.t != T::Int && r.headphoneSource == def.routing.headphoneSource;
        });
        if (r.afsAssignment != def.routing.afsAssignment)
            p.put("daveSmithAssignTrack", I(r.afsAssignment)); // [G] name (the importer's first guess)
        else
            p.ignore("daveSmithAssignTrack");
    }
    static constexpr const char* kAssign[] = {"AssignToNull", "AssignTo12", "AssignTo34", "AssignTo56", "AssignTo78",
                                              "AssignTo1",    "AssignTo2",  "AssignTo3",  "AssignTo4",  "AssignTo5",
                                              "AssignTo6",    "AssignTo7",  "AssignTo8"};
    for (int t = 0; t < 16; ++t)
    {
        const auto& tr = sc.tracks[static_cast<std::size_t>(t)].routing;
        Node ch;
        ch.type = "TrackRoutingData" + std::to_string(t);
        Props p(ch, "TrackRoutingData", &tr.extras, c.report, where(scene, t));
        const auto o = static_cast<std::size_t>(tr.output);
        p.put("assignableOutCh", S(o < std::size(kAssign) ? kAssign[o] : "AssignToNull"),
              [&](const Var& k) { return k.t == T::String && k.s.rfind("AssignTo", 0) != 0 && tr.output == m::OutputAssign::Master; });
        p.finish();
        n.children.push_back(std::move(ch));
    }
    restoreNodes(n, r.extras, c.report, at);
    return n;
}

Node sceneNode(const m::Scene& sc, Ctx& c, int s)
{
    const auto& def = defaults().scenes[static_cast<std::size_t>(s)];
    const std::string at = where(s);
    Node n;
    n.type = "SceneData" + std::to_string(s);
    {
        Props p(n, "SceneData", &sc.extras, c.report, at);
        p.put("colourIndex", I(sc.colour),
              [&](const Var& k) { return sameIntOrNone(k, sc.colour, 0, m::kPaletteColours - 1, def.colour); });
        p.put("name", S(sc.name), [&](const Var& k) { return k.t != T::String && sc.name == def.name; });
        p.put("bpm", D(sc.bpm ? *sc.bpm : 0.0), [&](const Var& k) {
            if (! k.isNumber())
                return ! sc.bpm.has_value();
            const double v = k.number();
            return sc.bpm.has_value() && std::isfinite(v) && *sc.bpm == std::clamp(v, m::kMinTempo, m::kMaxTempo);
        });
    }
    for (int t = 0; t < 16; ++t)
        n.children.push_back(trackNode(sc.tracks[static_cast<std::size_t>(t)], def.tracks[static_cast<std::size_t>(t)],
                                       c, s, t));
    for (int k = 0; k < 16; ++k)
    {
        n.children.push_back(patternNode(sc.patterns[static_cast<std::size_t>(k)], sc, c, s, k));
        ++c.report.patterns;
    }
    {
        Node send;
        send.type = "SendFx";
        writeEffect(send, "SendFx", sc.sendFx, c, at);
        n.children.push_back(std::move(send));
        Node master;
        master.type = "MasterFx";
        writeEffect(master, "MasterFx", sc.masterFx, c, at);
        n.children.push_back(std::move(master));
    }
    n.children.push_back(routingNode(sc, def, c, s));
    restoreNodes(n, sc.extras, c.report, at);
    ++c.report.scenes;
    return n;
}

Node arrangementNode(const m::Arrangement& arr, Ctx& c)
{
    Node n;
    n.type = "ArrangementData0";
    Props p(n, "ArrangementData", &arr.extras, c.report, "arrangement");
    if (! arr.sections.empty())
        c.report.drop("arrangement rows (the reference's section record is unknown, OQ-02)", "arrangement");
    p.put("length", I(1), [](const Var& k) { return isInt(k); });
    std::optional<Var> sections;
    if (auto k = p.kept("Sections"); k && k->t == T::Binary)
        sections = std::move(*k);
    p.put("Sections", sections ? std::move(*sections)
                               : Var::ofBinary(encodeSections(std::vector<SectionRecord>(kSectionCount))));
    p.finish();
    restoreNodes(n, arr.extras, c.report, "arrangement");
    return n;
}

Node ribbonNode(const m::RibbonSettings& rib, Ctx& c)
{
    Node n;
    n.type = "RibbonData";
    {
        Props p(n, "RibbonData", &rib.extras, c.report, "ribbon");
    }
    for (int u = 0; u < 2; ++u)
    {
        Node us;
        us.type = "UserSetting" + std::to_string(u);
        {
            Props p(us, "UserSetting", &rib.extras, c.report, "ribbon");
            if (u != 0)
                p.noUnknowns(); // the import keeps both users' unknown properties under one key
        }
        for (int i = 0; i < 8; ++i)
        {
            const auto& item = rib.user[static_cast<std::size_t>(u)][static_cast<std::size_t>(i)];
            Node it;
            it.type = "Item" + std::to_string(i);
            Props p(it, "Item", &rib.extras, c.report, "ribbon");
            if (u != 0 || i != 0)
                p.noUnknowns();
            p.ignore("topValue");
            p.ignore("bottomValue");
            p.ignore("target");
            p.put("topValue", D(item.top));
            p.put("bottomValue", D(item.bottom));
            p.put("target", item.target.empty() ? I(255) : S(item.target));
            p.finish();
            us.children.push_back(std::move(it));
        }
        n.children.push_back(std::move(us));
    }
    {
        Node ps;
        ps.type = "PitchSetting";
        Props p(ps, "PitchSetting", &rib.extras, c.report, "ribbon pitch");
        p.put("topCent", I(rib.pitchTopCents));
        p.put("bottomCent", I(rib.pitchBottomCents));
        p.ignore("topValue");
        p.ignore("bottomValue");
        p.finish();
        n.children.push_back(std::move(ps));
    }
    restoreNodes(n, rib.extras, c.report, "ribbon");
    return n;
}

Node midiNode(const m::MidiSettings& midi, Ctx& c)
{
    Node n;
    n.type = "MidiSettingData";
    {
        Props p(n, "MidiSettingData", &midi.extras, c.report, "midi");
    }
    for (int k = 0; k < 2; ++k)
    {
        const auto& port = k == 0 ? midi.input : midi.output;
        Node ch;
        ch.type = k == 0 ? "Input" : "Output";
        Props p(ch, ch.type, &midi.extras, c.report, "midi " + ch.type);
        p.put("bUseClock", B(port.clock));
        p.put("bUseStartStop", B(port.startStop));
        p.put("bUseNoteCC", B(port.noteCc));
        p.put("bUseSongPosition", B(port.songPosition));
        std::vector<Var> channels;
        for (const int chn : port.channels)
            channels.push_back(I(std::clamp(chn, 1, 16) - 1));
        p.put("channelNo", Var::ofArray(std::move(channels)));
        p.finish();
        n.children.push_back(std::move(ch));
    }
    restoreNodes(n, midi.extras, c.report, "midi");
    return n;
}

void settingsProps(Node& root, const m::Project& project, const std::string& name, Ctx& c)
{
    const auto& s = project.settings;
    const auto& d = defaults().settings;
    Props p(root, "ProjectData", &s.extras, c.report, "project");
    p.put("masterVolume", I(s.masterVolume), [&](const Var& k) { return sameInt(k, s.masterVolume, 0, 127, d.masterVolume); });
    p.put("headphoneVolume", I(s.headphoneVolume),
          [&](const Var& k) { return sameInt(k, s.headphoneVolume, 0, 127, d.headphoneVolume); });
    p.put("sendTrackVolume", I(s.sendTrackVolume),
          [&](const Var& k) { return sameInt(k, s.sendTrackVolume, 0, 127, d.sendTrackVolume); });
    p.put("monitorVolume", I(s.monitorVolume),
          [&](const Var& k) { return sameInt(k, s.monitorVolume, 0, 127, d.monitorVolume); });
    p.put("bDaveSmithFilter", B(s.afsEnabled));
    p.put("daveSmithRouteMode", S("DaveSmithRouteMode_" + std::string(m::storedName(s.afsRouteMode))),
          [&](const Var& k) { return sameEnum<m::AfsRouteMode>(k, s.afsRouteMode, d.afsRouteMode, "DaveSmithRouteMode_"); });
    const auto sendMode = m::enumFromInt<m::AfsSendMode>(s.afsSendMode).value_or(m::AfsSendMode::PreFx);
    p.put("daveSmithSendMode", S("DaveSmithSendMode_" + std::string(m::storedName(sendMode))), [&](const Var& k) {
        if (k.t == T::String && (k.s == "DaveSmithSendMode_Pre" || k.s == "DaveSmithSendMode_Post"))
            return s.afsSendMode == (k.s == "DaveSmithSendMode_Pre" ? 0 : 1);
        return sameEnum<m::AfsSendMode>(k, sendMode, static_cast<m::AfsSendMode>(d.afsSendMode), "DaveSmithSendMode_");
    });
    p.put("daveSmithSendTrack", I(s.afsSendTrack),
          [&](const Var& k) { return sameInt(k, s.afsSendTrack, -1, 15, d.afsSendTrack); });
    p.put("bSoftLimitter", B(s.softLimiter));
    p.put("bPlayArrangement", B(s.playArrangement));
    // Observed: the shown 0–50 % as an int; the model stores 50–75 (shown = 2 × (stored − 50)).
    p.put("swingRate", I(std::clamp(2 * (s.swing - 50), 0, 50)), [&](const Var& k) {
        if (! k.isNumber() || ! std::isfinite(k.number()) || k.number() < 0 || k.number() > 50)
            return false;
        return s.swing == static_cast<int>(std::lround(50.0 + k.number() / 2.0));
    });
    p.put("projectBpm", D(s.tempo), [&](const Var& k) { return sameDouble(k, s.tempo, m::kMinTempo, m::kMaxTempo, d.tempo); });
    for (const auto* unit : {"patternQuantizeUnit", "padQuantizeUnit"})
    {
        const auto field = std::string_view(unit) == "patternQuantizeUnit" ? s.patternQuantize : s.padQuantize;
        const auto def = std::string_view(unit) == "patternQuantizeUnit" ? d.patternQuantize : d.padQuantize;
        Var model = I(0);
        switch (field)
        {
            case m::QuantizeUnit::Off: model = I(0); break;
            case m::QuantizeUnit::Step: model = I(packed(1, 16)); break;
            case m::QuantizeUnit::Beat: model = I(packed(1, 4)); break;
            case m::QuantizeUnit::Bar: model = I(packed(1, 1)); break;
            case m::QuantizeUnit::Pattern: model = I(packed(4, 1)); break; // any length > 1 bar reads PATTERN
        }
        p.put(unit, std::move(model), [&](const Var& k) {
            const auto r = packedRatio(k);
            if (! r)
                return false;
            const double x = static_cast<double>(r->first) / r->second;
            std::optional<m::QuantizeUnit> q;
            if (std::fabs(x - 1.0 / 16) < 1e-9)
                q = m::QuantizeUnit::Step;
            else if (std::fabs(x - 0.25) < 1e-9)
                q = m::QuantizeUnit::Beat;
            else if (std::fabs(x - 1.0) < 1e-9)
                q = m::QuantizeUnit::Bar;
            else if (x > 1.0)
                q = m::QuantizeUnit::Pattern;
            return field == q.value_or(def);
        });
    }
    p.put("padSequenceStart", B(s.padStartsSequence));
    p.put("sceneIndex", I(s.currentScene), [&](const Var& k) { return sameInt(k, s.currentScene, 0, 15, d.currentScene); });
    p.put("patternIndex", I(s.currentPattern),
          [&](const Var& k) { return sameInt(k, s.currentPattern, 0, 15, d.currentPattern); });
    p.put("projectFilePath", S("/mnt/emmc/" + std::string(kRootFolder) + "/Projects/" + name + ".prj"));
    p.put("versionNo", I(s.formatVersion));
    p.put("minorVersionNo", I(s.formatMinorVersion));
}

// The samples the tracks use, with the URL and file each gets in the export.
std::map<std::string, ExportSample> planSamples(const m::Project& project, const ParamIndexMap& map, ExportReport& report)
{
    std::map<std::string, ExportSample> out;
    std::set<std::string> taken; // relPath, lower case
    auto safe = [](const std::string& part) {
        return ! part.empty() && part != "." && part != ".." && part.find('\\') == std::string::npos &&
               part.find(':') == std::string::npos;
    };
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 16; ++t)
        {
            const auto& key = project.scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)].playback.sample;
            if (key.empty() || out.count(key.id) != 0)
                continue;
            ExportSample e;
            e.id = key.id;
            if (const auto* ref = project.sampleRefs.find(key))
                e.ref = *ref;
            else
                e.ref.id = key.id;
            // An imported reference keeps its URL ("sp16:<url>"): same URL, file where that URL resolves.
            if (key.id.rfind("sp16:", 0) == 0)
            {
                const std::string url = key.id.substr(5);
                std::vector<std::string> parts;
                for (std::size_t b = 0; b <= url.size();)
                {
                    const auto slash = url.find('/', b);
                    const auto part = url.substr(b, slash == std::string::npos ? std::string::npos : slash - b);
                    if (! part.empty())
                        parts.push_back(part);
                    if (slash == std::string::npos)
                        break;
                    b = slash + 1;
                }
                std::size_t k = 0;
                if (k < parts.size() && parts[k][0] == '$')
                    ++k;
                std::string folder = "Samples";
                if (k < parts.size() && parts[k][0] == '%')
                {
                    if (auto it = map.pathAliases.find(parts[k]); it != map.pathAliases.end() && ! it->second.empty())
                        folder = it->second.front();
                    ++k;
                }
                std::string rest;
                bool ok = k < parts.size();
                for (; k < parts.size(); ++k)
                {
                    ok = ok && safe(parts[k]);
                    rest += (rest.empty() ? "" : "/") + parts[k];
                }
                std::string rel = folder.empty() ? rest : folder + "/" + rest;
                // The URL stays as imported; its file goes where the URL resolves (nowhere when it is unsafe or taken:
                // the sample was not found on import either, or two URLs name one file).
                e.url = url;
                if (ok && taken.insert(juce::String(rel).toLowerCase().toStdString()).second)
                    e.relPath = rel;
                else
                    report.drop("sample URL that names no file of the export (kept as is)", url);
                out.emplace(e.id, std::move(e));
                continue;
            }
            std::string base = e.ref.displayName;
            if (base.empty())
                base = juce::File::createFileWithoutCheckingPath(e.ref.path).getFileName().toStdString();
            if (base.empty() || ! safe(base))
                base = "sample";
            juce::String stem = juce::File::createFileWithoutCheckingPath(base).getFileNameWithoutExtension();
            stem = stem.replaceCharacters("/\\:*?\"<>|", "_________");
            std::string name = stem.toStdString() + ".wav";
            for (int n = 2; ! taken.insert(juce::String("Samples/" + name).toLowerCase().toStdString()).second; ++n)
                name = stem.toStdString() + "-" + std::to_string(n) + ".wav";
            e.url = "$Int0/%Smp/" + name;
            e.relPath = "Samples/" + name;
            out.emplace(e.id, std::move(e));
        }
    report.samples = static_cast<int>(out.size());
    return out;
}

// ---------------------------------------------------------------------------------------------- sample files
juce::File locate(const ExportOptions& o, const m::SampleRef& ref)
{
    if (o.locate)
        return o.locate(ref);
    if (juce::File::isAbsolutePath(ref.path))
        return juce::File(ref.path);
    return {};
}

Result<void> writeNewFile(const juce::File& f, const void* data, std::size_t size)
{
    if (f.exists())
        return Result<void>::failure("refusing to overwrite " + f.getFullPathName().toStdString());
    if (! f.getParentDirectory().createDirectory())
        return Result<void>::failure("cannot create " + f.getParentDirectory().getFullPathName().toStdString());
    juce::FileOutputStream out(f);
    if (! out.openedOk() || ! out.write(data, size))
        return Result<void>::failure("cannot write " + f.getFullPathName().toStdString());
    out.flush();
    return out.getStatus().wasOk() ? Result<void> {} : Result<void>::failure(out.getStatus().getErrorMessage().toStdString());
}

// 44.1 kHz WAV ≤ 64 s: a byte copy when the source already is a 16- or 24-bit PCM WAV at 44.1 kHz of at most 64 s,
// else decoded (the library's own decoder: resampled to 44.1 kHz with its windowed sinc, more than two channels mixed
// down, cut at 64 s) and written as PCM WAV (16 bits when the source has 16, else 24; services/render/AudioWriter).
Result<void> writeSample(const juce::File& src, const juce::File& dst, ExportReport& report, const std::string& at)
{
    auto info = services::library::probeAudioFile(src);
    if (! info)
        return Result<void>::failure("unreadable audio " + src.getFullPathName().toStdString() + ": " +
                                     info.error().message);
    const auto& i = info.value();
    const bool pcm = i.encoding == services::library::PcmEncoding::S16 || i.encoding == services::library::PcmEncoding::S24;
    if (i.container == services::library::AudioContainer::Wav && pcm && i.sampleRate == kExportSampleRate &&
        i.seconds() <= kExportMaxSeconds && ! i.truncated && i.channels <= 2)
    {
        juce::MemoryBlock mb;
        if (! src.loadFileAsData(mb))
            return Result<void>::failure("cannot read " + src.getFullPathName().toStdString());
        ++report.samplesCopied;
        return writeNewFile(dst, mb.getData(), mb.getSize());
    }
    if (i.seconds() > kExportMaxSeconds)
    {
        ++report.samplesTruncated;
        report.drop("sample audio beyond 64 s", at);
    }
    services::library::DecodeReport dr;
    services::library::DecodeOptions opts;
    opts.maxSeconds = kExportMaxSeconds;
    auto decoded = services::library::Decoder::decode(src, kExportSampleRate, dr, opts);
    if (! decoded)
        return Result<void>::failure("cannot decode " + src.getFullPathName().toStdString() + ": " +
                                     decoded.error().message);
    const auto& buf = *decoded.value().buffer;
    if (dst.exists())
        return Result<void>::failure("refusing to overwrite " + dst.getFullPathName().toStdString());
    dst.getParentDirectory().createDirectory();
    services::render::AudioWriterSpec spec;
    spec.format = i.encoding == services::library::PcmEncoding::S16 ? services::render::SampleFormat::Pcm16
                                                                     : services::render::SampleFormat::Pcm24;
    spec.channels = buf.channels >= 2 ? 2 : 1;
    spec.sampleRate = kExportSampleRate;
    auto writer = services::render::AudioFileWriter::create(dst, spec);
    if (! writer)
        return Result<void>::failure("cannot write " + dst.getFullPathName().toStdString() + ": " +
                                     writer.error().message);
    const auto frames = std::min<std::int64_t>(buf.frames, static_cast<std::int64_t>(kExportMaxSeconds * kExportSampleRate));
    constexpr std::int64_t kChunk = 1 << 15;
    for (std::int64_t f = 0; f < frames; f += kChunk)
    {
        const float* ch[2] = {buf.ch[0] + f, buf.ch[spec.channels > 1 ? 1 : 0] + f};
        if (! writer.value()->writePlanar(ch, static_cast<int>(std::min(kChunk, frames - f))))
            return Result<void>::failure("cannot write " + dst.getFullPathName().toStdString());
    }
    if (auto r = writer.value()->finish(); ! r)
        return r;
    ++report.samplesConverted;
    return {};
}

std::vector<std::byte> wavDatBytes(double bar, double bpm, int sampleNum)
{
    Node n;
    n.type = "SampleAdjunctData";
    n.props = {{"bar", D(bar)}, {"bpm", D(bpm)}, {"sampleNum", I(sampleNum)}};
    return encodeTree(n);
}
} // namespace

// ---------------------------------------------------------------------------------------------- report
void ExportReport::drop(const std::string& what, const std::string& where)
{
    if (auto it = index_.find(what); it != index_.end())
    {
        ++dropped[it->second].count;
        return;
    }
    index_.emplace(what, dropped.size());
    dropped.push_back({what, where, 1});
}

std::vector<std::string> ExportReport::lines() const
{
    std::vector<std::string> out;
    out.push_back("content scenes=" + std::to_string(scenes) + " patterns=" + std::to_string(patterns) +
                  " samples=" + std::to_string(samples) + " copied=" + std::to_string(samplesCopied) +
                  " converted=" + std::to_string(samplesConverted) + " truncated=" + std::to_string(samplesTruncated) +
                  " missing=" + std::to_string(samplesMissing) + " dropped=" + std::to_string(dropped.size()));
    for (const auto& d : dropped)
        out.push_back("dropped " + d.what + " x" + std::to_string(d.count) + " (first: " + d.first + ")");
    return out;
}

// ---------------------------------------------------------------------------------------------- public
Node buildExportTree(const m::Project& project, const ExportOptions& o, ExportReport& report,
                     std::vector<ExportSample>& samples)
{
    std::string note;
    const ParamIndexMap active = o.map != nullptr ? *o.map : activeParamIndexMap(note);
    Ctx c {project, active, report, {}, {}, {}};
    c.samples = planSamples(project, active, report);
    const std::string name = o.name.empty() ? std::string("Export") : o.name;

    Node root;
    root.type = "ProjectData";
    settingsProps(root, project, name, c);
    for (int s = 0; s < 16; ++s)
        root.children.push_back(sceneNode(project.scenes[static_cast<std::size_t>(s)], c, s));
    root.children.push_back(arrangementNode(project.arrangement, c));
    root.children.push_back(ribbonNode(project.ribbon, c));
    root.children.push_back(midiNode(project.midi, c));
    restoreNodes(root, project.extras, report, "project");
    for (const auto& e : project.sampleRefs.extras.entries)
        report.drop("sample pool data " + e.key, "samples");
    samples.clear();
    for (auto& [id, e] : c.samples)
        samples.push_back(std::move(e));
    std::sort(samples.begin(), samples.end(), [](const ExportSample& a, const ExportSample& b) { return a.relPath < b.relPath; });
    return root;
}

bool isInstrumentDisk(const juce::File& folder)
{
    for (auto d = folder; ; d = d.getParentDirectory())
    {
        for (const auto* name : kDiskRootNames)
            if (d.getFileName().equalsIgnoreCase(name))
                return true;
        if (d != folder)
        {
            const auto root = d.getChildFile(kRootFolder);
            if (root.getChildFile("System").isDirectory() || root.getChildFile("Projects").isDirectory())
                return true;
        }
        if (d.getParentDirectory() == d)
            break;
    }
    return false;
}

Result<void> exportProject(const m::Project& project, const juce::File& folder, const ExportOptions& o,
                           ExportReport& report)
{
    using R = Result<void>;
    if (folder == juce::File())
        return R::failure("No export folder");
    if (folder.exists())
        return R::failure("The export folder exists already (an export always goes to a new folder): " +
                          folder.getFullPathName().toStdString());
    if (! folder.getParentDirectory().isDirectory())
        return R::failure("No folder at " + folder.getParentDirectory().getFullPathName().toStdString());
    if (isInstrumentDisk(folder))
        return R::failure("Refusing to export onto an instrument disk (export to a new folder elsewhere and copy it "
                          "yourself, at your own risk): " +
                          folder.getFullPathName().toStdString());
    const juce::File partial = folder.getSiblingFile(folder.getFileName() + ".partial");
    if (partial.exists())
        return R::failure("A previous export left " + partial.getFullPathName().toStdString() + " (remove it first)");
    if (! partial.createDirectory())
        return R::failure("Cannot create " + partial.getFullPathName().toStdString());

    ExportOptions opts = o;
    if (opts.name.empty())
        opts.name = folder.getFileName().toStdString();
    std::vector<ExportSample> samples;
    const Node tree = buildExportTree(project, opts, report, samples);
    const auto bytes = writeContainer(tree);
    const juce::File disk = partial.getChildFile(kRootFolder);
    juce::String fileName = juce::String(opts.name).replaceCharacters("/\\:*?\"<>|", "_________");
    if (fileName.isEmpty() || fileName == "." || fileName == "..")
        fileName = "Export";
    const juce::File prj = disk.getChildFile("Projects").getChildFile(fileName + ".prj");
    auto fail = [&](const std::string& why) {
        partial.deleteRecursively();
        return R::failure(why);
    };
    if (auto r = writeNewFile(prj, bytes.data(), bytes.size()); ! r)
        return fail(r.error().message);
    if (opts.writeSamples)
    {
        int number = 0;
        for (const auto& s : samples)
        {
            ++number;
            if (s.relPath.empty())
                continue;
            const juce::File src = locate(opts, s.ref);
            const juce::File dst = disk.getChildFile(juce::String(s.relPath));
            if (src == juce::File() || ! src.existsAsFile())
            {
                ++report.samplesMissing;
                report.drop("missing sample audio (URL written, no file)", s.relPath);
                continue;
            }
            if (auto r = writeSample(src, dst, report, s.relPath); ! r)
                return fail(r.error().message);
            // The sidecar: the source's own when it has a readable one (bytes as is), else a fresh one.
            const juce::File srcDat(src.getFullPathName() + ".dat");
            const juce::File dstDat(dst.getFullPathName() + ".dat");
            juce::MemoryBlock mb;
            std::vector<std::byte> dat;
            if (srcDat.existsAsFile() && srcDat.getSize() <= 64 * 1024 && srcDat.loadFileAsData(mb) &&
                parseWavDat(std::span(static_cast<const std::byte*>(mb.getData()), mb.getSize())))
                dat.assign(static_cast<const std::byte*>(mb.getData()), static_cast<const std::byte*>(mb.getData()) + mb.getSize());
            else
                dat = wavDatBytes(0.0, 0.0, number);
            if (auto r = writeNewFile(dstDat, dat.data(), dat.size()); ! r)
                return fail(r.error().message);
        }
    }
    if (! partial.moveFileTo(folder))
        return fail("Cannot rename " + partial.getFullPathName().toStdString() + " to " +
                    folder.getFullPathName().toStdString());
    report.diskRoot = folder.getChildFile(kRootFolder);
    report.projectFile = report.diskRoot.getChildFile("Projects").getChildFile(fileName + ".prj");
    return {};
}

services::store::CodecInfo Sp16ExportCodec::info() const
{
    static constexpr std::string_view kExtensions[] = {".prj"};
    return {"sp16-prj-export", "Reference project (export)", kExtensions, true};
}

Result<std::unique_ptr<m::Project>> Sp16ExportCodec::read(const juce::File&, services::store::ImportReport&) const
{
    return Result<std::unique_ptr<m::Project>>::failure("The export codec does not read (use the reference import)");
}

Result<void> Sp16ExportCodec::write(const m::Project& project, const juce::File& folder) const
{
    if (! services::ext::flags().isOn(services::ext::kExport))
        return Result<void>::failure("Export for the reference is an extension (ext.interop.export is off)");
    ExportReport report;
    return exportProject(project, folder, {}, report);
}
} // namespace services::interop::sp16

APP_REGISTER_CODEC(::services::interop::sp16::Sp16ExportCodec)
