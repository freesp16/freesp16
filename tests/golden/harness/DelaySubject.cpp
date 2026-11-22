// fx.delay: a registered effect (the Insert Delay "delay", the Send/Master Delay "delayBus", the Reverb
// "reverb") built from its EffectDescriptor and wrapped in engine::fx::FxShell with the descriptor's shell policy, in a
// chosen slot kind and the Reference quirk profile (FX-052), with a tempo feed — against the research model run by
// tools/golden-gen/goldenlib/delayrun.py, which documents the case parameters:
//   {"effect": id, "slot": "insert"|"send"|"master", "fs": Hz, "bpm": BPM, "init": {"k": v}, "options": {"i": n},
//    "activate": bool, "constants": {...} (reverb only: ReverbConstants, tests/golden/reverb),
//    "events": [{"block", "k", "v"} | {"block", "option", "value"} | {"block", "op": "activate"|"stop"} |
//               {"block", "bpm"}]}
// The knobs start at the descriptor's ParamSpec defaults of the slot kind (core::fx::effectParams), then "init". The
// tempo is ProcessContext::bpm (the processors' parameter-7 feed, F017), as given (the processor clamps to 1…999).
#include "tests/golden/harness/Subject.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/FxShell.h"
#include "engine/fx/effects/Reverb.h"

#include <cstring>
#include <memory>

