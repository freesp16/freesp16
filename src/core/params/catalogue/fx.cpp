// Parameter catalogue, area FX: the slot-level FX parameters (type, bypass, FX mode, quirk profile,
// LFO restart preference), the FX parameter model of fx.h, and the ParamSpecs of the framework's test effects (None,
// GainTest, behind the feature flag "fx.test"). The twelve effects register theirs in fx_<name>.cpp.
#include "core/params/catalogue/fx.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

#if defined(__clang__)
#pragma clang fp contract(off) // float32 step law and SYNC conversion: every operation rounded (sync_convert.py)
#endif

namespace core::fx
{
namespace
{
// ------------------------------------------------------------------------------------------------ families (FX-072)

struct Frac
{
    std::int16_t num, den;
};
constexpr Frac kChorusFracs[] = {{16, 1}, {8, 1}, {4, 1}, {2, 1}, {1, 1}, {3, 4}, {1, 2}, {1, 4}, {1, 8}, {1, 16}};
constexpr Frac kFlangerFracs[] = {{64, 1}, {32, 1}, {16, 1}, {8, 1}, {4, 1}, {2, 1}, {1, 1}};
constexpr Frac kPhaserFracs[] = {{64, 1}, {32, 1}, {16, 1}, {8, 1}, {4, 1}, {2, 1},
                                 {1, 1},  {3, 4},  {1, 2},  {1, 4}, {1, 8}, {1, 16}};
constexpr Frac kInsertDelayFracs[] = {{1, 8}, {1, 4}, {1, 2}, {3, 4}, {1, 1}, {2, 1}};
constexpr Frac kSendDelayFracs[] = {{1, 8}, {1, 4}, {1, 2}, {3, 4}, {1, 1}, {2, 1}, {4, 1}, {8, 1}};

enum class Kind : std::uint8_t
{
    None,
    Rate,
    Delay,
    Ducker
};
enum class FreeMap : std::uint8_t
{
    Identity,
    Scale
}; // index → value
enum class FreeBack : std::uint8_t
{
    Round,
    RoundScale,
    TruncScale
}; // value → index

struct FamilySpec
{
    Kind kind;
    Grid free, sync;
    FreeMap i2v;
    FreeBack v2i;
    float scale, back;           // i2v scale, v2i scale
    std::span<const Frac> fracs; // empty for the Ducker (table)
    bool descending;             // fraction order
    int defaultIndex;            // indexToFraction fallback for an out-of-range index
};

const FamilySpec& spec(Family f) noexcept
{
    static const FamilySpec kPlain{Kind::None, {0, 2}, {0, 2}, FreeMap::Identity, FreeBack::Round, 1, 1, {}, false, 0};
    static const FamilySpec kChorus{Kind::Rate, {1, 101}, {0, 10},      FreeMap::Scale, FreeBack::RoundScale,
                                    0.1f,       10.0f,    kChorusFracs, true,           2};
    static const FamilySpec kFlanger{Kind::Rate, {1, 101}, {0, 7},        FreeMap::Scale, FreeBack::RoundScale,
                                     0.01f,      100.0f,   kFlangerFracs, true,           4};
    static const FamilySpec kPhaser{Kind::Rate, {1, 1001}, {0, 12},      FreeMap::Scale, FreeBack::RoundScale,
                                    0.01f,      100.0f,    kPhaserFracs, true,           4};
    static const FamilySpec kInsertDelay{
        Kind::Delay, {10, 1001}, {0, 6}, FreeMap::Identity, FreeBack::Round, 1.0f, 1.0f, kInsertDelayFracs, false, 2};
    static const FamilySpec kSendDelay{
        Kind::Delay, {10, 4001}, {0, 8}, FreeMap::Identity, FreeBack::Round, 1.0f, 1.0f, kSendDelayFracs, false, 6};
    static const FamilySpec kDuckA{Kind::Ducker, {0, 10001}, {0, 128}, FreeMap::Scale, FreeBack::TruncScale,
                                   0.1f,         10.0f,      {},       false,          0};
    static const FamilySpec kDuckR{Kind::Ducker, {1, 80001}, {0, 128}, FreeMap::Scale, FreeBack::TruncScale,
                                   0.1f,         10.0f,      {},       false,          0};
    switch (f)
    {
    case Family::Chorus:
        return kChorus;
    case Family::Flanger:
        return kFlanger;
    case Family::Phaser:
        return kPhaser;
    case Family::InsertDelay:
        return kInsertDelay;
    case Family::SendDelay:
        return kSendDelay;
    case Family::DuckerAttack:
        return kDuckA;
    case Family::DuckerRelease:
        return kDuckR;
    case Family::Plain:
        break;
    }
    return kPlain;
}

// FX-1210: T_A[k] = 4·(k/127)², T_R[k] = 0.25·128^(k/127), rounded to 0.001 sixteenth.
struct DuckerTables
{
    float a[128], r[128];
    DuckerTables() noexcept
    {
        for (int k = 0; k < 128; ++k)
        {
            const double x = k / 127.0;
            a[k] = static_cast<float>(std::round(4.0 * x * x * 1000.0) / 1000.0);
            r[k] = static_cast<float>(std::round(0.25 * std::pow(128.0, x) * 1000.0) / 1000.0);
        }
    }
};
const DuckerTables& duckerTables() noexcept
{
    static const DuckerTables t;
    return t;
}

int roundToInt(float x) noexcept // JUCE roundToInt: double + 1.5·2^52 = round half to even
{
    if (! (x == x) || std::isinf(x))
        return 0;
    return static_cast<int>(std::nearbyint(static_cast<double>(x)));
}

int truncS32(float x) noexcept // vcvt.s32.f32
{
    if (! (x == x))
        return 0;
    if (x >= 2147483647.0f)
        return 2147483647;
    if (x <= -2147483648.0f)
        return -2147483647 - 1;
    return static_cast<int>(x);
}

float clamp01(float v) noexcept // bmi → 0, vmovgt → 1 (NaN passes)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

float fracValue(Family f, int i) noexcept
{
    const auto& s = spec(f);
    if (s.kind == Kind::Ducker)
    {
        i = i < 0 ? 0 : (i > 127 ? 127 : i);
        return duckerSixteenths(f, i);
    }
    const int n = static_cast<int>(s.fracs.size());
    if (i < 0 || i >= n)
        i = s.defaultIndex;
    return static_cast<float>(s.fracs[static_cast<std::size_t>(i)].num) /
           static_cast<float>(s.fracs[static_cast<std::size_t>(i)].den);
}

int fracIndex(Family f, float x) noexcept
{
    const auto& s = spec(f);
    if (s.kind == Kind::Ducker)
    {
        int lo = 0, hi = 127;
        while (hi > lo)
        {
            const int mid = lo + (hi - lo) / 2;
            const float t = duckerSixteenths(f, mid) + 1e-4f;
            if (t > x)
                hi = mid;
            else
                lo = mid + 1;
        }
        return hi;
    }
    const int n = static_cast<int>(s.fracs.size());
    for (int k = 0; k + 1 < n; ++k)
    {
        const float a = fracValue(f, k), b = fracValue(f, k + 1);
        const float sum = a + b;
        const float m = sum * 0.5f;
        if (s.descending ? x > m : x < m)
            return k;
    }
    return n - 1;
}

float freeValue(Family f, int i) noexcept
{
    const auto& s = spec(f);
    return s.i2v == FreeMap::Scale ? static_cast<float>(i) * s.scale : static_cast<float>(i);
}

int freeIndex(Family f, float x) noexcept
{
    const auto& s = spec(f);
    switch (s.v2i)
    {
    case FreeBack::RoundScale:
        return roundToInt(x * s.back);
    case FreeBack::TruncScale:
        return truncS32(x * s.back);
    case FreeBack::Round:
        break;
    }
    return roundToInt(x);
}

// ------------------------------------------------------------------------------------------------ registration

struct Store
{
    std::deque<std::string> ids;        // generated ParamSpec ids (stable addresses: ParamSpec holds views)
    std::deque<EffectParamSet> effects; // what registerEffect recorded
};
Store& store()
{
    static Store s;
    return s;
}

std::string_view keep(std::string s)
{
    auto& ids = store().ids;
    ids.push_back(std::move(s));
    return ids.back();
}

struct SlotInfo
{
    std::uint8_t bit;
    std::string_view prefix, group;
};
constexpr SlotInfo kSlots[] = {{kSlotInsert, "fx1", "FX1"},
                               {kSlotInsert, "fx2", "FX2"},
                               {kSlotSend, "send", "SEND FX"},
                               {kSlotMaster, "master", "MASTER FX"}};

template <Family F> int formatFree(float v, char* buf, int n) noexcept
{
    return format(F, v, false, buf, n);
}
ParamFormatter freeFormatter(Family f) noexcept
{
    switch (f)
    {
    case Family::Chorus:
        return &formatFree<Family::Chorus>;
    case Family::Flanger:
        return &formatFree<Family::Flanger>;
    case Family::Phaser:
        return &formatFree<Family::Phaser>;
    case Family::InsertDelay:
        return &formatFree<Family::InsertDelay>;
    case Family::SendDelay:
        return &formatFree<Family::SendDelay>;
    case Family::DuckerAttack:
        return &formatFree<Family::DuckerAttack>;
    case Family::DuckerRelease:
        return &formatFree<Family::DuckerRelease>;
    case Family::Plain:
        break;
    }
    return nullptr;
}
template <Family F> int formatSync(float v, char* buf, int n) noexcept
{
    return format(F, v, true, buf, n);
}
ParamFormatter syncFormatter(Family f) noexcept
{
    switch (f)
    {
    case Family::Chorus:
        return &formatSync<Family::Chorus>;
    case Family::Flanger:
        return &formatSync<Family::Flanger>;
    case Family::Phaser:
        return &formatSync<Family::Phaser>;
    case Family::InsertDelay:
        return &formatSync<Family::InsertDelay>;
    case Family::SendDelay:
        return &formatSync<Family::SendDelay>;
    case Family::DuckerAttack:
        return &formatSync<Family::DuckerAttack>;
    case Family::DuckerRelease:
        return &formatSync<Family::DuckerRelease>;
    case Family::Plain:
        break;
    }
    return nullptr;
}
} // namespace

// ------------------------------------------------------------------------------------------------ public API

Grid freeGrid(Family f) noexcept
{
    return spec(f).free;
}
Grid syncGrid(Family f) noexcept
{
    return spec(f).sync;
}
bool hasSync(Family f) noexcept
{
    return spec(f).kind != Kind::None;
}

float duckerSixteenths(Family f, int k) noexcept
{
    k = k < 0 ? 0 : (k > 127 ? 127 : k);
    const auto& t = duckerTables();
    return f == Family::DuckerRelease ? t.r[k] : t.a[k];
}

float syncBeats(Family f, int i) noexcept
{
    return fracValue(f, i);
}

int stepIndex(float v, Grid g) noexcept
{
    const float p = v * static_cast<float>(g.span());
    const float x = static_cast<float>(g.lo) + p;
    return roundToInt(x);
}

float valueOfStep(int i, Grid g) noexcept
{
    const int d = g.span();
    if (d == 0)
        return 0.0f;
    return static_cast<float>(i - g.lo) / static_cast<float>(d);
}

float step(float v, Grid g, int delta, int multiplier) noexcept
{
    int i = stepIndex(v, g) + multiplier * delta;
    i = i < g.lo ? g.lo : (i > g.hi - 1 ? g.hi - 1 : i);
    return valueOfStep(i, g);
}

int coarseMultiplier(Grid g, Family f, float v, bool sync, bool shift) noexcept
{
    if (sync || shift)
        return 1;
    const auto kind = spec(f).kind;
    if (kind == Kind::Delay || kind == Kind::Ducker) // time parameters: 10 / 50 / 100 ms by magnitude
    {
        const int i = stepIndex(v, g);
        const float ms = kind == Kind::Ducker ? static_cast<float>(i) * 0.1f : static_cast<float>(i);
        const int msStep = ms < 100.0f ? 10 : (ms < 1000.0f ? 50 : 100);
        return kind == Kind::Ducker ? msStep * 10 : msStep;
    }
    const int steps = g.hi - g.lo;
    if (steps <= 101)
        return 1;
    const int m = static_cast<int>(std::lround(steps / 100.0));
    return m > 1 ? m : 1;
}

float convertSync(float v, Family f, bool toSync, double bpm) noexcept
{
    const auto& s = spec(f);
    if (s.kind == Kind::None)
        return v;
    float T = static_cast<float>(bpm); // FxProcessorBase 0x112a48: max(f32(tempo), 0.5)
    if (! (T >= 0.5f))
        T = 0.5f;
    if (s.kind != Kind::Ducker)
        v = clamp01(v);
    float y = 0.0f;
    if (toSync)
    {
        const float x = freeValue(f, stepIndex(v, s.free));
        if (s.kind == Kind::Rate)
        {
            const float a = T / 60.0f, b = 1.0f / x;
            y = a * b;
        }
        else if (s.kind == Kind::Delay)
        {
            const float a = T / 60.0f;
            const float b = x * a;
            y = b * 0.001f;
        }
        else
        {
            const float a = T * 0.004f;
            const float b = a / 60.0f;
            y = x * b;
        }
        return clamp01(valueOfStep(fracIndex(f, y), s.sync));
    }
    const float x = fracValue(f, stepIndex(v, s.sync));
    if (s.kind == Kind::Rate)
    {
        const float a = T / 60.0f, b = 1.0f / x;
        y = a * b;
    }
    else if (s.kind == Kind::Delay)
    {
        const float a = 60.0f / T;
        const float b = x * a;
        y = b * 1000.0f;
    }
    else
    {
        const float a = 60000.0f / T;
        const float b = x * a;
        y = b * 0.25f;
    }
    return clamp01(valueOfStep(freeIndex(f, y), s.free));
}

float ccToValue(int cc, Grid g) noexcept
{
    cc = cc < 0 ? 0 : (cc > 127 ? 127 : cc);
    const double x = static_cast<double>(cc) * static_cast<double>(g.span()) / 127.0;
    const int i = g.lo + static_cast<int>(std::nearbyint(x)); // half to even (MID-031)
    return valueOfStep(i, g);
}

int format(Family f, float v, bool sync, char* buf, int n) noexcept
{
    if (buf == nullptr || n <= 0)
        return 0;
    const auto& s = spec(f);
    int len = 0;
    if (sync && s.kind != Kind::None)
    {
        const int i = stepIndex(v, s.sync);
        if (s.kind == Kind::Ducker)
            len = std::snprintf(buf, static_cast<std::size_t>(n), "%.2f", static_cast<double>(fracValue(f, i)));
        else
        {
            const int k = i < 0 || i >= static_cast<int>(s.fracs.size()) ? s.defaultIndex : i;
            const auto fr = s.fracs[static_cast<std::size_t>(k)];
            len = std::snprintf(buf, static_cast<std::size_t>(n), "%d/%d", fr.num, fr.den);
        }
    }
    else
    {
        const int i = stepIndex(v, s.free);
        switch (f)
        {
        case Family::Chorus:
            len = std::snprintf(buf, static_cast<std::size_t>(n), "%.1f Hz", static_cast<double>(freeValue(f, i)));
            break;
        case Family::Flanger:
        case Family::Phaser:
            len = std::snprintf(buf, static_cast<std::size_t>(n), "%.2f Hz", static_cast<double>(freeValue(f, i)));
            break;
        case Family::InsertDelay:
        case Family::SendDelay:
            len = std::snprintf(buf, static_cast<std::size_t>(n), "%d ms", i);
            break;
        case Family::DuckerAttack:
        case Family::DuckerRelease:
            len = std::snprintf(buf, static_cast<std::size_t>(n), "%.1f ms", static_cast<double>(freeValue(f, i)));
            break;
        case Family::Plain:
            len = std::snprintf(buf, static_cast<std::size_t>(n), "%d", roundToInt(v * 100.0f));
            break;
        }
    }
    return len < 0 ? 0 : (len < n ? len : n - 1);
}

float defaultFor(const EffectParamDef& d, std::uint8_t slot) noexcept
{
    return slot == kSlotSend ? d.defSend : (slot == kSlotMaster ? d.defMaster : d.defInsert);
}

void registerEffect(ParamRegistry& r, std::string_view effect, std::span<const EffectParamDef> defs,
                    std::uint8_t slotMask, std::string_view featureFlag, std::span<const EffectOptionDef> options)
{
    auto& st = store();
    const std::string_view name = keep(std::string(effect));
    bool known = false;
    for (const auto& e : st.effects)
        known = known || e.effect == effect;
    if (! known)
        st.effects.push_back({name, defs, options, slotMask, featureFlag});
    for (const auto& slot : kSlots)
    {
        if ((slotMask & slot.bit) == 0)
            continue;
        const bool insert = slot.bit == kSlotInsert;
        for (const auto& d : defs)
        {
            const Grid g = d.free;
            const float span = static_cast<float>(g.span() > 0 ? g.span() : 1);
            const int coarse = coarseMultiplier(g, d.family, defaultFor(d, slot.bit), false, false);
            ParamSpec p;
            p.id = keep(std::string(slot.prefix) + "." + std::string(effect) + "." + std::string(d.key));
            p.label = d.label;
            p.shortLabel = d.shortLabel;
            p.group = slot.group;
            p.type = ParamType::Continuous;
            p.minValue = 0.0f;
            p.maxValue = 1.0f;
            p.defaultValue = defaultFor(d, slot.bit);
            p.coarseStep = static_cast<float>(coarse) / span;
            p.fineStep = 1.0f / span;
            p.format = d.format != nullptr ? d.format : freeFormatter(d.family);
            p.lockable = insert;    // FX-078 / FX-094
            p.lfoTarget = insert;   // FX-083
            p.stripTarget = insert; // FX-095
            p.bipolar = d.bipolar;
            p.featureFlag = featureFlag;
            p.scope = insert ? ParamScope::Track : ParamScope::Scene;
            r.add(p);
            if (hasSync(d.family)) // UID-830: the SYNC form `<id>.div`, the same knob k on the sync grid
            {
                const Grid sg = d.sync.span() > 0 ? d.sync : syncGrid(d.family);
                const float sspan = static_cast<float>(sg.span() > 0 ? sg.span() : 1);
                ParamSpec q = p;
                q.id = keep(std::string(p.id) + ".div");
                const bool ducker = d.family == Family::DuckerAttack || d.family == Family::DuckerRelease;
                q.label = ducker ? d.label : "DIV"; // the Ducker keeps ATTACK / RELEASE and shows sixteenths
                q.shortLabel = ducker ? d.shortLabel : "DIV";
                q.defaultValue = convertSync(p.defaultValue, d.family, true, 120.0);
                q.coarseStep =
                    static_cast<float>(coarseMultiplier(sg, d.family, q.defaultValue, true, false)) / sspan;
                q.fineStep = 1.0f / sspan;
                q.format = syncFormatter(d.family);
                q.bipolar = false;
                r.add(q);
            }
        }
        for (const auto& o : options)
        {
            ParamSpec p;
            p.id = keep(std::string(slot.prefix) + "." + std::string(effect) + "." + std::string(o.key));
            p.label = o.label;
            p.shortLabel = o.shortLabel;
            p.group = slot.group;
            p.type = o.enumLabels.empty() ? ParamType::Bool : ParamType::Enum;
            p.minValue = 0.0f;
            p.maxValue = o.enumLabels.empty() ? 1.0f : static_cast<float>(o.enumLabels.size() - 1);
            p.defaultValue = slot.bit == kSlotSend ? o.defSend : (slot.bit == kSlotMaster ? o.defMaster : o.defInsert);
            p.enumLabels = o.enumLabels;
            p.featureFlag = featureFlag;
            p.scope = insert ? ParamScope::Track : ParamScope::Scene;
            r.add(p);
        }
    }
}

const EffectParamSet* effectParams(std::string_view effect) noexcept
{
    for (const auto& e : store().effects)
        if (e.effect == effect)
            return &e;
    return nullptr;
}

const EffectParamDef* paramByK(const EffectParamSet& set, int k) noexcept
{
    for (const auto& d : set.params)
        if (d.k == k)
            return &d;
    return nullptr;
}

std::vector<std::int8_t> internalIndexTable(const ParamRegistry& r)
{
    std::vector<std::int8_t> out(r.size(), static_cast<std::int8_t>(-1));
    for (std::size_t i = 0; i < r.size(); ++i)
    {
        const std::string_view id = r.all()[i].id;
        const auto a = id.find('.');
        if (a == std::string_view::npos)
            continue;
        const auto prefix = id.substr(0, a);
        if (prefix != "fx1" && prefix != "fx2" && prefix != "send" && prefix != "master")
            continue;
        const auto rest = id.substr(a + 1);
        const auto b = rest.find('.');
        if (b == std::string_view::npos)
            continue;
        const auto* set = effectParams(rest.substr(0, b));
        if (set == nullptr)
            continue;
        auto key = rest.substr(b + 1);
        if (key.size() > 4 && key.substr(key.size() - 4) == ".div") // the SYNC form: the same knob k
            key = key.substr(0, key.size() - 4);
        for (const auto& d : set->params)
            if (d.key == key)
                out[i] = d.k;
    }
    return out;
}

std::string_view effectIdOf(model::EffectType t) noexcept
{
    using model::EffectType;
    switch (t)
    {
    case EffectType::Chorus:
        return "chorus";
    case EffectType::Flanger:
        return "flanger";
    case EffectType::Phaser:
        return "phaser";
    case EffectType::ParametricEq2:
        return "eq2";
    case EffectType::MultiModeFilter:
        return "filter";
    case EffectType::InsertDelay:
        return "delay";
    case EffectType::SendDelay:
        return "delayBus";
    case EffectType::Reverb:
        return "reverb";
    case EffectType::LoFi:
        return "lofi";
    case EffectType::Distortion:
        return "distortion";
    case EffectType::Compressor:
        return "compressor";
    case EffectType::Ducker:
        return "ducker";
    case EffectType::VintagePitch:
        return "vpitch";
    case EffectType::Off:
        break;
    }
    return {};
}

model::EffectType effectTypeOf(std::string_view id) noexcept
{
    for (int i = 1; i <= static_cast<int>(model::EffectType::VintagePitch); ++i)
        if (effectIdOf(static_cast<model::EffectType>(i)) == id)
            return static_cast<model::EffectType>(i);
    return model::EffectType::Off;
}

// ------------------------------------------------------------------------------------------------ the catalogue

namespace
{
constexpr std::string_view kModeLabels[] = {"BOTH", "SEND", "MASTER"};
constexpr std::string_view kQuirkLabels[] = {"SIXTEEN", "REFERENCE"};

// The framework's test effects (engine/fx/effects/None, GainTest): behind "fx.test", never shown.
constexpr EffectParamDef kNoneParams[] = {{.key = "mix",
                                           .label = "MIX",
                                           .shortLabel = "MIX",
                                           .k = 5,
                                           .free = {0, 101},
                                           .defInsert = 0.5f,
                                           .defSend = 0.5f,
                                           .defMaster = 0.5f}};
constexpr EffectParamDef kGainTestParams[] = {{.key = "gain",
                                               .label = "GAIN",
                                               .shortLabel = "GAIN",
                                               .k = 0,
                                               .free = {0, 128},
                                               .defInsert = 100.0f / 127.0f,
                                               .defSend = 100.0f / 127.0f,
                                               .defMaster = 100.0f / 127.0f}};

void addSlotParams(ParamRegistry& r)
{
    for (const auto& slot : kSlots)
    {
        const bool insert = slot.bit == kSlotInsert;
        ParamSpec t;
        t.id = keep(std::string(slot.prefix) + ".type");
        t.label = "FX TYPE";
        t.shortLabel = "FX TYPE";
        t.group = slot.group;
        t.type = ParamType::Integer; // the slot kind's list index + 1, 0 = NONE (engine/fx/EffectRegistryOrder.h)
        t.minValue = 0.0f;
        t.maxValue = 31.0f;
        t.defaultValue = 0.0f; // FX-091: a fresh slot is Off
        t.scope = insert ? ParamScope::Track : ParamScope::Scene;
        r.add(t);
        // FX-044/045 inserts; FX-046 Send/Master: a Sixteen bypass (the reference stores the flag but never reads it,
        // so its field is not mapped: imports load it off).
        ParamSpec b;
        b.id = keep(std::string(slot.prefix) + ".bypass");
        b.label = "FX BYPASS";
        b.shortLabel = "BYPASS";
        b.group = slot.group;
        b.type = ParamType::Bool;
        b.maxValue = 1.0f;
        b.scope = insert ? ParamScope::Track : ParamScope::Scene;
        if (insert)
            b.storage = {.reference = slot.prefix == "fx1" ? "bBypassFx1" : "bBypassFx2"};
        r.add(b);
    }
    r.add({.id = "fx.mode",
           .label = "FX MODE",
           .shortLabel = "FX MODE",
           .group = "MASTER/SEND FX",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 2,
           .defaultValue = 0, // FX-009: Both
           .enumLabels = kModeLabels,
           .scope = ParamScope::Scene});
    r.add({.id = "fx.quirkProfile",
           .label = "FX PROFILE",
           .shortLabel = "FX PROF",
           .group = "MASTER/SEND FX",
           .type = ParamType::Enum,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 0, // FX-053: Sixteen
           .enumLabels = kQuirkLabels,
           .scope = ParamScope::Project});
    // The slot whose IN/OUT peaks and telemetry the engine measures (the effect panel on show, UID-834; FxSlotBank
    // setMeteredSlot): UI session state, never saved ("session." key, like pads.mode).
    r.add({.id = "fx.meteredSlot",
           .label = "FX METER",
           .shortLabel = "FX METER",
           .group = "EFFECTS",
           .type = ParamType::Integer,
           .minValue = -1,
           .maxValue = 33,
           .defaultValue = -1,
           .scope = ParamScope::Global,
           .storage = {.key = "session.fx.meteredSlot"}});
    r.add({.id = "fx.restartLfoOnPlay",
           .label = "RESTART FX LFOS ON PLAY",
           .shortLabel = "FX LFO",
           .group = "EFFECTS",
           .type = ParamType::Bool,
           .minValue = 0,
           .maxValue = 1,
           .defaultValue = 1, // FX-089
           .scope = ParamScope::Global});
    const std::uint8_t all = kSlotInsert | kSlotSend | kSlotMaster;
    registerEffect(r, "none", kNoneParams, all, "fx.test");
    registerEffect(r, "gainTest", kGainTestParams, all, "fx.test");
}
} // namespace
} // namespace core::fx

APP_REGISTER_PARAMS("fx", 700, &core::fx::addSlotParams)
