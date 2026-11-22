// fx.effect (usable by every effect): a registered engine::fx effect, built from its
// EffectDescriptor, wrapped in engine::fx::FxShell with the descriptor's shell policy (Insert slot) in the Reference
// quirk profile (FX-052), against the research effect model wrapped in tools/fx/effectbase.py
// (tools/golden-gen/goldenlib/effectrun.py, which documents the case parameters):
//   {"effect": id, "fs": Hz, "init": {"k": v}, "options": {"i": n}, "activate": bool,
//    "events": [{"block", "k", "v"} | {"block", "option", "value"} | {"block", "op": "activate"|"stop"}]}
// The knobs start at the descriptor's ParamSpec defaults (the Insert slot's, core::fx::effectParams), then "init".
#include "tests/golden/harness/Subject.h"

#include "core/params/ParamRegistry.h"
#include "core/params/catalogue/fx.h"
#include "engine/fx/EffectRegistry.h"
#include "engine/fx/FxShell.h"

#include <cstring>
#include <memory>

namespace golden
{
namespace
{
using namespace engine::fx;

struct Event
{
    int block = 0, what = 0; // 0 knob, 1 option, 2 activate, 3 stop
    int index = 0;
    float value = 0.0f;
};

class EffectSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto& p = c.params;
        const auto id = p["effect"].toString().toStdString();
        const auto* d = EffectRegistry::builtin().find(id);
        if (d == nullptr || d->create == nullptr)
            return "fx.effect: no registered effect '" + id + "'";
        if (c.inputNames.size() != 2 || c.outputNames.size() != 2)
            return "fx.effect needs 2 inputs and 2 outputs (L, R)";
        const double fs = p["fs"].isVoid() ? c.sampleRate : static_cast<double>(p["fs"]);
        effect_ = d->create();
        effect_->prepare(fs, engine::kBlockFrames);
        shell_.prepare(*effect_, d->shell, fs, SlotKind::Insert, copyL_, copyR_);
        shell_.setQuirkProfile(kQuirkProfileReference);
        ctx_ = ProcessContext{fs, 120.0, false, 0u, {}, 0};
        ctx_.triggerOffset.fill(-1);
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
                    x.what = e["op"].toString() == "activate" ? 2 : 3;
                else if (e.hasProperty("k"))
                {
                    x.what = 0;
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
                shell_.setParam(e.index, e.value);
                break;
            case 1:
                shell_.setOption(e.index, static_cast<int>(e.value));
                break;
            case 2:
                shell_.activate(true);
                break;
            default:
                shell_.stop();
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

std::unique_ptr<Subject> makeEffectSubject()
{
    return std::make_unique<EffectSubject>();
}
} // namespace golden
