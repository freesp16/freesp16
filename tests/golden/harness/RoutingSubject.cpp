// Routing goldens: the app's engine::mixer routing against the research Selector, HeadphoneUnit
// and device output stage (tools/golden-gen/goldenlib/models/routing_{router,headphones,device}.py driving
// tools/fx/headphone_mix.py, track_mix.py, fx_processors.py and device_out.py, MIT; docs/effects/signal-flow.md
// F209-F213, F109-F113). Every subject starts settled at the case's initial parameters (prepare()), applies the
// case's events before their block (the engine's block-start drain) and runs the Reference profile unless the case
// says "profile": "app".
//
//   routing.router      OutputRouter: inputs master.L/R, t<k>.L/R; outputs out12.L/R … out78.L/R (silent = zeros).
//                       Params {"mode", "assign": [OutputAssign index per track], "present", "profile", "events":
//                       [{"block", "mode"} | {"block", "track", "assign"} | {"block", "present"}]}.
//   routing.headphones  HeadphoneRouter: inputs out12…out78 (the router's pairs), aux.L/R, input.L/R; outputs
//                       phones.L/R. Params {"mode", "cue", "level", "profile", "events": [{"block", "cue"|"mode"|
//                       "level"}]}.
//   routing.device      OutputRouterStage steps 12-13 (M + 6CH, no track, Main + Phones present): inputs master.L/R,
//                       aux.L/R; outputs main.L/R, phones.L/R. Params {"trim", "phones_level", "cue", "events":
//                       [{"block", "trim"|"mute_all"|"phones_level"|"cue"}]}.
#include "tests/golden/harness/Subject.h"

#include "engine/EngineApi.h"
#include "engine/mixer/HeadphoneRouter.h"
#include "engine/mixer/Mixer.h"
#include "engine/mixer/OutputRouter.h"
#include "engine/mixer/OutputRouterStage.h"
#include "engine/stages/BlockBuffers.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace golden
{
namespace
{
constexpr int kN = engine::kBlockFrames;
using engine::mixer::AssignMode;

struct Event
{
    int block = 0, track = -1;
    std::string key;
    int value = 0;
};

// The case's events: every property but "block" / "track" is a key (one per event in the recipes).
std::string parseEvents(const juce::var& params, std::vector<Event>& out)
{
    out.clear();
    if (const auto* ev = params["events"].getArray())
        for (const auto& e : *ev)
        {
            const auto* o = e.getDynamicObject();
            if (o == nullptr)
                return "an event is not an object";
            Event x;
            x.block = static_cast<int>(e["block"]);
            x.track = e.hasProperty("track") ? static_cast<int>(e["track"]) : -1;
            for (const auto& p : o->getProperties())
            {
                const auto name = p.name.toString().toStdString();
                if (name == "block" || name == "track")
                    continue;
                x.key = name;
                x.value = p.value.isBool() ? (static_cast<bool>(p.value) ? 1 : 0) : static_cast<int>(p.value);
            }
            if (x.key.empty())
                return "an event without a key";
            out.push_back(x);
        }
    return {};
}

int intParam(const juce::var& params, const char* key, int fallback)
{
    return params.hasProperty(key) ? static_cast<int>(params[key]) : fallback;
}

bool referenceProfile(const juce::var& params)
{
    return ! params.hasProperty("profile") || params["profile"].toString() != "app";
}

void emit(float* dst, const float* src, float scale) noexcept
{
    if (src == nullptr)
    {
        std::memset(dst, 0, sizeof(float) * kN);
        return;
    }
    for (int i = 0; i < kN; ++i)
        dst[i] = src[i] * scale; // the fault test's deliberately wrong gain (x 1 exactly otherwise)
}

class FaultScale
{
protected:
    bool fault(Fault f) noexcept
    {
        scale_ = f == Fault::Gain ? kFaultGainScale : 1.0f;
        return f == Fault::None || f == Fault::Gain;
    }
    float scale_ = 1.0f;
};

// ---- routing.router ----

class RouterSubject final : public Subject, FaultScale
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto* assign = c.params["assign"].getArray();
        if (assign == nullptr || assign->isEmpty() || assign->size() > engine::kTracks)
            return "routing.router needs 1..16 track assignments";
        numTracks_ = assign->size();
        if (c.inputNames.size() != 2u + 2u * static_cast<std::size_t>(numTracks_) || c.outputNames.size() != 8u)
            return "routing.router needs master.L/R + t<k>.L/R inputs and 8 outputs";
        router_ = std::make_unique<engine::mixer::OutputRouter>();
        tracks_ = std::make_unique<std::array<engine::stages::StereoBlock, engine::kTracks>>();
        router_->setMode(static_cast<AssignMode>(intParam(c.params, "mode", 0)));
        for (int t = 0; t < numTracks_; ++t)
            router_->setAssign(t, engine::mixer::outAssignFromIndex(static_cast<int>((*assign)[t])));
        router_->prepare(c.sampleRate, {}); // settled
        router_->setPresent(static_cast<std::uint8_t>(intParam(c.params, "present", 0x0F)));
        router_->setReferenceQuirk(referenceProfile(c.params));
        return parseEvents(c.params, events_);
    }
    bool injectFault(Fault f) noexcept override { return fault(f); }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        auto& r = *router_;
        for (const auto& e : events_)
        {
            if (e.block != block)
                continue;
            if (e.key == "mode")
                r.setMode(static_cast<AssignMode>(e.value));
            else if (e.key == "present")
                r.setPresent(static_cast<std::uint8_t>(e.value));
            else if (e.key == "assign")
                r.setAssign(e.track, engine::mixer::outAssignFromIndex(e.value));
        }
        for (int t = 0; t < numTracks_; ++t)
        {
            std::memcpy((*tracks_)[static_cast<std::size_t>(t)].l, in[2 + 2 * t], sizeof(float) * kN);
            std::memcpy((*tracks_)[static_cast<std::size_t>(t)].r, in[3 + 2 * t], sizeof(float) * kN);
        }
        const float* master[2] = {in[0], in[1]};
        engine::mixer::OutputBuffers ob;
        r.process({tracks_->data(), (1u << numTracks_) - 1u}, master, ob);
        for (int n = 0; n < 8; ++n)
            emit(out[n], ob.channel(n), scale_);
    }

