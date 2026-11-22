#include "engine/mixer/InputMonitor.h"

#include "core/util/GainLaw.h"
#include "engine/mixer/OutputRouterKernels.h"

namespace engine::mixer
{
namespace
{
constexpr int kN = kBlockFrames;
}

InputMonitor::InputMonitor() noexcept
{
    (void) core::GainLaw::gain(0); // the lazy table, here (message thread)
    fade_.reset(true);             // off by default (the sampler switches it on, MIX-050)
    level_.reset(core::GainLaw::gain(levelIndex_));
}

void InputMonitor::prepare(double) noexcept
{
    fade_.reset(fade_.muted());
    level_.reset(level_.target());
}

void InputMonitor::setLevel(float index) noexcept
{
    int i = static_cast<int>(index < 0.0f ? index - 0.5f : index + 0.5f);
    i = i < 0 ? 0 : (i > 127 ? 127 : i);
    levelIndex_ = i;
    level_.setTarget(core::GainLaw::gain(i), 0);
}

void InputMonitor::setOn(bool on) noexcept
{
    if (on != this->on())
        fade_.set(! on, 0);
}

void InputMonitor::process(const float* inL, const float* inR, MonitorBuses& mb) noexcept
{
    mb.inputL = mb.inputR = nullptr;
    const bool input = inL != nullptr && inR != nullptr;
    const MuteRamp::Kind k = fade_.fill(input ? gain_ : nullptr, kN);
    if (! input || k == MuteRamp::Kind::Closed)
    {
        level_.fill(nullptr, kN); // the ramp advances as if it had run
        return;
    }
    const bool levelIdle = level_.idle();
    const float lv = level_.current();
    if (k == MuteRamp::Kind::Open && levelIdle)
    {
        if (lv == 1.0f)
        {
            mb.inputL = inL; // unity: the input itself (NFR-030, no copy)
            mb.inputR = inR;
            return;
        }
        routing::copyScale(l_, inL, lv, kN);
        routing::copyScale(r_, inR, lv, kN);
    }
    else
    {
        if (k == MuteRamp::Kind::Open)
            level_.fill(gain_, kN);
        else if (levelIdle)
        {
            if (lv != 1.0f)
                routing::copyScale(gain_, gain_, lv, kN);
        }
        else
        {
            level_.fill(levelGain_, kN);
            routing::multiply(gain_, levelGain_, kN);
        }
        routing::copyGain(l_, inL, gain_, kN);
        routing::copyGain(r_, inR, gain_, kN);
    }
    mb.inputL = l_;
    mb.inputR = r_;
}
} // namespace engine::mixer
