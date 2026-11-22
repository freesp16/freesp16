// The 16 track LFOs as an engine stage, and the engine's track-parameter sink of the ModMatrix (spec
// 02-features/lfo.md, effects/parameters.md FX-083…085). Plain members of engine::Engine, called directly once per
// block (no virtual call per sample; the sink is one virtual call per modulated destination per block).
//
// Block order (engine::Engine::renderBlock, after step 6's router and FX step locks, before step 8's sources):
//   lfo_.process(ctx, scene, triggerRouter_.blockEvents(), modMatrix_)   restarts (transport start, Full / Half
//                     triggers per Mode / Trigger On), step locks of slots 18–20, one advanceBlock per LFO, then
//                     ModMatrix::set(track, destination, Lfo, Offset, lfoOut · depth) — or clear() when the LFO is
//                     bypassed, has no destination or depth 0
//   modSink_.beginBlock(scene, bpm); modMatrix_.applyBlock(fxBank_, modSink_)
//
// Parameters (core/params/catalogue/lfo.cpp, ParamChange scope = (scene << 4) | track, native units): lfo.speed 0–127,
// lfo.depth 0–127, lfo.phase 0–127, lfo.shape 0–6 (LfoShape), lfo.destination = the destination's ParamId value
// (−1 NONE; runtime only — files keep the ParamSpec id), lfo.sync 0/1, lfo.triggerMode 0 FREE / 1 TRIGGER /
// 2 ONE SHOT, lfo.triggerOn 0 FULL / 1 FULL & HALF, lfo.randomStart 0–127, lfo.bypass 0/1. Every scene keeps its own
// values (data-model.md: scene[s].track[t].lfo); a scene switch reconfigures the LFOs and keeps their phase running.
//
// Trigger (LFO-007, 03-ui/enumerations.md "Trigger mode"): FREE runs from transport start and never restarts on
// triggers; TRIGGER restarts at the phase offset on Full triggers (pads, MIDI, steps, ratchets) and, with
// FULL & HALF, on Half steps; ONE SHOT (our addition) restarts like TRIGGER and holds after one cycle. Every mode
// restarts at the transport start (renders repeat). SYNC restarts are anchored at the trigger's exact PPQ.
// Locks (LFO-010, sequencer.md slots 18 LFO speed, 19 depth, 20 phase offset): a step's lock applies from its trigger
// and reverts at the track's next step unless its change type is Hold (SEQ-021); ratchet repeats do not re-apply.
#pragma once

#include "engine/EngineApi.h"
#include "engine/bridge/ParamBindings.h"
#include "engine/modulation/Lfo.h"
#include "engine/modulation/ModMatrix.h"
#include "engine/trigger/TriggerEvent.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace engine::mixer
{
class Mixer;
}
namespace engine::trigger
{
struct SourceSet;
}

namespace engine::modulation
{
enum class LfoField : std::uint8_t
{
    Speed,       // "lfo.speed"       0–127 (core/util/lfo/LfoLaw.h)
    Depth,       // "lfo.depth"       0–127
    Phase,       // "lfo.phase"       0–127
    Shape,       // "lfo.shape"       LfoShape 0–6
    Destination, // "lfo.destination" ParamId value, −1 = NONE
    Sync,        // "lfo.sync"        0/1
    TriggerMode, // "lfo.triggerMode" 0 FREE, 1 TRIGGER, 2 ONE SHOT
    TriggerOn,   // "lfo.triggerOn"   0 FULL, 1 FULL & HALF
    RandomStart, // "lfo.randomStart" 0–127
    Bypass,      // "lfo.bypass"      0/1
    Count
};

struct LfoParamId
{
    std::string_view id;
    LfoField field;
};
// Set-up only (string ids never reach the audio thread); the engine binds them to ParamTarget::LfoFirst + field.
inline constexpr std::array<LfoParamId, static_cast<std::size_t>(LfoField::Count)> kLfoParamIds {{
    {"lfo.speed", LfoField::Speed},
    {"lfo.depth", LfoField::Depth},
    {"lfo.phase", LfoField::Phase},
    {"lfo.shape", LfoField::Shape},
    {"lfo.destination", LfoField::Destination},
    {"lfo.sync", LfoField::Sync},
    {"lfo.triggerMode", LfoField::TriggerMode},
    {"lfo.triggerOn", LfoField::TriggerOn},
    {"lfo.randomStart", LfoField::RandomStart},
    {"lfo.bypass", LfoField::Bypass},
}};

// Lock slots (core::ParamIndex, sequencer.md "Lockable parameters").
constexpr int kLockSlotLfoSpeed = 18, kLockSlotLfoDepth = 19, kLockSlotLfoPhase = 20;

enum class LfoTriggerMode : std::uint8_t { Free, Trigger, OneShot };
enum class LfoTriggerOn : std::uint8_t { Full, FullAndHalf };

struct LfoParams // [scene][track]; the defaults of the catalogue (lfo.cpp)
{
    float speed = 21.0f; // knob 0–127 (1.60 default 21 [OBS])
    std::uint8_t depth = 63, phase = 0, randomStart = 0;
    LfoShape shape = LfoShape::Sine;
    LfoTriggerMode mode = LfoTriggerMode::Trigger;
    LfoTriggerOn triggerOn = LfoTriggerOn::Full;
    bool sync = false, bypass = false;
    core::ParamId destination {}; // invalid = NONE
};

// Writes one ParamChange value (native units) into a parameter block; false = nothing changed.
bool applyLfoParam(LfoParams& p, LfoField field, float value) noexcept;

class LfoStage
{
public:
    static constexpr int kScenes = 16;

