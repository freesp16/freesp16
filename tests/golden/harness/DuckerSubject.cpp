// fx.ducker: engine::fx::effects::Ducker through EffectBase::process with the Source track's trigger
// slot in the ProcessContext (the host rule of research docs/effects/ducker.md, F201), Reference quirk profile, against
// tools/golden-gen/goldenlib/models/ducker.py (which drives the research model tools/fx/ducker.py per block with
// set_param(7, bpm), (8, flag), (9, offset)). Parameters as the adapter; omitted knobs = the model's Insert defaults.
#include "tests/golden/harness/Subject.h"

#include "engine/fx/FxShell.h"
#include "engine/fx/effects/Ducker.h"

#include <cstring>
#include <vector>

namespace golden
{
namespace
{
using engine::fx::effects::Ducker;

class DuckerSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        if (c.inputNames.size() != 2 || c.outputNames.size() != 2)
            return "fx.ducker needs 2 inputs and 2 outputs (L, R)";
        const auto& p = c.params;
        effect_.prepare(c.sampleRate, engine::kBlockFrames);
        effect_.setQuirkProfile(engine::fx::kQuirkProfileReference); // goldens run the reference profile (FX-052)
        effect_.setOption(Ducker::kOptSource, 0);
        // INSERT_DEFAULTS of the model (F089), then the case's values
        const float defaults[6] = {0.03f, 0.5f, 2999.0f / 79999.0f, 0.5f, 0.0f, 1.0f};
        const char* keys[6] = {"attack", "attack_curve", "release", "release_curve", "", "depth"};
        for (int k = 0; k < 6; ++k)
        {
            if (k == 4)
                continue;
            const auto& v = p[keys[k]];
            effect_.setParam(k, v.isVoid() ? defaults[k] : static_cast<float>(static_cast<double>(v)));
        }
        effect_.setOption(Ducker::kOptSync, 0);
        fs_ = c.sampleRate;
        bpm_ = p["bpm"].isVoid() ? 120.0 : static_cast<double>(p["bpm"]);
        triggers_.assign(static_cast<std::size_t>(c.blocks), -1);
        if (auto* t = p["triggers"].getArray())
            for (auto& e : *t)
            {
                const int b = static_cast<int>(e["block"]);
                if (b >= 0 && b < c.blocks)
                    triggers_[static_cast<std::size_t>(b)] = static_cast<int>(e["offset"]);
            }
        events_.clear();
        if (auto* ev = p["events"].getArray())
            for (auto& e : *ev)
                events_.push_back({static_cast<int>(e["block"]), static_cast<int>(e["k"]),
                                   static_cast<float>(static_cast<double>(e["v"]))});
        resets_.clear();
        if (auto* rs = p["resets"].getArray())
            for (auto& r : *rs)
                resets_.push_back(static_cast<int>(r));
        return {};
    }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        for (int r : resets_)
            if (r == block)
                effect_.reset();
        for (const auto& e : events_)
            if (e.block == block)
                effect_.setParam(e.k, e.v);
        engine::fx::ProcessContext ctx{fs_, bpm_, true, 0u, {}, 0};
        ctx.triggerOffset.fill(-1);
        const int o = triggers_[static_cast<std::size_t>(block)];
        if (o >= 0)
        {
            ctx.triggerMask = 1u;
            ctx.triggerOffset[0] = static_cast<std::int16_t>(o);
        }
        std::memcpy(out[0], in[0], sizeof(float) * engine::kBlockFrames);
        std::memcpy(out[1], in[1], sizeof(float) * engine::kBlockFrames);
        effect_.process(out[0], out[1], ctx);
    }

private:
    struct Event
    {
        int block, k;
        float v;
    };
    Ducker effect_;
    double fs_ = 44100.0, bpm_ = 120.0;
    std::vector<int> triggers_, resets_;
    std::vector<Event> events_;
};
} // namespace

std::unique_ptr<Subject> makeDuckerSubject()
{
    return std::make_unique<DuckerSubject>();
}
} // namespace golden
