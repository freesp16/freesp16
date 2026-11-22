#include "services/midilearn/MidiLearn.h"

#include "services/store/NativeCodecJson.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace services::midilearn
{
namespace
{
bool integral(const core::ParamSpec& s) noexcept
{
    return s.type == core::ParamType::Integer || s.type == core::ParamType::Enum || s.type == core::ParamType::Bool ||
           s.type == core::ParamType::Note;
}

float toNative(const core::ParamSpec& s, float v01) noexcept
{
    float v = s.minValue + std::clamp(v01, 0.0f, 1.0f) * (s.maxValue - s.minValue);
    if (integral(s))
        v = std::nearbyint(v);
    return s.clamp(v);
}

bool sameSource(const LearnBinding& b, std::uint8_t channel, std::uint8_t status, std::uint8_t number) noexcept
{
    return b.channel == channel && b.status == status && b.number == number;
}
} // namespace

bool learnable(const core::ParamSpec& s) noexcept
{
    if (s.id == kCarrierId || s.isPreference())
        return false;
    const auto key = s.storageKey();
    return key.substr(0, 9) != "internal." && s.maxValue > s.minValue;
}

std::vector<core::ParamId> learnTargets(const core::ParamRegistry& r)
{
    std::vector<core::ParamId> out;
    for (std::size_t i = 0; i < r.size(); ++i)
        if (learnable(r.all()[i]))
            out.push_back(core::ParamId {static_cast<std::uint16_t>(i)});
    return out;
}

core::Result<void> LearnMap::add(const LearnBinding& b, const core::ParamRegistry& r, const core::midi::CcTable* fixed)
{
    using R = core::Result<void>;
    if (! b.target.valid() || b.target.value >= r.size() || ! learnable(r[b.target]))
        return R::failure("MIDI learn: the parameter cannot be learned");
    if (b.channel < 1 || b.channel > 16 || b.number > 127 || (b.status != kStatusCc && b.status != kStatusNote))
        return R::failure("MIDI learn: not a CC or note source");
    if (! (b.lo >= 0.0f && b.lo <= 1.0f && b.hi >= 0.0f && b.hi <= 1.0f))
        return R::failure("MIDI learn: the range must lie within 0-1");
    if (fixed != nullptr && b.status == kStatusCc && fixedCc(*fixed, b.number))
        return R::failure("MIDI learn: CC " + std::to_string(b.number) + " belongs to the fixed map");
    for (auto& x : bindings_)
        if (sameSource(x, b.channel, b.status, b.number))
        {
            x = b;
            return {};
        }
    if (bindings_.size() >= kMaxBindings)
        return R::failure("MIDI learn: " + std::to_string(kMaxBindings) + " bindings at most");
    bindings_.push_back(b);
    return {};
}

bool LearnMap::remove(std::uint8_t channel, std::uint8_t status, std::uint8_t number)
{
    const auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                 [&](const LearnBinding& b) { return sameSource(b, channel, status, number); });
    if (it == bindings_.end())
        return false;
    bindings_.erase(it);
    return true;
}

int LearnMap::apply(const Message& m, const core::ParamRegistry& r, std::vector<Applied>& out) const
{
    const bool note = m.status == kStatusNote || m.status == 0x80;
    const std::uint8_t status = note ? kStatusNote : m.status;
    int n = 0;
    for (const auto& b : bindings_)
    {
        if (! sameSource(b, m.channel, status, m.number) || b.target.value >= r.size())
            continue;
        const auto& spec = r[b.target];
        float v01 = 0;
        if (note)
            v01 = m.status == kStatusNote && m.value > 0 ? b.hi : b.lo;
        else
            v01 = b.lo + (b.hi - b.lo) * static_cast<float>(std::min<int>(m.value, 127)) / 127.0f;
        out.push_back({&spec, b.scope, toNative(spec, v01)});
        ++n;
    }
    return n;
}

// The native codec's canonical JSON (services/store/NativeCodecJson.h), so the key is written as plain JSON in the file.
std::string LearnMap::toJson(const core::ParamRegistry& r) const
{
    juce::Array<juce::var> list;
    for (const auto& b : bindings_)
    {
        if (b.target.value >= r.size())
            continue;
        auto* o = new juce::DynamicObject();
        const auto id = r[b.target].id;
        o->setProperty("ch", static_cast<int>(b.channel));
        o->setProperty("st", static_cast<int>(b.status));
        o->setProperty("no", static_cast<int>(b.number));
        o->setProperty("param", juce::String::fromUTF8(id.data(), static_cast<int>(id.size())));
        o->setProperty("scope", static_cast<int>(b.scope));
        o->setProperty("lo", static_cast<double>(b.lo));
        o->setProperty("hi", static_cast<double>(b.hi));
        list.add(juce::var(o));
    }
    return services::store::detail::canonicalJson(juce::var(list));
}

