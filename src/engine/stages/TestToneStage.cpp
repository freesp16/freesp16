#include "engine/stages/TestToneStage.h"

#include "core/util/GainLaw.h"

#include <cmath>
#include <cstring>

namespace engine::stages
{
void TestToneStage::Rotor::setFrequency(double hz, double sampleRate) noexcept
{
    const double w = 2.0 * 3.14159265358979323846 * hz / sampleRate;
    dc = static_cast<float>(std::cos(w));
    ds = static_cast<float>(std::sin(w));
}

void TestToneStage::Rotor::renormalise() noexcept
{
    const float k = 1.5f - 0.5f * (c * c + s * s); // first-order radius correction, once per block (no sqrt)
    c *= k;
    s *= k;
}

void TestToneStage::prepare(double sampleRate) noexcept
{
    tone_ = {};
    ping_ = {};
    tone_.setFrequency(static_cast<double>(kFrequency), sampleRate);
    ping_.setFrequency(2.0 * static_cast<double>(kFrequency), sampleRate);
    gain_.reset(kLevel * core::GainLaw::gain(100)); // also initialises the GainLaw table off the audio thread
    pingEnv_ = 0.0f;
    pingDecay_ = static_cast<float>(std::pow(1e-3, 1.0 / (0.3 * sampleRate))); // −60 dB in 300 ms
}

void TestToneStage::setVolumeIndex(float index, int rampSamples) noexcept
{
    const int i = index <= 0.0f ? 0 : static_cast<int>(index + 0.5f);
    gain_.setTarget(kLevel * core::GainLaw::gain(i), rampSamples);
}

void TestToneStage::trigger(std::uint8_t velocity) noexcept
{
    pingEnv_ = 0.5f * static_cast<float>(velocity) / 127.0f;
    ping_.c = 1.0f;
    ping_.s = 0.0f;
}

bool TestToneStage::process(StereoBlock& out) noexcept
{
    const bool tone = toneEnabled_;
    if (! tone && pingEnv_ == 0.0f)
    {
        gain_.skip(kBlockFrames);
        return false;
    }
    float* l = out.l;
    const bool ramp = ! gain_.isConstant();
    if (ramp)
        gain_.fill(gainBuf_, kBlockFrames);
    const float g = tone ? gain_.current() : 0.0f;
    float tc = tone_.c, ts = tone_.s, pc = ping_.c, ps = ping_.s, env = pingEnv_;
    const float tdc = tone_.dc, tds = tone_.ds, pdc = ping_.dc, pds = ping_.ds, decay = pingDecay_;
    for (int i = 0; i < kBlockFrames; ++i)
    {
        const float gi = ramp ? (tone ? gainBuf_[i] : 0.0f) : g;
        l[i] = ts * gi + ps * env;
        const float tc2 = tc * tdc - ts * tds;
        ts = ts * tdc + tc * tds;
        tc = tc2;
        const float pc2 = pc * pdc - ps * pds;
        ps = ps * pdc + pc * pds;
        pc = pc2;
        env *= decay;
    }
    std::memcpy(out.r, l, sizeof(out.r));
    tone_.c = tc;
    tone_.s = ts;
    ping_.c = pc;
    ping_.s = ps;
    pingEnv_ = env < 1e-6f ? 0.0f : env;
    tone_.renormalise();
    ping_.renormalise();
    return true;
}
} // namespace engine::stages
