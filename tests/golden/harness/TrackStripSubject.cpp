// mixer.track_strip: the app's engine::mixer::Mixer against the research strip and bus chain
// (tools/golden-gen/goldenlib/models/track_strip.py driving tools/fx/track_mix.py and bus_mix.py, MIT, with
// core::GainLaw). The subject is the real mixer, which starts settled at the case's initial parameters (no
// start-up ramps; the adapter settles the model the same way).
//
// Parameters: {"tracks": [{"fader", "pan", "send", "main_on"}], "events": [{"block", "track", "offset"?, "fader"|"pan"|
// "send"|"main_on"} | {"block", "offset"?, "master"|"return"}], "bus"?: {"master", "return", "send_fx": "off"|"half",
// "master_fx": "none"|"swap"}}. Inputs t<k>.L/R; outputs main.L, main.R, send.L, send.R (the bus sums after the
// strips), and master.L, master.R with a bus (main → Master FX → + send return → master fader; trim at 0 dB, no aux,
// limiter off). Events are applied before their block, as the engine's FIFO drain does, with their frame offsets.
#include "tests/golden/harness/Subject.h"

#include "engine/EngineApi.h"
#include "engine/mixer/Mixer.h"
#include "engine/stages/BlockBuffers.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace golden
{
namespace
{
constexpr int kN = engine::kBlockFrames;

struct Half final : engine::mixer::BusInsert // Send FX stand-in: x * 0.5
{
    void process(float* l, float* r) noexcept override
    {
        for (int i = 0; i < kN; ++i)
        {
            l[i] *= 0.5f;
            r[i] *= 0.5f;
        }
    }
};

struct Swap final : engine::mixer::BusInsert // Master FX stand-in: (L, R) -> (R, L)
{
    void process(float* l, float* r) noexcept override
    {
        for (int i = 0; i < kN; ++i)
            std::swap(l[i], r[i]);
    }
};

class TrackStripSubject final : public Subject
{
public:
    std::string configure(const GoldenCase& c) override
    {
        const auto* tracks = c.params["tracks"].getArray();
        if (tracks == nullptr || tracks->isEmpty() || tracks->size() > engine::kTracks)
            return "mixer.track_strip needs 1..16 tracks";
        const auto& busVar = c.params["bus"];
        hasBus_ = busVar.isObject();
        if (c.inputNames.size() != 2 * static_cast<std::size_t>(tracks->size()) ||
            c.outputNames.size() != (hasBus_ ? 6u : 4u))
            return "mixer.track_strip needs t<k>.L/R inputs per track and main.L/R, send.L/R (+ master.L/R) outputs";
        numTracks_ = tracks->size();
        mixer_ = std::make_unique<engine::mixer::Mixer>();
        buffers_ = std::make_unique<engine::stages::BlockBuffers>();
        hooks_ = {};
        for (int t = 0; t < numTracks_; ++t)
        {
            const auto& p = (*tracks)[t];
            engine::mixer::StripParams s;
            s.volume = static_cast<std::uint8_t>(p.hasProperty("fader") ? static_cast<int>(p["fader"]) : 100);
            s.pan = static_cast<std::int8_t>(p.hasProperty("pan") ? static_cast<int>(p["pan"]) : 0);
            s.send = static_cast<std::uint8_t>(p.hasProperty("send") ? static_cast<int>(p["send"]) : 0);
            s.mute = p.hasProperty("main_on") && ! static_cast<bool>(p["main_on"]);
            mixer_->setStrip(0, t, s, 0);
        }
        if (hasBus_)
        {
            using G = engine::mixer::GlobalParam;
            const auto index = [&](const char* key) {
                return busVar.hasProperty(key) ? static_cast<float>(static_cast<int>(busVar[key])) : 100.0f;
            };
            mixer_->setGlobal(G::MasterVolume, index("master"));
            mixer_->setGlobal(G::SendReturnVolume, index("return"));
            const auto text = [&](const char* key, const char* fallback) {
                return busVar.hasProperty(key) ? busVar[key].toString() : juce::String(fallback);
            };
            const auto sendFx = text("send_fx", "off");
            const auto masterFx = text("master_fx", "none");
            if (sendFx == "half")
                hooks_.sendFx = &half_;
            else if (sendFx != "off")
                return "unknown send_fx stand-in";
            if (masterFx == "swap")
                hooks_.masterFx = &swap_;
            else if (masterFx != "none")
                return "unknown master_fx stand-in";
        }
        mixer_->prepare(c.sampleRate, kN); // settled at the initial parameters
        buffers_->activeTracks = (1u << numTracks_) - 1u;

        events_.clear();
        if (const auto* ev = c.params["events"].getArray())
            for (const auto& e : *ev)
            {
                Event x;
                x.block = static_cast<int>(e["block"]);
                x.offset = e.hasProperty("offset") ? static_cast<int>(e["offset"]) : 0;
                x.track = e.hasProperty("track") ? static_cast<int>(e["track"]) : -1;
                if (x.track >= numTracks_ || x.offset < 0 || x.offset >= kN)
                    return "event out of range";
                static constexpr struct
                {
                    const char* name;
                    Key key;
                } kKeys[] = {{"fader", Key::Fader}, {"pan", Key::Pan},       {"send", Key::Send},
                             {"main_on", Key::Main}, {"master", Key::Master}, {"return", Key::Return}};
                for (const auto& k : kKeys)
                    if (e.hasProperty(k.name))
                    {
                        x.key = k.key;
                        if ((x.track < 0) != (x.key == Key::Master || x.key == Key::Return))
                            return "track events need a track, bus events none";
                        const auto& v = e[k.name];
                        x.value = x.key == Key::Main ? (static_cast<bool>(v) ? 1 : 0) : static_cast<int>(v);
                        events_.push_back(x);
                    }
            }
        std::stable_sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) { return a.block < b.block; });
        return {};
    }
    bool injectFault(Fault f) noexcept override
    {
        faultScale_ = f == Fault::Gain ? kFaultGainScale : 1.0f;
        return f == Fault::None || f == Fault::Gain;
    }
    void process(int block, const float* const* in, float* const* out) noexcept override
    {
        using G = engine::mixer::GlobalParam;
        auto& m = *mixer_;
        for (const auto& e : events_)
        {
            if (e.block != block)
                continue;
            if (e.key == Key::Master || e.key == Key::Return)
            {
                m.setGlobal(e.key == Key::Master ? G::MasterVolume : G::SendReturnVolume, static_cast<float>(e.value),
                            e.offset);
                continue;
            }
            auto s = m.strip(0, e.track);
            switch (e.key)
            {
                case Key::Fader: s.volume = static_cast<std::uint8_t>(e.value); break;
                case Key::Pan: s.pan = static_cast<std::int8_t>(e.value); break;
                case Key::Send: s.send = static_cast<std::uint8_t>(e.value); break;
                default: s.mute = e.value == 0; break;
            }
            m.setStrip(0, e.track, s, e.offset);
        }
        auto& b = *buffers_;
        for (int t = 0; t < numTracks_; ++t)
        {
            std::memcpy(b.track[static_cast<std::size_t>(t)].l, in[2 * t], sizeof(float) * kN);
            std::memcpy(b.track[static_cast<std::size_t>(t)].r, in[2 * t + 1], sizeof(float) * kN);
        }
        m.processTracks(b, hooks_);
        if (! m.sendBusUsed())
            b.sendBus.clear(); // the mixer leaves an untapped send bus stale
        const float* src[4] = {b.mainBus.l, b.mainBus.r, b.sendBus.l, b.sendBus.r};
        for (int o = 0; o < 4; ++o)
            copyScaled(out[o], src[o]);
        if (! hasBus_)
            return;
        m.processSend(b, hooks_);
        m.processMaster(b, hooks_);
        copyScaled(out[4], b.master.l);
        copyScaled(out[5], b.master.r);
    }

private:
    enum class Key
    {
        Fader,
        Pan,
        Send,
        Main,
        Master,
        Return
    };
    struct Event
    {
        int block = 0, track = -1, offset = 0;
        Key key = Key::Fader;
        int value = 0;
    };
    void copyScaled(float* dst, const float* src) const noexcept
    {
        for (int i = 0; i < kN; ++i)
            dst[i] = src[i] * faultScale_; // the fault test's deliberately wrong gain (x 1 exactly otherwise)
    }

    std::unique_ptr<engine::mixer::Mixer> mixer_;
    std::unique_ptr<engine::stages::BlockBuffers> buffers_;
    engine::mixer::Hooks hooks_ {};
    Half half_;
    Swap swap_;
    std::vector<Event> events_;
    int numTracks_ = 0;
    bool hasBus_ = false;
    float faultScale_ = 1.0f;
};
} // namespace

std::unique_ptr<Subject> makeTrackStripSubject()
{
    return std::make_unique<TrackStripSubject>();
}
} // namespace golden
