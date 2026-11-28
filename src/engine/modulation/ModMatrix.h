// ModMatrix — the one summing point for per-block parameter modulation (spec 02-features/lfo.md
// LFO-003/005/006/013, effects/parameters.md FX-083…085). The LFO, the sequencer's transpose and the touch strip
// (ribbon PITCH / USER1/2) build against it; keep it stable.
//
// Sources (one contribution per source per destination per track):
//   Lfo        the track LFO (engine/modulation/LfoStage), Offset = lfoOut · depth/127 (FX-084, LFO-003/006)
//   Transpose  sequencer transpose (SEQ-035), Offset on playback.pitch (pitchOffset01 below)
//   Ribbon     touch strip: PITCH = Offset on playback.pitch, USER1/2 items = Override (RIB-002/004)
//   PitchBend  MIDI pitch bend, Offset on playback.pitch
// Values are normalised to the destination ParamSpec's range (ParamSpec::normalised): an Offset is a signed fraction
// of the full range (−1…+1), an Override an absolute position 0–1. Per destination and block the result is
//     applied = clamp((override if any source overrides, else base) + Σ offsets, 0, 1)
// with base = the destination's stored or locked value at that moment (FX-084: "base is the stored or locked value").
// The clamp lives where the base lives: the FX slot bank clamps FX destinations (FxSlotBank::setModulation:
// clamp(effective + amount)), the engine's ParamStateSink clamps track destinations. Several Override sources on one
// destination: the highest ModSource wins (our choice). No quantisation to the step grid.
//
// Destinations are ParamSpec ids as ParamIds (the LFO stores `playback.pitch`, `fx1.filter.cutoff`, …). In
// applyBlock an id the FX bank routes as an insert knob (FxSlotBank::routes(): <fx1|fx2>.<effect>.<key>, which encodes
// slot, effect type and internal index k) goes to FxSlotBank::setModulation(track, unit, type, k, amount); an FX
// destination whose effect type is not the slot's current type is skipped and changes nothing (FX-085, LFO-013:
// "the LFO does nothing until that type returns"; stale() reports it). Every other id goes to the ParamStateSink.
//
// Threads: everything but prepare() runs on the audio thread, inside the engine block, in this order:
//   step 6 sequencer/router (triggers, step locks)  →  sources call set()/clear()  →  applyBlock() once  →  step 8 voices
// so set() may be called any number of times per block (the last value wins) and applyBlock() turns the block's
// values into at most one call per destination. Fixed arrays (16 tracks × 8 destination slots × 4 sources); nothing
// allocates, locks or does I/O after prepare(). Dirty flags: a slot is applied when its value changed, when it was
// touched (touch(): the destination's base changed — a ParamChange, a step lock, a scene switch) or once when its last
// source was cleared (release); free slots cost nothing. Budget: ≤ 80 cycles per applied destination.
#pragma once

#include "core/params/ParamId.h"

#include <array>
#include <cstdint>

namespace engine::fx
{
class FxSlotBank;
}

namespace engine::modulation
{
enum class ModSource : std::uint8_t
{
    Lfo,
    Transpose,
    Ribbon,
    PitchBend,
    Count
};
enum class ModKind : std::uint8_t
{
    Offset,  // added to the base (signed fraction of the normalised range)
    Override // replaces the base (absolute normalised value 0–1): ribbon USER items
};

constexpr int kModTracks = 16;
constexpr int kModDestsPerTrack = 8;
constexpr int kModSources = static_cast<int>(ModSource::Count);

// playback.pitch is −24…+24 st (core/params/catalogue/playback.cpp): a pitch offset in cents as a normalised Offset,
// and back. Transpose ±24 st = ±0.5; the ribbon's +200 cents = 200 / 4800.
constexpr float kPitchSpanCents = 4800.0f;
constexpr float pitchOffset01(float cents) noexcept { return cents / kPitchSpanCents; }
constexpr float pitchOffsetCents(float offset01) noexcept { return offset01 * kPitchSpanCents; }

// The block's modulation of one destination, as the destination applies it.
struct ModValue
{
    float offset01 = 0.0f;   // Σ Offset contributions (fraction of the normalised range)
    float override01 = 0.0f; // the winning Override value (0–1), when hasOverride
    bool hasOverride = false;

