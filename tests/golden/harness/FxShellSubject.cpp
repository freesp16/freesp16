// fx.shell: engine::fx::FxShell and engine::fx::FxProcessor against the research wrapper and hosts
// (tools/golden-gen/goldenlib/models/fx_shell.py drives tools/fx/effectbase.py and fx_processors.py). The stub effects
// are the adapter's: "wet = 0.5 · input" for the shell; for the processors, type i: wet = f32((i + 1)/16) · input, or
// with tempo_stub on the tempo-fed indices f32(bpm) · 2^-10 (the fed, clamped tempo made audible).
#include "tests/golden/harness/Subject.h"

#include "engine/fx/FxProcessor.h"
#include "engine/fx/FxShell.h"

#include <array>
#include <cstring>
#include <memory>

namespace golden
{
namespace
{
using namespace engine::fx;

class HalfStub final : public EffectBase
{
public:
    void prepare(double, int) override {}
    void reset() noexcept override {}
    void setParam(int, float) noexcept override {}
    void process(float* l, float* r, const ProcessContext&) noexcept override
    {
        for (int i = 0; i < engine::kBlockFrames; ++i)
        {
            l[i] *= 0.5f;
            r[i] *= 0.5f;
        }
    }
};

bool gTempoStub = false; // set by configure() before the processor builds its instances
SlotKind gKind = SlotKind::Insert;

template <int I> class GainStub final : public EffectBase
{
public:
    void prepare(double, int) override {}
    void reset() noexcept override {}
    void setParam(int, float) noexcept override {}
    void process(float* l, float* r, const ProcessContext& ctx) noexcept override
    {
        float g = static_cast<float>((I + 1) / 16.0);
        if (tempo_)
            g = static_cast<float>(ctx.bpm) * (1.0f / 1024.0f);
        for (int i = 0; i < engine::kBlockFrames; ++i)
        {
            l[i] *= g;
            r[i] *= g;
        }
    }
    bool tempo_ = gTempoStub && fed(I);

private:
    static bool fed(int i) noexcept
    {
        if (gKind == SlotKind::Insert)
            return i == 0 || i == 1 || i == 4 || i == 8;
        return i == 0 || i == 2 || i == 3 || i == 9;
    }
};

template <int... I>
constexpr std::array<EffectDescriptor, sizeof...(I)> stubDescriptors(std::integer_sequence<int, I...>)
{
    return {EffectDescriptor{.id = "stub",
                             .name = "STUB",
                             .slots = 7,
                             .params = {},
                             .options = {},
                             .visual = {},
                             .shell = {},
                             .create = &make<GainStub<I>>,
                             .featureFlag = {}}...};
}
const auto kStubs = stubDescriptors(std::make_integer_sequence<int, 10>{});

std::uint8_t lawOf(const juce::String& s)
{
    if (s == "wetOnly")
        return static_cast<std::uint8_t>(MixLaw::WetOnly);
    if (s == "unityK5")
        return static_cast<std::uint8_t>(MixLaw::UnityDryK5);
    if (s == "delay")
        return static_cast<std::uint8_t>(MixLaw::DelayPiecewise);
    if (s == "reverb")
        return static_cast<std::uint8_t>(MixLaw::ReverbPiecewise);
    return static_cast<std::uint8_t>(MixLaw::CrossfadeK5);
}

struct Event
{
    int block = 0, what = 0; // shell: 0 activate, 1 stop, 2 mix; processor: 0 type, 1 bypass, 2 disable, 3 bpm
    double value = 0.0;
};

class FxShellSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto& p = c.params;
        if (static_cast<double>(p["fs"]) != c.sampleRate)
            return "params.fs must equal the recipe's sample_rate";
        const juce::String kind = p["kind"].isVoid() ? juce::String("shell") : p["kind"].toString();
        shellMode_ = kind == "shell";
        if (c.outputNames.size() != (shellMode_ ? 2u : 3u))
            return "fx.shell: wrong output channels";
        ctx_ = ProcessContext{c.sampleRate, 120.0, false, 0u, {}, 0};
        ctx_.triggerOffset.fill(-1);
        events_.clear();
        if (auto* ev = p["events"].getArray())
            for (auto& e : *ev)
            {
                Event x;
                x.block = static_cast<int>(e["block"]);
                if (shellMode_)
                {
                    const auto op = e["op"].toString();
                    x.what = op == "activate" ? 0 : (op == "stop" ? 1 : 2);
                    x.value = e["v"].isVoid() ? 0.0 : static_cast<double>(e["v"]);
                }
                else if (e.hasProperty("type"))
                    x = {x.block, 0, static_cast<double>(e["type"])};
                else if (e.hasProperty("bypass"))
                    x = {x.block, 1, static_cast<double>(e["bypass"])};
                else if (e.hasProperty("disable"))
                    x = {x.block, 2, static_cast<double>(e["disable"])};
                else
                    x = {x.block, 3, static_cast<double>(e["bpm"])};
                events_.push_back(x);
            }
        if (shellMode_)
        {
            ShellPolicy pol{};
            pol.gateMode = static_cast<std::uint8_t>(static_cast<int>(p["gate"]));
            pol.mixLaw = lawOf(p["law"].toString());
            pol.wetHighPass = static_cast<bool>(p["hpf"]);
            pol.skipWhenSilent = pol.clampOutput = static_cast<bool>(p["skip"]);
            stub_.prepare(c.sampleRate, engine::kBlockFrames);
            shell_.prepare(stub_, pol, c.sampleRate, SlotKind::Insert, copyL_, copyR_);
            shell_.setQuirkProfile(kQuirkProfileReference);
            shell_.setParam(5, static_cast<float>(static_cast<double>(p["mix"])));
            return {};
        }
        gKind = kind == "insert" ? SlotKind::Insert : (kind == "send" ? SlotKind::Send : SlotKind::Master);
        gTempoStub = static_cast<bool>(p["tempo_stub"]);
        if (! p["bpm"].isVoid())
            ctx_.bpm = static_cast<double>(p["bpm"]);
        std::array<const EffectDescriptor*, 10> types{};
        for (std::size_t i = 0; i < types.size(); ++i)
            types[i] = &kStubs[i];
        proc_ = std::make_unique<FxProcessor>();
        proc_->build(gKind, types);
        proc_->prepare(c.sampleRate, engine::kBlockFrames);
        if (gKind == SlotKind::Send)
            proc_->setReferenceStartUp();
        return {};
    }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        for (const auto& e : events_)
        {
            if (e.block != block)
                continue;
            if (shellMode_)
            {
                if (e.what == 0)
                    shell_.activate(true);
                else if (e.what == 1)
                    shell_.stop();
                else
                    shell_.setParam(5, static_cast<float>(e.value));
                continue;
            }
            switch (e.what)
            {
            case 0:
                proc_->requestType(static_cast<int>(e.value));
                break;
            case 1:
                proc_->setBypass(e.value != 0.0);
                break;
            case 2:
                proc_->setDisabled(e.value != 0.0);
                break;
            default:
                ctx_.bpm = e.value;
                break;
            }
        }
        std::memcpy(out[0], in[0], sizeof(float) * engine::kBlockFrames);
        std::memcpy(out[1], in[1], sizeof(float) * engine::kBlockFrames);
        if (shellMode_)
        {
            shell_.process(out[0], out[1], ctx_);
            return;
        }
        if (! proc_->process(out[0], out[1], ctx_)) // a settled Off/disabled Send: silence the caller may skip
        {
            std::memset(out[0], 0, sizeof(float) * engine::kBlockFrames);
            std::memset(out[1], 0, sizeof(float) * engine::kBlockFrames);
        }
        const float cur = static_cast<float>(proc_->current());
        for (int i = 0; i < engine::kBlockFrames; ++i)
            out[2][i] = cur;
    }

private:
    bool shellMode_ = true;
    HalfStub stub_;
    FxShell shell_;
    alignas(16) float copyL_[engine::kBlockFrames]{};
    alignas(16) float copyR_[engine::kBlockFrames]{};
    std::unique_ptr<FxProcessor> proc_;
    ProcessContext ctx_{};
    std::vector<Event> events_;
};
} // namespace

std::unique_ptr<Subject> makeFxShellSubject()
{
    return std::make_unique<FxShellSubject>();
}
} // namespace golden
