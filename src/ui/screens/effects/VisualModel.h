// The display-only visual kinds of the EffectPanel's visual zone (screens/effects.md "Visual kinds", UID-834/835):
// lfoCurve, eqCurve, filterCurve, delayTaps, decayCurve, meters, duckEnvelope. Each kind is a renderer widget
// registered as "fxvisual.<kind>" (APP_REGISTER_WIDGET), drawn from a VisualModel in engineering units. An effect
// supplies only a VisualAdapter (knob fractions → units, PanelHints::visual); without one the kind's default adapter
// maps the spec's page order roughly. Renderers recompute curves only when the model changes (≤ 0.3 ms for 256 points
// on the A9) and append cached spans afterwards; live parts (meters, the duck dot) are cheap.
#pragma once

#include "ui/render/DrawList.h"
#include "ui/widgets/Widget.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ui
{
struct VisualModel
{
    std::uint32_t colour = 0; // ink: the track colour (insert) or accent (bus); 0 = accent
    float opacity = 1;        // 0.4 while the insert is bypassed (screens/effects.md "States")
    bool sync = false, bus = false, playing = false;

    // lfoCurve: cycles across the zone (free: 4 s window, sync: 4 bars), WIDTH as amplitude 0–1, R's PHASE offset 0–1
    float cycles = 4, depth = 0.8f, phase = 0.25f;
    double lfoStartMs = 0; // animation origin while playing

    // eqCurve: two bands; type = the FX-069 internal value
    // (LowShelving 0, HighShelving 1, Peaking 2, LowPass 3, HighPass 4)
    struct EqBand
    {
        std::uint8_t type = 2;
        float freqHz = 1000, gainDb = 0, q = 0.7f;
    };
    std::array<EqBand, 2> band{{{0, 120, -4.5f, 0.7f}, {1, 5000, 6, 0.7f}}};

    // filterCurve: type 0 LPF, 1 HPF, 2 BPF, 3 OFF (FX-069 UI values); slope 12 or 24 dB/Oct
    std::uint8_t filterType = 0, slopeDb = 12;
    float cutoffHz = 1000, filterGainDb = 0, resonance = 0.7f;

    // delayTaps: tap spacing as a fraction of the window (free 2 s, sync 1 bar), feedback 0–1, beat lines in the
    // window (16 = sixteenths of a bar while synced, 0 = none), SPREAD 0–1 (bus: L/R taps alternate up/down)
    float tapFraction = 0.125f, feedback = 0.45f, spread = 0;
    int beatLines = 0;

    // decayCurve: PRE DELAY and TIME as fractions of the zone, DAMPING 0–1 (dims the tail)
    float preDelayFraction = 0.02f, lengthFraction = 0.75f, damping = 0.3f;

    // meters: TRANSFER (threshold 0–1 of the 128 lpx graph, ratio ≥ 1, soft knee), IN/OUT (stereo) and GR levels 0–1
    float threshold = 0.66f, ratio = 4;
    bool softKnee = false;
    std::array<float, 2> in{}, out{};
    float gainReduction = 0;

    // duckEnvelope: ATTACK/RELEASE as fractions of the graph width, curves −1…+1, DEPTH 0–1, the source track (1–16),
    // the live envelope 0–1 (dot position) and the last source trigger (chip flash)
    float attackFraction = 0.02f, releaseFraction = 0.2f, attackCurve = 0, releaseCurve = 0, duckDepth = 1;
    int sourceTrack = 1;
    float envelope = 0;
    double lastTriggerMs = -1e9;
    std::uint32_t sourceColour = 0; // the source track's colour (0 = the slot ink)
};

// A visual renderer: a widget whose model the EffectPanel sets before each build.
class FxVisual : public Widget
{
public:
    explicit FxVisual(RectF zone) noexcept : zone_(zone) {}
    RectF bounds() const noexcept override { return zone_; }
    // Applies a new model; returns true when the cached geometry was rebuilt (parameter change).
    virtual bool setModel(const VisualModel& m) = 0;
    const VisualModel& model() const noexcept { return model_; }
    virtual std::string_view kind() const noexcept = 0;

protected:
    RectF zone_;
    VisualModel model_{};
};

// Visual zone geometry (screens/effects.md "EffectPanel geometry": 32, 336, 1216 × 152).
namespace fxzone
{
inline constexpr RectF zone{32, 336, 1216, 152};
inline constexpr float x0 = 32, x1 = 1248, y0 = 336, y1 = 488;
} // namespace fxzone
} // namespace ui