private:
    std::unique_ptr<engine::mixer::OutputRouter> router_;
    std::unique_ptr<std::array<engine::stages::StereoBlock, engine::kTracks>> tracks_;
    std::vector<Event> events_;
    int numTracks_ = 0;
};

// ---- routing.headphones ----

class HeadphonesSubject final : public Subject, FaultScale
{
public:
    std::string configure(const GoldenCase& c) override
    {
        if (c.inputNames.size() != 12u || c.outputNames.size() != 2u)
            return "routing.headphones needs out12…out78, aux, input (12 inputs) and phones.L/R";
        hp_ = std::make_unique<engine::mixer::HeadphoneRouter>();
        hp_->setMode(static_cast<AssignMode>(intParam(c.params, "mode", 0)));
        hp_->setCue(intParam(c.params, "cue", 0));
        hp_->setLevel(static_cast<float>(intParam(c.params, "level", 127)));
        hp_->setReferenceQuirk(referenceProfile(c.params));
        hp_->prepare(c.sampleRate); // settled
        return parseEvents(c.params, events_);
    }
    bool injectFault(Fault f) noexcept override { return fault(f); }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        auto& h = *hp_;
        for (const auto& e : events_)
        {
            if (e.block != block)
                continue;
            if (e.key == "cue")
                h.setCue(e.value);
            else if (e.key == "mode")
                h.setMode(static_cast<AssignMode>(e.value));
            else if (e.key == "level")
                h.setLevel(static_cast<float>(e.value));
        }
        engine::mixer::OutputBuffers ob;
        for (int n = 0; n < 8; ++n)
            ob.ch[static_cast<std::size_t>(n)] = in[n];
        const engine::mixer::MonitorBuses mon {in[8], in[9], in[10], in[11]};
        h.process(ob, nullptr, mon, l_, r_);
        emit(out[0], l_, scale_);
        emit(out[1], r_, scale_);
    }

