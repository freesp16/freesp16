// Default VisualAdapters, one per visual kind: used when an effect registers no PanelHints::visual. They read the knob
// fractions in the spec's page order of the effect that introduced the kind (screens/effects.md "The 12 effects") and
// map them to plausible units. Each effect replaces them with its exact formulas (its own adapter in
// ui/screens/effects/hints/<id>.cpp); a new effect with a new layout needs only that adapter.
#include "ui/widgets/EffectPanel.h"

#include <algorithm>
#include <cmath>

namespace ui
{
namespace
{
float expMap(float v01, float lo, float hi) noexcept
{
    return lo * std::pow(hi / lo, std::clamp(v01, 0.0f, 1.0f));
}

void common(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    m.colour = in.slotColour;
    m.sync = in.sync;
    m.bus = in.bus;
    if (in.live != nullptr)
    {
        m.in = in.live->in;
        m.out = in.live->out;
        m.gainReduction = in.live->gainReduction;
        m.envelope = in.live->duckEnvelope;
        m.lastTriggerMs = in.live->lastTriggerMs;
    }
}

// RATE · WIDTH · PHASE · DELAY · (FEEDBACK) · MIX
void lfo(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    common(in, m);
    const float k = in.knob01[0];
    // free: 0.1…10 Hz over a 4 s window; sync: 16 … 1/16 beats per cycle over 4 bars (16 beats)
    m.cycles = in.sync ? 16.0f / expMap(1 - k, 1.0f / 16, 16) : 4 * expMap(k, 0.1f, 10);
    m.cycles = std::clamp(m.cycles, 0.25f, 48.0f);
    m.depth = in.knob01[1];
    m.phase = in.knob01[2] * 0.5f;
}

// B1 Q · B1 FREQ · B1 GAIN · B2 Q · B2 FREQ · B2 GAIN; options BAND1 TYPE, BAND2 TYPE
void eq(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    common(in, m);
    for (int b = 0; b < 2; ++b)
    {
        auto& band = m.band[static_cast<std::size_t>(b)];
        const int o = in.options[static_cast<std::size_t>(b)];
        band.type = static_cast<std::uint8_t>(o >= 0 && o <= 4 ? o : 2);
        band.q = expMap(in.knob01[static_cast<std::size_t>(3 * b)], 0.3f, 12);
        band.freqHz = expMap(in.knob01[static_cast<std::size_t>(3 * b + 1)], 20, 20000);
        band.gainDb = (in.knob01[static_cast<std::size_t>(3 * b + 2)] - 0.5f) * 36;
    }
}

// CUT OFF · GAIN · RESONANCE · GLIDE · — · MIX; options TYPE (LPF HPF BPF OFF), SLOPE (12, 24)
void filter(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    common(in, m);
    m.cutoffHz = expMap(in.knob01[0], 20, 20000);
    m.filterGainDb = (in.knob01[1] - 0.5f) * 24;
    m.resonance = 0.5f + in.knob01[2] * 9.5f;
    m.filterType = static_cast<std::uint8_t>(in.options[0] >= 0 && in.options[0] <= 3 ? in.options[0] : 0);
    m.slopeDb = in.options[1] == 1 ? 24 : 12;
}

// TIME · FEEDBACK · HI CUT · (LO CUT) · (SPREAD) · MIX
void delay(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    common(in, m);
    m.beatLines = in.sync ? 16 : 0;
    m.tapFraction = in.sync ? std::max(1.0f / 32, in.knob01[0] * 0.5f) : std::max(0.005f, in.knob01[0] * 0.5f);
    m.feedback = in.knob01[1];
    m.spread = in.bus ? in.knob01[4] : 0;
}

// PRE DELAY · ROOM SIZE · TIME · DAMPING · SPREAD · MIX
void decay(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    common(in, m);
    m.preDelayFraction = in.knob01[0] * 0.2f;
    m.lengthFraction = 0.1f + 0.9f * in.knob01[2];
    m.damping = in.knob01[3];
}

// RATIO · ATTACK · RELEASE · THRESHOLD · GAIN · MIX; options LOOK AHEAD, PEAK|RMS, SOFT KNEE
void meters(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    common(in, m);
    m.ratio = 1 + 19 * in.knob01[0];
    m.threshold = in.knob01[3];
    m.softKnee = in.options[2] == 1;
}

// ATTACK · CURVE · RELEASE · CURVE · — · DEPTH; options SYNC, SOURCE (track 1–16 → 0–15)
void duck(const EffectVisualInputs& in, VisualModel& m) noexcept
{
    common(in, m);
    m.attackFraction = 0.2f * in.knob01[0];
    m.attackCurve = 2 * in.knob01[1] - 1;
    m.releaseFraction = 0.05f + 0.6f * in.knob01[2];
    m.releaseCurve = 2 * in.knob01[3] - 1;
    m.duckDepth = in.knob01[5];
    m.sourceTrack = in.options[1] >= 0 ? in.options[1] + 1 : 1;
    m.sourceColour = theme::trackColour(m.sourceTrack);
}
} // namespace

VisualAdapter defaultVisualAdapter(std::string_view kind) noexcept
{
    if (kind == "lfoCurve")
        return &lfo;
    if (kind == "eqCurve")
        return &eq;
    if (kind == "filterCurve")
        return &filter;
    if (kind == "delayTaps")
        return &delay;
    if (kind == "decayCurve")
        return &decay;
    if (kind == "meters")
        return &meters;
    if (kind == "duckEnvelope")
        return &duck;
    return &common;
}
} // namespace ui
