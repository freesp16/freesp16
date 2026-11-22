// fx.mod: a tempo-fed, retriggerable registered effect (Chorus, Flanger, Phaser), built from its
// EffectDescriptor, wrapped in engine::fx::FxShell with the descriptor's shell policy (Insert slot) in the Reference
// quirk profile (FX-052), against the research model wrapped in tools/fx/effectbase.py (tools/golden-gen/goldenlib/
// modrun.py, which documents the case parameters):
//   {"effect": id, "fs": Hz, "bpm": x, "parity_maths": bool, "init": {"k": v}, "options": {"i": n}, "activate": bool,
//    "events": [{"block", "k", "v"} | {"block", "option", "value"} | {"block", "bpm"} |
//               {"block", "op": "activate"|"stop"|"retrigger"}]}
// fx.effect (EffectSubject.cpp) plus the tempo of the ProcessContext (FX-049: every block, as the processor feeds it,
// clamped 1…999 like FxProcessor) and EffectBase::retrigger() (FX-089/090, as FxProcessor::retrigger calls it). The
// knobs start at the descriptor's ParamSpec defaults (the Insert slot's, core::fx::effectParams), then "init".
#include "tests/golden/harness/Subject.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/FxProcessor.h"
#include "engine/fx/FxShell.h"

#include <cstring>
#include <memory>

namespace golden
{
namespace
{
using namespace engine::fx;

constexpr int kParityMathsOption = 90; // engine::fx::Phaser::kOptParityMaths (FX-054 test hook)

struct Event
{
    int block = 0, what = 0; // 0 knob, 1 option, 2 activate, 3 stop, 4 retrigger, 5 bpm
    int index = 0;
    double value = 0.0;
};

class ModSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto& p = c.params;
        const auto id = p["effect"].toString().toStdString();
        const auto* d = EffectRegistry::builtin().find(id);
        if (d == nullptr || d->create == nullptr)
            return "fx.mod: no registered effect '" + id + "'";
        if (c.inputNames.size() != 2 || c.outputNames.size() != 2)
            return "fx.mod needs 2 inputs and 2 outputs (L, R)";
        const double fs = p["fs"].isVoid() ? c.sampleRate : static_cast<double>(p["fs"]);
        effect_ = d->create();
        effect_->prepare(fs, engine::kBlockFrames);
        if (! p["parity_maths"].isVoid() && static_cast<bool>(p["parity_maths"]))
            effect_->setOption(kParityMathsOption, 1);
        shell_.prepare(*effect_, d->shell, fs, SlotKind::Insert, copyL_, copyR_);
        shell_.setQuirkProfile(kQuirkProfileReference);
        ctx_ = ProcessContext{fs, 120.0, true, 0u, {}, 0};
        ctx_.triggerOffset.fill(-1);
        if (! p["bpm"].isVoid())
            ctx_.bpm = FxProcessor::clampTempo(static_cast<double>(p["bpm"]));
        float init[6]{};
        (void) core::builtinParams(); // runs the catalogues, so effectParams() knows the effect
        if (const auto* set = core::fx::effectParams(id))
            for (const auto& def : set->params)
                if (def.k >= 0 && def.k < 6)
                    init[def.k] = core::fx::defaultFor(def, core::fx::kSlotInsert);
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
                {
                    const auto op = e["op"].toString();
                    x.what = op == "activate" ? 2 : (op == "stop" ? 3 : 4);
                }
                else if (e.hasProperty("bpm"))
                {
                    x.what = 5;
                    x.value = static_cast<double>(e["bpm"]);
                }
                else if (e.hasProperty("k"))
                {
                    x.what = 0;
                    x.index = static_cast<int>(e["k"]);
                    x.value = static_cast<double>(e["v"]);
                }
                else
                {
                    x.what = 1;
                    x.index = static_cast<int>(e["option"]);
                    x.value = static_cast<double>(static_cast<int>(e["value"]));
                }
                events_.push_back(x);
            }
        if (p["activate"].isVoid() || static_cast<bool>(p["activate"]))
            shell_.activate(true);
        return {};
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
                shell_.setParam(e.index, static_cast<float>(e.value));
                break;
            case 1:
                shell_.setOption(e.index, static_cast<int>(e.value));
                break;
            case 2:
                shell_.activate(true);
                break;
            case 3:
                shell_.stop();
                break;
            case 4:
                effect_->retrigger();
                break;
            default:
                ctx_.bpm = FxProcessor::clampTempo(e.value);
                break;
            }
        }
        std::memcpy(out[0], in[0], sizeof(float) * engine::kBlockFrames);
        std::memcpy(out[1], in[1], sizeof(float) * engine::kBlockFrames);
        shell_.process(out[0], out[1], ctx_);
    }

private:
    std::unique_ptr<EffectBase> effect_;
    FxShell shell_;
    ProcessContext ctx_{};
    std::vector<Event> events_;
    alignas(16) float copyL_[engine::kBlockFrames]{};
    alignas(16) float copyR_[engine::kBlockFrames]{};
};
} // namespace

std::unique_ptr<Subject> makeModSubject()
{
    return std::make_unique<ModSubject>();
}
} // namespace golden