core::Result<LearnMap> LearnMap::fromJson(std::string_view text, const core::ParamRegistry& r, std::string* warnings)
{
    using R = core::Result<LearnMap>;
    juce::var v;
    if (const auto res = juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), v);
        res.failed() || ! v.isArray())
        return R::failure("MIDI learn: the stored bindings are not a JSON array");
    LearnMap map;
    for (const auto& e : *v.getArray())
    {
        const auto id = e["param"].toString().toStdString();
        LearnBinding b;
        b.channel = static_cast<std::uint8_t>(static_cast<int>(e["ch"]));
        b.status = static_cast<std::uint8_t>(static_cast<int>(e["st"]));
        b.number = static_cast<std::uint8_t>(static_cast<int>(e["no"]));
        b.scope = static_cast<std::uint16_t>(static_cast<int>(e["scope"]));
        b.lo = static_cast<float>(static_cast<double>(e["lo"]));
        b.hi = static_cast<float>(static_cast<double>(e["hi"]));
        b.target = r.idOf(id).value_or(core::ParamId {});
        if (auto ok = map.add(b, r); ! ok)
        {
            if (warnings != nullptr)
                *warnings += (warnings->empty() ? "" : "; ") + id + ": " + ok.error().message;
            continue;
        }
    }
    return map;
}

LearnMap LearnMap::load(const core::model::Project& p, const core::ParamRegistry& r, std::string* warnings)
{
    const auto* e = p.extras.find(kExtrasKey);
    if (e == nullptr)
        return {};
    auto m = fromJson(e->value, r, warnings);
    if (! m)
    {
        if (warnings != nullptr)
            *warnings += m.error().message;
        return {};
    }
    return std::move(m.value());
}

void LearnSession::arm(core::ParamId target, std::uint16_t scope, float lo, float hi) noexcept
{
    armed_ = true;
    target_ = target;
    scope_ = scope;
    lo_ = lo;
    hi_ = hi;
}

std::optional<LearnBinding> LearnSession::capture(const Message& m, bool fromControllerPort) noexcept
{
    if (! armed_ || fromControllerPort)
        return std::nullopt; // learn never captures a port marked `controller`
    const bool noteOn = m.status == kStatusNote && m.value > 0;
    if (m.status != kStatusCc && ! noteOn)
        return std::nullopt;
    armed_ = false;
    LearnBinding b;
    b.channel = m.channel;
    b.status = noteOn ? kStatusNote : kStatusCc;
    b.number = m.number;
    b.target = target_;
    b.scope = scope_;
    b.lo = lo_;
    b.hi = hi_;
    return b;
}

bool fixedCc(const core::midi::CcTable& fixed, int cc) noexcept
{
    return cc >= 0 && cc < 128 && fixed.cc[static_cast<std::size_t>(cc)].kind != core::midi::CcKind::None;
}

void buildOverlay(const core::midi::CcTable& fixed, core::ParamId carrier, core::midi::CcTable& out) noexcept
{
    out = fixed;
    for (int cc = 0; cc < 128; ++cc)
    {
        auto& e = out.cc[static_cast<std::size_t>(cc)];
        if (e.kind != core::midi::CcKind::None)
            continue;
        e.kind = core::midi::CcKind::Linear;
        e.integral = true;
        e.param = carrier;
        e.lo = static_cast<float>(cc * 128);
        e.hi = static_cast<float>(cc * 128 + 127);
    }
}

std::optional<Message> decodeCarrier(const core::midi::UiEvent& e, core::ParamId carrier,
                                     const core::model::MidiSettings& midi)
{
    if (e.kind != core::midi::UiEvent::Kind::Param || ! carrier.valid() || e.param != carrier.value)
        return std::nullopt;
    const int code = static_cast<int>(std::lround(e.value));
    if (code < 0 || code >= 128 * 128)
        return std::nullopt;
    Message m;
    m.status = kStatusCc;
    m.number = static_cast<std::uint8_t>(code / 128);
    m.value = static_cast<std::uint8_t>(code % 128);
    const int channel = midi.input.channels[static_cast<std::size_t>(e.scope & 15)];
    if (channel < 1 || channel > 16)
        return std::nullopt;
    m.channel = static_cast<std::uint8_t>(channel);
    return m;
}
} // namespace services::midilearn
