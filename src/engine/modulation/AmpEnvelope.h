// The AHR amp envelope of one Sample track (the frozen single-track API; spec 02-features/amp-envelope.md). A one-lane
// AmpEnvelopeLanes: the same code as the 16-track structure-of-arrays bank (AmpEnvelopeBank.h), which the engine's
// track stage should prefer (one set of tables, one cache footprint). Every behaviour — sample-accurate events, curves,
// sync, retrigger, velocity, bypass, locks, Thru — is described there.
#pragma once

#include "engine/modulation/AmpEnvelopeBank.h"
#include "engine/modulation/AmpEnvelopeParams.h"
#include "engine/trigger/TriggerEvent.h"

namespace engine::modulation
{
using core::ampenv::EnvCurve;

class AmpEnvelope
{
public:
    void prepare(double sampleRate) noexcept { lane_.prepare(sampleRate); }
    void setParams(const AmpEnvParams& p, double bpm) noexcept { lane_.setParams(0, p, bpm); } // tables, not per sample
    // Full: restart (from the current level, no click). Half: restart only when `retrigger` (ENV-007).
    // The event's track is ignored (this envelope is one track); its frame is honoured.
    void trigger(const trigger::TriggerEvent& e, bool gate) noexcept
    {
        trigger::TriggerEvent own = e;
        own.track = 0;
        lane_.trigger(own, gate);
    }
    void release(int frame) noexcept { lane_.release(0, frame); } // GATE pad release (ENV-008)
    void kill(int frame) noexcept { lane_.kill(0, frame); }       // 64-frame de-click to 0
    float currentGain() const noexcept { return lane_.currentGain(0); }
    bool finished() const noexcept { return lane_.finished(0); }          // finished → TrackSource stops the voice
    void process(float* l, float* r) noexcept { lane_.process(0, l, r); } // 64 frames, multiplies in place, NEON

    // Additions beyond the frozen API.
    void setThru(bool thru) noexcept { lane_.setThru(0, thru); } // Thru tracks have no envelope
    void reset() noexcept { lane_.reset(0); }
    void renderGain(float* gain) noexcept { lane_.renderGain(0, gain); }
    AmpEnvStage stage() const noexcept { return lane_.stage(0); }
    void useScalarKernels(bool scalar) noexcept { lane_.useScalarKernels(scalar); }

private:
    AmpEnvelopeLanes<1> lane_ {};
};
} // namespace engine::modulation
