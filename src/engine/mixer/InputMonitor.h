// engine::mixer::InputMonitor — the input monitor (spec 02-features/mixer-and-routing.md MIX-050,
// 03-ui/screens.md UI-323; audio-engine.md step 11 "Master out = MasterFx out + send return + input monitor").
//
//   monitor = input × MONITOR LEVEL × on/off fade. The level is monitorVolume 0–127 on the fader law: 100 = unity
//   ("leaves the mixer balance unchanged", UI-323), 127 = +6 dB, 0 = silence (our law), 100-sample ramp
//   (MIX-002). On/off fades over 128 samples (the mute ramp). The monitor bus feeds the master (before the master
//   fader, after the send return: Mixer::setMonitorInput) and the headphones (HeadphoneRouter, MonitorBuses::input).
//   The input is BlockBuffers::inputL/R — the sampling input stage replaces it with the post-soft-limiter input.
//   Off and settled (the default), or no input: nothing is computed and the bus is null.
//
// Threads: prepare() on the message thread, the rest on the audio thread; no allocation. Budget ≤ 300 cycles per block.
#pragma once

#include "engine/EngineApi.h"
#include "engine/mixer/HeadphoneRouter.h"
#include "engine/mixer/Ramps.h"

namespace engine::mixer
{
class InputMonitor
{
public:
    InputMonitor() noexcept;
    void prepare(double sampleRate) noexcept; // ramps settle on their targets

    // ---- audio thread ----
    void setLevel(float index0to127) noexcept;
    void setOn(bool on) noexcept;
    // One block of the input (null = no input). Sets mb.inputL/R to the monitor block (or the input itself when the
    // gain is exactly 1), null when silent.
    void process(const float* inL, const float* inR, MonitorBuses& mb) noexcept;

    bool on() const noexcept { return ! fade_.muted(); }
    int level() const noexcept { return levelIndex_; }

private:
    MuteRamp fade_;
    GainRamp level_;
    int levelIndex_ = 100;
    alignas(16) float l_[kBlockFrames] {};
    alignas(16) float r_[kBlockFrames] {};
    alignas(16) float gain_[kBlockFrames] {};
    alignas(16) float levelGain_[kBlockFrames] {};
};
} // namespace engine::mixer