    void prepare(double sampleRate) noexcept;

    // ---- audio thread ----
    // A ParamChange of an lfo.* id (scope = (scene << 4) | track); takes effect at this block's process().
    void setParam(LfoField field, std::uint16_t scope, float value) noexcept;
    // Restarts, locks, one value per LFO → the matrix (see the file comment).
    void process(const BlockContext& ctx, int scene, std::span<const trigger::TriggerEvent> events,
                 ModMatrix& matrix) noexcept;

    // ---- inspection (audio thread / tests) ----
    const Lfo& lfo(int track) const noexcept { return lfo_[static_cast<std::size_t>(track & 15)]; }
    const LfoParams& params(int scene, int track) const noexcept { return params_[scene & 15][track & 15]; }
    core::ParamId appliedDestination(int track) const noexcept { return applied_[static_cast<std::size_t>(track & 15)]; }
    float amount(int track) const noexcept { return amount_[static_cast<std::size_t>(track & 15)]; } // 0 = not applied

private:
    struct Locks
    {
        std::uint8_t mask = 0, hold = 0; // bit 0 speed, 1 depth, 2 phase
        float value[3] {};
    };
    void configure(int track, const LfoParams& p) noexcept; // shape, speed, sync, depth, phase, seed, one-shot
    void applyLocks(int track, const trigger::LockSet* locks) noexcept;

    LfoParams params_[kScenes][kTracks]; // default-initialised (no braces: GCC 11 expands them element-wise)
    std::array<Lfo, kTracks> lfo_ {};
    std::array<Locks, kTracks> locks_ {};
    std::array<core::ParamId, kTracks> applied_ {}; // the destination the matrix holds for each LFO
    std::array<float, kTracks> amount_ {};
    std::array<float, kTracks> speedSet_ {};        // the speed knob value last converted (exp2 only on change)
    std::uint16_t dirty_ = 0xFFFF;                   // bit t = reconfigure LFO t at the next process()
    int scene_ = -1;
};

// The engine's track destinations (ModMatrix::applyBlock → ParamStateSink), applied on top of their stored or locked
// base and clamped to the ParamSpec range (FX-084 / LFO-003):
//   playback.pitch      → TrackVoice::setModCents(voice::ModSource::Lfo, …): the matrix sum (LFO + transpose +
//                         ribbon + bend) in one slot, clamped against the stored pitch (−24…+24 st)
//   env.velocity/attack/hold/release → the envelope lane's parameters (AmpEnvelopeBank::setParams; from its next
//                         segment, ENV docs)
//   track.volume/pan/sendAmount → mixer::Mixer::setModulation (index offsets; MIX-002's 100-sample ramps)
// Any other id: not a track destination (modulate() returns false).
class TrackModSink final : public ParamStateSink
{
public:
    void attach(const bridge::ParamBindings* bindings, trigger::SourceSet* sources, mixer::Mixer* mixer) noexcept
    {
        bindings_ = bindings;
        sources_ = sources;
        mixer_ = mixer;
    }
    void beginBlock(int scene, double bpm) noexcept
    {
        scene_ = scene & 15;
        bpm_ = bpm;
    }
    bool modulate(int track, core::ParamId dest, const ModValue& v) noexcept override;
    void release(int track, core::ParamId dest) noexcept override;

private:
    bool apply(int track, core::ParamId dest, const ModValue* v) noexcept; // null = release

    const bridge::ParamBindings* bindings_ = nullptr;
    trigger::SourceSet* sources_ = nullptr;
    mixer::Mixer* mixer_ = nullptr;
    int scene_ = 0;
    double bpm_ = 120.0;
};
} // namespace engine::modulation
