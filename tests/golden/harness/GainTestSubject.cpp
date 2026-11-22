// fx.gain_test: GainTest through the EffectBase contract (prepare, setParam per block, process 64 frames in place).
// Parameters as tools/golden-gen/goldenlib/models/gain_test.py: {"index": k, "events": [{"block": b, "index": k}]};
// an index travels as value01 = float(k / 127.0), as the model passes it.
#include "tests/golden/harness/GainTest.h"
#include "tests/golden/harness/Subject.h"

#include <cstring>

namespace golden
{
namespace
{
class GainTestSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        if (c.inputNames.size() != 2 || c.outputNames.size() != 2)
            return "fx.gain_test needs 2 inputs and 2 outputs (L, R)";
        effect_.prepare(c.sampleRate, engine::kBlockFrames);
        ctx_ = engine::fx::ProcessContext{c.sampleRate, 120.0, false, 0u, {}, 0};
        ctx_.triggerOffset.fill(-1);
        events_.clear();
        if (auto* ev = c.params["events"].getArray())
            for (auto& e : *ev)
                events_.push_back({static_cast<int>(e["block"]), value01(static_cast<int>(e["index"]))});
        const auto& init = c.params["index"];
        effect_.setOption(GainTest::kBrokenGain, broken_ ? 1 : 0);
        effect_.setParam(0, value01(init.isVoid() ? GainTest::kDefaultIndex : static_cast<int>(init)));
        effect_.reset(); // the initial index applies without a ramp
        return {};
    }
    bool injectFault(Fault f) noexcept override
    {
        broken_ = f == Fault::Gain;
        return f == Fault::None || f == Fault::Gain;
    }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        for (const auto& e : events_)
            if (e.block == block)
                effect_.setParam(0, e.value01);
        std::memcpy(out[0], in[0], sizeof(float) * engine::kBlockFrames);
        std::memcpy(out[1], in[1], sizeof(float) * engine::kBlockFrames);
        effect_.process(out[0], out[1], ctx_);
    }

private:
    static float value01(int index) { return static_cast<float>(index / 127.0); }
    struct Event
    {
        int block;
        float value01;
    };
    GainTest effect_;
    engine::fx::ProcessContext ctx_{};
    std::vector<Event> events_;
    bool broken_ = false;
};
} // namespace

std::unique_ptr<Subject> makeGainTestSubject()
{
    return std::make_unique<GainTestSubject>();
}
} // namespace golden
