// fx.oscillator: engine::fx::ModOscillator (src/engine/fx/effects/Oscillator.h) with the app's random
// table (FX-1305) in the Reference quirk profile, against the research model tools/fx/osc.py
// (tools/golden-gen/goldenlib/models/oscillator.py documents the parameters, the set-up order and the stride).
// The golden records the FNV-1a hash of the table the model ran with; tests/unit/fx_color/test_oscillator.cpp checks
// the compiled kOscRandomTable against the same formula and hash.
#include "tests/golden/harness/Subject.h"

#include "engine/EngineApi.h"
#include "engine/fx/effects/Oscillator.h"

#include <cstring>

namespace golden
{
namespace
{
using namespace engine::fx;

enum class Op
{
    SetFrequency,
    SetPeriodMs,
    SetPhaseOffset,
    ResetPhase,
    SetSmoothingHz,
    SetAmplitude,
    SetOffset,
    Hold,
    ClearHold,
    SmoothingOff
};

struct Event
{
    int block = 0;
    Op op = Op::SetFrequency;
    float v = 0.0f;
};

class OscillatorSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto& p = c.params;
        if (! c.inputNames.empty() || c.outputNames.size() != 2)
            return "fx.oscillator has no inputs and 2 outputs (L, R)";
        const double fs = p["fs"].isVoid() ? c.sampleRate : static_cast<double>(p["fs"]);
        auto f = [&](const char* key, double def)
        { return static_cast<float>(p[key].isVoid() ? def : static_cast<double>(p[key])); };
        osc_ = ModOscillator{};
        osc_.setQuirkProfile(0xffffffffu);
        osc_.setRandomOffset(static_cast<std::uint32_t>(static_cast<int>(f("rand_offset", 0.0))));
        if (! osc_.init(static_cast<int>(p["wave"]), f("freq", 1.0), fs))
            return "fx.oscillator: invalid set-up";
        osc_.setAmplitude(f("amp", 1.0));
        osc_.setOffset(f("offset", 0.0));
        if (! p["phase"].isVoid())
            osc_.setPhaseOffset(f("phase", 0.0));
        if (! p["smoothing_hz"].isVoid())
            osc_.setSmoothingHz(f("smoothing_hz", 50.0));
        if (! p["smoothing_off"].isVoid() && static_cast<bool>(p["smoothing_off"]))
            osc_.setSmoothingOff(true);
        if (! p["reset_phase"].isVoid())
            osc_.resetPhase(f("reset_phase", 0.0));
        stride_ = p["stride"].isVoid() ? 1 : static_cast<int>(p["stride"]);
        if (stride_ < 1 || 64 % stride_ != 0)
            return "fx.oscillator: stride must divide 64";
        events_.clear();
        if (auto* ev = p["events"].getArray())
            for (auto& e : *ev)
            {
                const auto op = e["op"].toString();
                Event x;
                x.block = static_cast<int>(e["block"]);
                x.v = static_cast<float>(e["v"].isVoid() ? 0.0 : static_cast<double>(e["v"]));
                x.op = op == "set_frequency"      ? Op::SetFrequency
                       : op == "set_period_ms"    ? Op::SetPeriodMs
                       : op == "set_phase_offset" ? Op::SetPhaseOffset
                       : op == "reset_phase"      ? Op::ResetPhase
                       : op == "set_smoothing_hz" ? Op::SetSmoothingHz
                       : op == "set_amplitude"    ? Op::SetAmplitude
                       : op == "set_offset"       ? Op::SetOffset
                       : op == "hold"             ? Op::Hold
                       : op == "clear_hold"       ? Op::ClearHold
                                                  : Op::SmoothingOff;
                if (op == "smoothing_off")
                    x.v = static_cast<bool>(e["v"]) ? 1.0f : 0.0f;
                events_.push_back(x);
            }
        return {};
    }
    void process(int block, const float* const*, float* const* out) noexcept override
    {
        for (const auto& e : events_)
        {
            if (e.block != block)
                continue;
            switch (e.op)
            {
            case Op::SetFrequency:
                osc_.setFrequency(e.v);
                break;
            case Op::SetPeriodMs:
                osc_.setPeriodMs(e.v);
                break;
            case Op::SetPhaseOffset:
                osc_.setPhaseOffset(e.v);
                break;
            case Op::ResetPhase:
                osc_.resetPhase(e.v);
                break;
            case Op::SetSmoothingHz:
                osc_.setSmoothingHz(e.v);
                break;
            case Op::SetAmplitude:
                osc_.setAmplitude(e.v);
                break;
            case Op::SetOffset:
                osc_.setOffset(e.v);
                break;
            case Op::Hold:
                osc_.setHold();
                break;
            case Op::ClearHold:
                osc_.clearHold();
                break;
            case Op::SmoothingOff:
                osc_.setSmoothingOff(e.v != 0.0f);
                break;
            }
        }
        float l[engine::kBlockFrames], r[engine::kBlockFrames];
        for (int s = 0; s < stride_; ++s)
        {
            osc_.process(l, r, engine::kBlockFrames);
            for (int j = 0; j < engine::kBlockFrames / stride_; ++j)
            {
                const int frame = s * (engine::kBlockFrames / stride_) + j;
                out[0][frame] = l[stride_ * j + stride_ - 1];
                out[1][frame] = r[stride_ * j + stride_ - 1];
            }
        }
    }

private:
    ModOscillator osc_;
    int stride_ = 1;
    std::vector<Event> events_;
};
} // namespace

std::unique_ptr<Subject> makeOscillatorSubject()
{
    return std::make_unique<OscillatorSubject>();
}

} // namespace golden
