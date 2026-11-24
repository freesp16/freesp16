// Engine step 3 (audio-engine.md "Per-block processing order"): the transport clock and the metronome as one stage
// of engine::Engine (engine/stages/Stages.h names it stages::TransportStage).
//
//   command(TransportCmd)       step 1, the FIFO drain (Transport.h lists the meanings)
//   setParam(TransportParam, v) step 1, ParamChange of the transport.* ids below (bound in Engine's constructor)
//   process(ctx, buffers)       step 3: ctx.ppqStart/ppqEnd/bpm/playing/started/stopped, timing.preCount
//   renderClick(ctx, buffers)   after every other writer of BlockBuffers::aux and before step 11 (the mixer adds aux
//                               after the master fader): the click of this block, added into aux. A writer that
//                               *clears* aux (the BROWSE preview) must run before it.
//
// Parameters (core/params/catalogue/transport.cpp): transport.tempo (BPM), transport.metronome (0/1),
// transport.metronomeVolume (0–127, GainLaw), transport.nudge (−1/0/+1 while held), transport.nudgeAmount (1–10 %).
// The pre-count clicks even with the metronome off (SEQ-044); transport.preCount itself is read by the UI's
// TransportController (it posts Play with the bars); transport.metronomeMonitor is not read by the engine
// (aux = master + headphones).
#pragma once

#include "core/util/GainLaw.h"
#include "engine/EngineApi.h"
#include "engine/stages/BlockBuffers.h"
#include "engine/transport/Metronome.h"
#include "engine/transport/Transport.h"

#include <cstdint>

namespace engine::transport
{
enum class TransportParam : std::uint8_t
{
    Tempo,
    Metronome,
    MetronomeVolume,
    Nudge,
    NudgeAmount,
    Count
};
// ParamSpec ids in TransportParam order (Engine binds them to ParamTarget::TransportTempo + i).
inline constexpr const char* kParamIds[] = {"transport.tempo", "transport.metronome", "transport.metronomeVolume",
                                            "transport.nudge", "transport.nudgeAmount"};
static_assert(sizeof(kParamIds) / sizeof(kParamIds[0]) == static_cast<std::size_t>(TransportParam::Count));

class TransportStage
{
public:
    void prepare(double sampleRate) noexcept
    {
        transport_.prepare(sampleRate);
        metronome_.prepare(sampleRate);
        clickGain_ = core::GainLaw::gain(volume_) * Metronome::kPeak;
    }
    void command(const TransportCmd& c) noexcept { transport_.command(c); }
    void setParam(TransportParam p, float v) noexcept
    {
        switch (p)
        {
            case TransportParam::Tempo: transport_.setTempo(static_cast<double>(v)); break;
            case TransportParam::Metronome: metronomeOn_ = v >= 0.5f; break;
            case TransportParam::MetronomeVolume:
                volume_ = static_cast<int>((v < 0.0f ? 0.0f : (v > 127.0f ? 127.0f : v)) + 0.5f);
                clickGain_ = core::GainLaw::gain(volume_) * Metronome::kPeak;
                break;
            case TransportParam::Nudge:
                nudge_ = v > 0.5f ? 1 : (v < -0.5f ? -1 : 0);
                transport_.setNudge(nudge_, nudgeAmount_);
                break;
            case TransportParam::NudgeAmount:
                nudgeAmount_ = (v < 0.0f ? 0.0f : (v > 50.0f ? 50.0f : v)) / 100.0f;
                transport_.setNudge(nudge_, nudgeAmount_);
                break;
            case TransportParam::Count: break;
        }
    }

    void process(BlockContext& ctx, stages::BlockBuffers& b) noexcept
    {
        transport_.advance(ctx);
        b.timing.preCount = transport_.preCountThisBlock();
        if (transport_.hardStopThisBlock())
            metronome_.reset();
    }

    void renderClick(const BlockContext& ctx, stages::BlockBuffers& b) noexcept
    {
        metronome_.set(true, clickGain_, ! metronomeOn_);
        metronome_.setBarOrigin(b.timing.patternStartPpq);
        if (! metronome_.renderMono(ctx))
            return;
        if (b.auxActive)
            metronome_.addTo(b.aux.l, b.aux.r);
        else
        {
            metronome_.copyTo(b.aux.l, b.aux.r); // aux was empty: no clear + add
            b.auxActive = true;
        }
    }

    bool playing() const noexcept { return transport_.state() == State::Playing; }
    bool preCount() const noexcept { return transport_.state() == State::PreCount; }
    double ppq() const noexcept { return transport_.ppq(); }
    double bpm() const noexcept { return transport_.bpm(); } // stored tempo (display); ctx.bpm is the effective one
    double effectiveBpm() const noexcept { return transport_.effectiveBpm(); } // the followed clock / nudge applied
    State state() const noexcept { return transport_.state(); }
    bool hardStopThisBlock() const noexcept { return transport_.hardStopThisBlock(); }
    // Another name for the hard stop (FX-036: the second STOP cuts every tail).
    bool secondStopThisBlock() const noexcept { return transport_.hardStopThisBlock(); }
    int startFrame() const noexcept { return transport_.startFrame(); }
    bool locatedThisBlock() const noexcept { return transport_.locatedThisBlock(); }

    Transport& transport() noexcept { return transport_; }
    const Transport& transport() const noexcept { return transport_; }
    Metronome& metronome() noexcept { return metronome_; }

private:
    Transport transport_;
    Metronome metronome_;
    bool metronomeOn_ = false;
    int volume_ = 100;
    float clickGain_ = Metronome::kPeak; // GainLaw(volume_) × kPeak
    int nudge_ = 0;
    float nudgeAmount_ = 0.03f; // transport.nudgeAmount default 3 %
};
} // namespace engine::transport