namespace golden
{
namespace
{
using namespace engine::fx;

struct Event
{
    int block = 0, what = 0; // 0 knob, 1 option, 2 activate, 3 stop, 4 bpm
    int index = 0;
    float value = 0.0f;
    double bpm = 120.0;
};

template <std::size_t N> bool readArray(const juce::var& v, std::array<float, N>& out)
{
    const auto* a = v.getArray();
    if (a == nullptr || a->size() != static_cast<int>(N))
        return false;
    for (std::size_t i = 0; i < N; ++i)
        out[i] = static_cast<float>(static_cast<double>((*a)[static_cast<int>(i)]));
    return true;
}

class DelaySubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto& p = c.params;
        const auto id = p["effect"].toString().toStdString();
        const auto* d = EffectRegistry::builtin().find(id);
        if (d == nullptr || d->create == nullptr)
            return "fx.delay: no registered effect '" + id + "'";
        if (c.inputNames.size() != 2 || c.outputNames.size() != 2)
            return "fx.delay needs 2 inputs and 2 outputs (L, R)";
        const auto slot = p["slot"].isVoid() ? std::string("insert") : p["slot"].toString().toStdString();
        SlotKind kind = SlotKind::Insert;
        std::uint8_t slotBit = core::fx::kSlotInsert;
        if (slot == "send")
        {
            kind = SlotKind::Send;
            slotBit = core::fx::kSlotSend;
        }
        else if (slot == "master")
        {
            kind = SlotKind::Master;
            slotBit = core::fx::kSlotMaster;
        }
        else if (slot != "insert")
            return "fx.delay: slot must be insert, send or master";
        if (! allows(d->slots, kind))
            return "fx.delay: '" + id + "' is not allowed in slot " + slot;
        const double fs = p["fs"].isVoid() ? c.sampleRate : static_cast<double>(p["fs"]);
        effect_ = d->create();
        if (auto* k = p["constants"].getDynamicObject())
        {
            auto* rv = dynamic_cast<Reverb*>(effect_.get());
            if (rv == nullptr)
                return "fx.delay: constants are for the reverb only";
            ReverbConstants rc;
            if (! readArray(k->getProperty("combL"), rc.combL) || ! readArray(k->getProperty("combR"), rc.combR) ||
                ! readArray(k->getProperty("apL"), rc.apL) || ! readArray(k->getProperty("apR"), rc.apR) ||
                ! readArray(k->getProperty("t60v"), rc.t60v) || ! readArray(k->getProperty("t60s"), rc.t60s))
                return "fx.delay: constants need combL/combR (5), apL/apR (2), t60v/t60s (5)";
            rv->overrideConstants(rc);
        }
        effect_->prepare(fs, engine::kBlockFrames);
        shell_.prepare(*effect_, d->shell, fs, kind, copyL_, copyR_);
        shell_.setQuirkProfile(kQuirkProfileReference);
        honourSkip_ = kind != SlotKind::Send; // the Send processor activates without the skip (reference)
        ctx_ = ProcessContext{fs, p["bpm"].isVoid() ? 120.0 : static_cast<double>(p["bpm"]), false, 0u, {}, 0};
        ctx_.triggerOffset.fill(-1);
        float init[6]{};
        (void) core::builtinParams();
        if (const auto* set = core::fx::effectParams(id))
            for (const auto& def : set->params)
                if (def.k >= 0 && def.k < 6)
                    init[def.k] = core::fx::defaultFor(def, slotBit);
        if (auto* o = p["init"].getDynamicObject())
            for (const auto& kv : o->getProperties())
                init[kv.name.toString().getIntValue()] = static_cast<float>(static_cast<double>(kv.value));
        for (int k = 0; k < 6; ++k)
            shell_.setParam(k, init[k]);
        if (auto* o = p["options"].getDynamicObject())
            for (const auto& kv : o->getProperties())
                shell_.setOption(kv.name.toString().getIntValue(), static_cast<int>(kv.value));
        events_.clear();
        if (auto* ev = p["events"].getArray())
            for (auto& e : *ev)
            {
                Event x;
                x.block = static_cast<int>(e["block"]);
                if (e.hasProperty("op"))
                    x.what = e["op"].toString() == "activate" ? 2 : 3;
                else if (e.hasProperty("bpm"))
                {
                    x.what = 4;
                    x.bpm = static_cast<double>(e["bpm"]);
                }
                else if (e.hasProperty("k"))
                {
                    x.index = static_cast<int>(e["k"]);
                    x.value = static_cast<float>(static_cast<double>(e["v"]));
                }
                else
                {
                    x.what = 1;
                    x.index = static_cast<int>(e["option"]);
                    x.value = static_cast<float>(static_cast<int>(e["value"]));
                }
                events_.push_back(x);
            }
        if (p["activate"].isVoid() || static_cast<bool>(p["activate"]))
            shell_.activate(honourSkip_);
        return {};
    }
    bool injectFault(Fault f) noexcept override
    {
        fault_ = f;
        return f == Fault::None || f == Fault::Gain;
    }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        for (const auto& e : events_)
        {
            if (e.block != block)
                continue;
            switch (e.what)
            {
            case 0:
                shell_.setParam(e.index, e.value);
                break;
            case 1:
                shell_.setOption(e.index, static_cast<int>(e.value));
                break;
            case 2:
                shell_.activate(honourSkip_);
                break;
            case 3:
                shell_.stop();
                break;
            default:
                ctx_.bpm = e.bpm;
                break;
            }
        }
        std::memcpy(out[0], in[0], sizeof(float) * engine::kBlockFrames);
        std::memcpy(out[1], in[1], sizeof(float) * engine::kBlockFrames);
        shell_.process(out[0], out[1], ctx_);
        if (fault_ == Fault::Gain)
            for (int i = 0; i < engine::kBlockFrames; ++i)
            {
                out[0][i] *= kFaultGainScale;
                out[1][i] *= kFaultGainScale;
            }
    }

private:
    std::unique_ptr<EffectBase> effect_;
    FxShell shell_;
    ProcessContext ctx_{};
    std::vector<Event> events_;
    bool honourSkip_ = true;
    Fault fault_ = Fault::None;
    alignas(16) float copyL_[engine::kBlockFrames]{};
    alignas(16) float copyR_[engine::kBlockFrames]{};
};
} // namespace

std::unique_ptr<Subject> makeDelaySubject()
{
    return std::make_unique<DelaySubject>();
}
} // namespace golden