private:
    std::unique_ptr<engine::mixer::HeadphoneRouter> hp_;
    std::vector<Event> events_;
    alignas(16) float l_[kN] {};
    alignas(16) float r_[kN] {};
};

// ---- routing.device ----

class DeviceSubject final : public Subject, FaultScale
{
public:
    std::string configure(const GoldenCase& c) override
    {
        if (c.inputNames.size() != 4u || c.outputNames.size() != 4u)
            return "routing.device needs master.L/R, aux.L/R and main.L/R, phones.L/R";
        using engine::mixer::RoutingParam;
        mixer_ = std::make_unique<engine::mixer::Mixer>(); // never processed: no strip feeds the router
        stage_ = std::make_unique<engine::mixer::OutputRouterStage>();
        b_ = std::make_unique<engine::stages::BlockBuffers>();
        stage_->setPresent(static_cast<std::uint8_t>(1u << static_cast<int>(engine::OutPort::Main) |
                                                     1u << static_cast<int>(engine::OutPort::Phones)));
        stage_->setOutputTrim(static_cast<float>(intParam(c.params, "trim", 1023)));
        stage_->setHeadphoneVolume(static_cast<float>(intParam(c.params, "phones_level", 127)));
        stage_->set(RoutingParam::HeadphoneCue, 0, static_cast<float>(intParam(c.params, "cue", 0)));
        stage_->setReferenceQuirk(true);
        stage_->prepare(c.sampleRate); // settled
        return parseEvents(c.params, events_);
    }
    bool injectFault(Fault f) noexcept override { return fault(f); }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        using engine::mixer::RoutingParam;
        auto& s = *stage_;
        for (const auto& e : events_)
        {
            if (e.block != block)
                continue;
            if (e.key == "trim")
                s.setOutputTrim(static_cast<float>(e.value));
            else if (e.key == "mute_all")
                s.router().setMuteAll(e.value != 0);
            else if (e.key == "phones_level")
                s.setHeadphoneVolume(static_cast<float>(e.value));
            else if (e.key == "cue")
                s.set(RoutingParam::HeadphoneCue, 0, static_cast<float>(e.value));
        }
        auto& b = *b_;
        std::memcpy(b.master.l, in[0], sizeof(float) * kN);
        std::memcpy(b.master.r, in[1], sizeof(float) * kN);
        std::memcpy(b.aux.l, in[2], sizeof(float) * kN);
        std::memcpy(b.aux.r, in[3], sizeof(float) * kN);
        b.auxActive = true;
        s.process(ctx_, b, *mixer_);
        s.output(ctx_, b);
        const auto port = [&](engine::OutPort p, int side) {
            return b.port[static_cast<std::size_t>(2 * static_cast<int>(p) + side)];
        };
        emit(out[0], port(engine::OutPort::Main, 0), scale_);
        emit(out[1], port(engine::OutPort::Main, 1), scale_);
        emit(out[2], port(engine::OutPort::Phones, 0), scale_);
        emit(out[3], port(engine::OutPort::Phones, 1), scale_);
    }

private:
    std::unique_ptr<engine::mixer::Mixer> mixer_;
    std::unique_ptr<engine::mixer::OutputRouterStage> stage_;
    std::unique_ptr<engine::stages::BlockBuffers> b_;
    engine::BlockContext ctx_ {};
    std::vector<Event> events_;
};
} // namespace

std::unique_ptr<Subject> makeRouterSubject()
{
    return std::make_unique<RouterSubject>();
}
std::unique_ptr<Subject> makeHeadphonesSubject()
{
    return std::make_unique<HeadphonesSubject>();
}
std::unique_ptr<Subject> makeDeviceSubject()
{
    return std::make_unique<DeviceSubject>();
}
} // namespace golden