    // clamp((override or base) + offset, 0, 1) on the normalised range (FX-084 / LFO-003).
    constexpr float apply(float base01) const noexcept
    {
        const float v = (hasOverride ? override01 : base01) + offset01;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
};

// The engine's track-parameter state as the matrix sees it (implemented once by the engine, engine/modulation/
// LfoStage.h TrackModSink; tests use their own). Called once per applied destination per block, never per sample;
// the destination ramps over the block (64 samples) where its parameter is continuous.
class ParamStateSink
{
public:
    virtual ~ParamStateSink() = default;
    // Apply `v` to `dest` of `track` (current scene) on top of its stored or locked base. false = the engine has no
    // such track destination (the slot stays, so it applies once the destination exists; nothing changes meanwhile).
    virtual bool modulate(int track, core::ParamId dest, const ModValue& v) noexcept = 0;
    // `dest` of `track` is no longer modulated: back to its base value.
    virtual void release(int track, core::ParamId dest) noexcept = 0;
};

class ModMatrix
{
public:
    // Message thread (or before the device starts). Clamped to kModTracks × kModDestsPerTrack (the arrays are fixed
    // members); clears every slot without releasing anything.
    void prepare(int tracks = kModTracks, int maxDestsPerTrack = kModDestsPerTrack) noexcept;

    // ---- audio thread, before applyBlock() ----
    // Source `src`'s contribution to `dest` of `track` (replaces its previous one there). false = invalid track or
    // id, or no free destination slot on the track (the contribution is dropped).
    bool set(int track, core::ParamId dest, ModSource src, ModKind kind, float value01) noexcept;
    void clear(int track, ModSource src) noexcept;                        // every destination of src on the track
    void clear(int track, core::ParamId dest, ModSource src) noexcept;    // one destination
    void clearAll(ModSource src) noexcept;                                // every track (e.g. ribbon release)
    void touch(int track) noexcept; // the track's bases changed: re-apply its active destinations this block
    void touchAll() noexcept;       // scene switch
    void swapTracks(int a, int b) noexcept;

    // Once per block, after the step locks and before the voices (see above).
    void applyBlock(fx::FxSlotBank& fx, ParamStateSink& sink) noexcept;

    // ---- inspection (audio thread; tests and the stages) ----
    ModValue value(int track, core::ParamId dest) const noexcept;                         // zero if not modulated
    float contribution(int track, core::ParamId dest, ModSource src) const noexcept;      // 0 if none
    bool modulated(int track, core::ParamId dest) const noexcept;
    int activeDestinations(int track) const noexcept;
    // An FX destination skipped by the last applyBlock() because its effect type is not in the slot (FX-085).
    bool stale(int track, core::ParamId dest) const noexcept;

private:
    struct Slot
    {
        core::ParamId dest {};
        float value[kModSources] {};
        std::uint8_t sources = 0;   // bit s = source s contributes
        std::uint8_t overrides = 0; // bit s = source s is an Override
        bool dirty = false;         // apply in the next applyBlock()
        bool applied = false;       // the destination currently holds a modulation (release it when emptied)
        bool stale = false;
    };
    struct Track
    {
        std::array<Slot, kModDestsPerTrack> slot {};
        std::uint8_t used = 0; // bit i = slot i has a destination (active or waiting for its release)
    };
    Slot* find(int track, core::ParamId dest) noexcept;
    const Slot* find(int track, core::ParamId dest) const noexcept;
    static ModValue sum(const Slot&) noexcept;

    std::array<Track, kModTracks> tracks_ {};
    int numTracks_ = kModTracks, numDests_ = kModDestsPerTrack;
};
} // namespace engine::modulation
