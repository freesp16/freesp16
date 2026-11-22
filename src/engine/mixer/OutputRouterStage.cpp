#include "engine/mixer/OutputRouterStage.h"

#include "engine/mixer/OutputRouterKernels.h"

#include <cstring>

namespace engine::mixer
{
namespace
{
constexpr int kN = kBlockFrames;
constexpr int kPhonesBit = static_cast<int>(OutPort::Phones);

int toIndex(float v, int lo, int hi) noexcept
{
    if (! (v == v))
        return lo;
    const int i = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
    return i < lo ? lo : (i > hi ? hi : i);
}
} // namespace

OutputRouterStage::OutputRouterStage() noexcept
{
    (void) outputTrimGain(0); // the lazy table, here (message thread)
    trim_.reset(outputTrimGain(1023));
    knob_.reset(outputTrimGain(1023));
    gate_.reset(1.0f);
}

void OutputRouterStage::bindParams(const std::function<core::ParamId(std::string_view)>& resolve, // rt-ok: set-up only
                                   bridge::ParamBindings& b)
{
    if (! resolve)
        return;
    for (std::size_t i = 0; i < ids_.size(); ++i)
    {
        ids_[i] = resolve(kRoutingParamIds[i]);
        b.bind(ids_[i], bridge::ParamTarget::Routing);
    }
}

void OutputRouterStage::prepare(double sampleRate) noexcept
{
    router_.prepare(sampleRate, {});
    router_.setPresent(static_cast<std::uint8_t>(present_ & 0x0Fu));
    phones_.prepare(sampleRate);
    monitor_.prepare(sampleRate);
    trim_.reset(trim_.target());
    knob_.reset(knob_.target());
    gate_.reset(gate_.target());
    buses_ = {};
    outputs_ = {};
}

bool OutputRouterStage::onParam(const ParamChange& c) noexcept
{
    if (! c.param.valid())
        return false;
    for (std::size_t i = 0; i < ids_.size(); ++i)
        if (ids_[i].valid() && ids_[i] == c.param)
        {
            set(static_cast<RoutingParam>(i), c.scope, c.value);
            return true;
        }
    return false;
}

bool OutputRouterStage::set(RoutingParam p, std::uint16_t scope, float value) noexcept
{
    const auto trackScene = static_cast<std::size_t>((scope >> 4) & 15);
    const auto track = static_cast<std::size_t>(scope & 15);
    switch (p)
    {
        case RoutingParam::AssignMode:
            if (scope >= kTracks)
                return false;
            mode_[scope] = static_cast<std::uint8_t>(toIndex(value, 0, 1));
            if (scope == scene_)
            {
                const auto m = static_cast<AssignMode>(mode_[scope]);
                router_.setMode(m);
                phones_.setMode(m);
            }
            return true;
        case RoutingParam::OutAssign:
            if (scope > 0xFF)
                return false;
            assign_[trackScene][track] = static_cast<std::uint8_t>(toIndex(value, 0, 12));
            if (static_cast<int>(trackScene) == scene_)
                applyTrackAssign(static_cast<int>(track));
            return true;
        case RoutingParam::HeadphoneCue:
            if (scope >= kTracks)
                return false;
            cue_[scope] = static_cast<std::uint8_t>(toIndex(value, 0, kCues - 1));
            if (scope == scene_)
                phones_.setCue(cue_[scope]);
            return true;
        case RoutingParam::MonitorLevel: monitor_.setLevel(value); return true;
        case RoutingParam::Monitor: monitor_.setOn(value >= 0.5f); return true;
        case RoutingParam::ThruBypass:
        {
            if (scope > 0xFF)
                return false;
            auto& m = thruBypass_[trackScene];
            const auto bit = static_cast<std::uint16_t>(1u << track);
            m = static_cast<std::uint16_t>(value >= 0.5f ? (m | bit) : (m & ~bit));
            bypassDirty_ = bypassDirty_ || static_cast<int>(trackScene) == scene_;
            return true;
        }
        case RoutingParam::MainKnob:
        {
            const int i = toIndex(value, 0, 1023);
            knob_.setTarget(outputTrimGain(i), 0);
            gate_.setTarget(i == 0 ? 0.0f : 1.0f, 0);
            return true;
        }
        case RoutingParam::Count: break;
    }
    return false;
}

void OutputRouterStage::applyTrackAssign(int track) noexcept
{
    router_.setAssign(track, outAssignFromIndex(assign_[static_cast<std::size_t>(scene_)][static_cast<std::size_t>(track)]));
}

void OutputRouterStage::setScene(int scene) noexcept
{
    scene &= 15;
    if (scene == scene_)
        return;
    scene_ = scene;
    const auto m = static_cast<AssignMode>(mode_[static_cast<std::size_t>(scene)]);
    router_.setMode(m);
    phones_.setMode(m);
    phones_.setCue(cue_[static_cast<std::size_t>(scene)]);
    for (int t = 0; t < kTracks; ++t)
        applyTrackAssign(t);
    bypassDirty_ = true;
}

void OutputRouterStage::setPresent(std::uint8_t portMask) noexcept
{
    portMask = static_cast<std::uint8_t>(portMask | 1u);
    if (portMask == present_)
        return;
    present_ = portMask;
    router_.setPresent(static_cast<std::uint8_t>(present_ & 0x0Fu));
}

void OutputRouterStage::setOutputTrim(float index) noexcept
{
    trim_.setTarget(outputTrimGain(toIndex(index, 0, 1023)), 0);
}

void OutputRouterStage::monitor(stages::BlockBuffers& b, Mixer& m) noexcept
{
    monitor_.process(b.inputL, b.inputR, buses_);
    m.setMonitorInput(buses_.inputL, buses_.inputR);
}

void OutputRouterStage::process(const BlockContext&, stages::BlockBuffers& b, const Mixer& m) noexcept
{
    const TrackOutputs tracks {b.track.data(), m.stripOutputMask()};
    const float* master[2] = {b.master.l, b.master.r};
    router_.process(tracks, master, outputs_);
    if (((present_ >> kPhonesBit) & 1u) == 0)
        return; // no headphone port: nothing to compute
    buses_.auxL = b.auxActive ? b.aux.l : nullptr;
    buses_.auxR = b.auxActive ? b.aux.r : nullptr;
    phones_.process(outputs_, master, buses_, phones_l_, phones_r_);
}

void OutputRouterStage::output(const BlockContext&, stages::BlockBuffers& b) noexcept
{
    b.port.fill(nullptr); // FilterSend included (silent unless the analog-filter slot writes it after)
    bool silent = false;
    const float* mute = router_.muteAllGains(silent);
    const bool trimIdle = trim_.idle();
    const bool knobIdle = knob_.idle(), gateIdle = gate_.idle();
    if (! knobIdle)
        knob_.fill(knobGain_, kN);
    if (! gateIdle)
        gate_.fill(gateGain_, kN);
    if (silent)
    {
        if (! trimIdle)
            trim_.fill(nullptr, kN);
        return;
    }
    // Every output: × trim (MIX-060 / MIX-023), then × mute-all — the research device stage's order (device_out.py:
    // OutputVolume, then BulkMute), so the result is bit-exact with it (two multiplies only during a mute-all fade).
    const float tv = trim_.current();
    const bool unity = trimIdle && tv == 1.0f && mute == nullptr;
    if (! trimIdle)
        trim_.fill(gain_, kN);
    const auto emit = [&](int port, const float* l, const float* r, float* dl, float* dr) noexcept {
        if (l == nullptr || r == nullptr || ((present_ >> port) & 1u) == 0)
            return;
        // The MAIN OUT pot's factor of this port (1 = none): a constant, or a ramp (knobGain_ / gateGain_).
        const bool main = port == 0;
        const float k = main ? gate_.current() : knob_.current();
        const float* ramp = main ? (gateIdle ? nullptr : gateGain_) : (knobIdle ? nullptr : knobGain_);
        const bool knobUnity = ramp == nullptr && k == 1.0f;
        if (unity && knobUnity)
        {
            b.port[static_cast<std::size_t>(2 * port)] = l;
            b.port[static_cast<std::size_t>(2 * port + 1)] = r;
            return;
        }
        if (trimIdle)
        {
            routing::copyScale(dl, l, tv, kN);
            routing::copyScale(dr, r, tv, kN);
        }
        else
        {
            routing::copyGain(dl, l, gain_, kN);
            routing::copyGain(dr, r, gain_, kN);
        }
        if (mute != nullptr)
        {
            routing::multiply(dl, mute, kN);
            routing::multiply(dr, mute, kN);
        }
        if (ramp != nullptr)
        {
            routing::multiply(dl, ramp, kN);
            routing::multiply(dr, ramp, kN);
        }
        else if (! knobUnity)
        {
            routing::copyScale(dl, dl, k, kN);
            routing::copyScale(dr, dr, k, kN);
        }
        b.port[static_cast<std::size_t>(2 * port)] = dl;
        b.port[static_cast<std::size_t>(2 * port + 1)] = dr;
    };
    for (int p = 0; p < kRouterPairs; ++p)
        emit(p, outputs_.ch[static_cast<std::size_t>(2 * p)], outputs_.ch[static_cast<std::size_t>(2 * p + 1)],
             port_[p][0], port_[p][1]);
    emit(kPhonesBit, phones_l_, phones_r_, port_[kRouterPairs][0], port_[kRouterPairs][1]);
}
} // namespace engine::mixer
