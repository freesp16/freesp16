// fx.compressor: engine::fx::effects::Compressor (default maths) against the app-maths model
// tools/golden-gen/goldenlib/models/compressor.py (research tools/fx/compressor.py with crmath.expf). Output = the
// slot-18 mix with the post-stage neutral, out = f32(f32(in·dry) + f32(y·wet)), dry/wet from the effect after each
// block (telemetry kTelemetryDry/Wet, the EffectDefined law). Reference quirk profile (FX-052).
#include "tests/golden/harness/Subject.h"

#include "engine/fx/FxShell.h"
#include "engine/fx/effects/Compressor.h"

#include <cstring>
#include <vector>

namespace golden
{
namespace
{
using engine::fx::effects::Compressor;

class CompressorSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        if (c.inputNames.size() != 2 || c.outputNames.size() != 2)
            return "fx.compressor needs 2 inputs and 2 outputs (L, R)";
        const auto& p = c.params;
        effect_.setQuirkProfile(engine::fx::kQuirkProfileReference);
        effect_.prepare(c.sampleRate, engine::kBlockFrames);
        const float defaults[6] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}; // the model's INSERT_DEFAULTS
        const char* keys[6] = {"threshold", "ratio", "attack", "release", "gain", "mix"};
        for (int k = 0; k < 6; ++k)
        {
            const auto& v = p[keys[k]];
            effect_.setParam(k, v.isVoid() ? defaults[k] : static_cast<float>(static_cast<double>(v)));
        }
        set("knee", p["knee"].isVoid() ? 0.0 : static_cast<double>(p["knee"]));
        set("detect", p["detect"].isVoid() ? 0.0 : static_cast<double>(p["detect"]));
        if (! p["lookahead"].isVoid())
            set("lookahead", static_cast<double>(p["lookahead"]));
        events_.clear();
        if (auto* ev = p["events"].getArray())
            for (auto& e : *ev)
            {
                Event x{static_cast<int>(e["block"]), -1, {}, static_cast<double>(e["v"])};
                if (e["k"].isString())
                    x.option = e["k"].toString().toStdString();
                else
                    x.k = static_cast<int>(e["k"]);
                events_.push_back(x);
            }
        return {};
    }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        for (const auto& e : events_)
            if (e.block == block)
            {
                if (e.k >= 0)
                    effect_.setParam(e.k, static_cast<float>(e.v));
                else
                    set(e.option, e.v);
            }
        std::memcpy(out[0], in[0], sizeof(float) * engine::kBlockFrames);
        std::memcpy(out[1], in[1], sizeof(float) * engine::kBlockFrames);
        effect_.process(out[0], out[1], ctx_);
        const float d = effect_.telemetry(engine::fx::kTelemetryDry), w = effect_.telemetry(engine::fx::kTelemetryWet);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < engine::kBlockFrames; ++i)
            {
                const float a = in[ch][i] * d;
                const float b = out[ch][i] * w;
                out[ch][i] = a + b;
            }
    }

private:
    void set(const std::string& key, double v) noexcept
    {
        const int i = v != 0.0 ? 1 : 0;
        if (key == "knee")
            effect_.setOption(Compressor::kOptKnee, i);
        else if (key == "detect")
            effect_.setOption(Compressor::kOptDetect, i);
        else if (key == "lookahead")
            effect_.setOption(Compressor::kOptLookAhead, i);
    }
    struct Event
    {
        int block, k;
        std::string option;
        double v;
    };
    Compressor effect_;
    engine::fx::ProcessContext ctx_{44100.0, 120.0, true, 0u, {}, 0};
    std::vector<Event> events_;
};
} // namespace

std::unique_ptr<Subject> makeCompressorSubject()
{
    return std::make_unique<CompressorSubject>();
}
} // namespace golden
