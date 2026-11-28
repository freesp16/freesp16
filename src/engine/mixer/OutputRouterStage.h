// Steps 12–13 of the engine block (audio-engine.md "Per-block processing order"): the output router,
// the headphone cue, the input monitor and the output trim, as one plain member of engine::Engine (stages::
// OutputRouterStage). It also keeps the routing parameters of the 16 scenes (a scene switch is an index change).
//
//   before step 11  monitor(b, mixer)   InputMonitor → MonitorBuses::input and Mixer::setMonitorInput (master)
//   step 12         process(ctx, b, m)  OutputRouter (Main / OUT 3-4 / 5-6 / 7-8 from the master and the strips'
//                                       post-fader blocks), HeadphoneRouter (cue + aux + monitor × level)
//   step 13         output(ctx, b)      output trim (MIX-060, every logical output incl. headphones, MIX-023) and the
//                                       mute-all fade → BlockBuffers::port. FilterSend keeps its existing semantics:
//                                       null (silent) unless the analog-filter slot writes b.port[FilterSend] after
//                                       this step.
//
// Parameters (core/params/catalogue/routing.cpp; ParamTarget::Routing → onParam):
//   routing.assignMode   scene      0 M + 6CH, 1 8CH (MIX-030)
//   routing.outAssign    track      0 "--", 1 OUT1-2 … 4 OUT7-8, 5 … 12 mono 1 … 8 (MIX-031)
//   routing.headphoneCue scene      0 MASTER, 1 OUT1-2 … 4 OUT7-8, 5 … 12 mono 1 … 8 (MIX-040)
//   routing.monitorLevel project    0–127, 100 = unity (MIX-050, UI-323)
//   routing.monitor      global     0/1 input monitor on (MIX-050; the sampler switches it)
//   routing.thruBypass   track      0/1 Thru BYPASS (MIX-051): Engine bypasses both inserts of the Thru track
// plus mixer.headphoneVolume and mixer.outputTrim (the mixer's ids), forwarded by Engine::applyMixerGlobal.
#pragma once

#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/mixer/HeadphoneRouter.h"
#include "engine/mixer/InputMonitor.h"
#include "engine/mixer/Mixer.h"
#include "engine/mixer/OutputRouter.h"
#include "engine/mixer/Ramps.h"
#include "engine/stages/BlockBuffers.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

namespace engine::mixer
{
enum class RoutingParam : std::uint8_t
{
    AssignMode,
    OutAssign,
    HeadphoneCue,
    MonitorLevel,
    Monitor,
    ThruBypass,
    MainKnob,
    Count
};
inline constexpr std::string_view kRoutingParamIds[] = {"routing.assignMode",   "routing.outAssign",
                                                        "routing.headphoneCue", "routing.monitorLevel",
                                                        "routing.monitor",      "routing.thruBypass",
                                                        "routing.mainKnob"};
static_assert(std::size(kRoutingParamIds) == static_cast<std::size_t>(RoutingParam::Count));

class OutputRouterStage
{
public:
    OutputRouterStage() noexcept;

    // ---- message thread ----
    void bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                    bridge::ParamBindings& b);
    void prepare(double sampleRate) noexcept;

    // ---- audio thread ----
    bool onParam(const ParamChange&) noexcept;      // true = a routing.* parameter (consumed)
    bool set(RoutingParam, std::uint16_t scope, float value) noexcept; // the same without the ParamId (tests); true = applied
    void setScene(int scene) noexcept;              // the scene's mode, cue and assignments (block start)
    void swapAssignments(int scene, int a, int b) noexcept;
    void setPresent(std::uint8_t portMask) noexcept; // bit = engine::OutPort present on the device
    void setHeadphoneVolume(float index) noexcept { phones_.setLevel(index); }
    void setOutputTrim(float index1023) noexcept;
    void setReferenceQuirk(bool on) noexcept // FX-052 reference profile: the F209 selector quirk (headphones, mode)
    {
        phones_.setReferenceQuirk(on);
        router_.setReferenceQuirk(on);
    }

    void monitor(stages::BlockBuffers& b, Mixer& m) noexcept;                       // before step 11
    void process(const BlockContext&, stages::BlockBuffers& b, const Mixer& m) noexcept; // step 12
    void output(const BlockContext&, stages::BlockBuffers& b) noexcept;              // step 13

    // Thru BYPASS of the current scene (bit t), for the engine's FX bypass override.
    std::uint16_t thruBypassMask() const noexcept { return thruBypass_[static_cast<std::size_t>(scene_)]; }
    bool thruBypassChanged() noexcept
    {
        const bool c = bypassDirty_;
        bypassDirty_ = false;
        return c;
    }

    OutputRouter& router() noexcept { return router_; }
    const OutputRouter& router() const noexcept { return router_; }
    HeadphoneRouter& headphones() noexcept { return phones_; }
    InputMonitor& input() noexcept { return monitor_; }
    const OutputBuffers& outputs() const noexcept { return outputs_; }
    const float* resampleTap(int ch) const noexcept { return router_.resampleTap(ch); }
    int scene() const noexcept { return scene_; }

private:
    void applyTrackAssign(int track) noexcept;

    OutputRouter router_;
    HeadphoneRouter phones_;
    InputMonitor monitor_;
    std::array<core::ParamId, static_cast<std::size_t>(RoutingParam::Count)> ids_ {};

    // Per scene (16) / scene × track.
    std::array<std::uint8_t, kTracks> mode_ {};
    std::array<std::uint8_t, kTracks> cue_ {};
    std::array<std::array<std::uint8_t, kTracks>, kTracks> assign_ {};
    std::array<std::uint16_t, kTracks> thruBypass_ {};
    int scene_ = 0;
    bool bypassDirty_ = true;
    std::uint8_t present_ = 1; // OutPort bits

    MonitorBuses buses_ {};
    OutputBuffers outputs_ {};
    GainRamp trim_;
    // The MAIN OUT pot (routing.mainKnob): PHONES and OUT3-8 × the trim law of the pot, MAIN × 0/1 (F113). Unity at
    // the default 1023, so the device-stage goldens are unchanged.
    GainRamp knob_, gate_;
    alignas(16) float knobGain_[kBlockFrames] {};
    alignas(16) float gateGain_[kBlockFrames] {};
    alignas(16) float phones_l_[kBlockFrames] {};
    alignas(16) float phones_r_[kBlockFrames] {};
    alignas(16) float gain_[kBlockFrames] {};
    alignas(16) float port_[kRouterPairs + 1][2][kBlockFrames] {}; // trimmed Main … Out78, Phones
};
} // namespace engine::mixer
