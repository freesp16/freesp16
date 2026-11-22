// Playback parameters of a Sample track (PLY-001…044): the region, pitch,
// bypass and stretch mode per [scene][track], the ParamChange targets that fill them, and the step-lock resolution
// of a trigger (SEQ-021: Start, Loop start, Length, Pitch, Time stretch, Loop, Reverse).
//
// Units on the wire (ParamChange::value, LockSet::values — the ParamSpec units of core/params/catalogue/playback.cpp):
//   playback.start / loopStart / length   frames (exact as float below 2^24 frames)
//   playback.pitch                        semitones (kPitchCentsPerUnit converts; the voice works in cents)
//   playback.loop / reverse / bypass      0 / 1;  playback.timeStretch  0 OFF, 1 RESAMPLE, 2 MASTER TEMPO
#pragma once

#include "core/model/enums/Enums.h"
#include "core/util/playback/Grid.h"

#include <cstdint>
#include <string_view>

namespace engine::voice
{
using core::model::StretchMode;
using core::playback::kWholeSample;

constexpr float kPitchCentsPerUnit = 100.0f; // playback.pitch is edited in semitones (ParamSpec unit "st")
constexpr float kMaxPitchCents = 2400.0f;    // PLY-020, TIME STRETCH OFF

struct Region // PLY-001: frame positions within the sample; end = start + length
{
    std::int64_t start = 0, loopStart = 0, length = kWholeSample;
    bool loop = false, reverse = false;

    std::int64_t end() const noexcept { return start + length; }
    // PLY-001 invariants against a sample of `frames` frames (core::playback::clampRegion).
    Region clamp(std::int64_t frames) const noexcept
    {
        const auto r = core::playback::clampRegion({start, loopStart, length}, frames);
        return {r.start, r.loopStart, r.length, loop, reverse};
    }
};

struct PlaybackParams // [scene][track] block, ParamChange target
{
    Region region {};
    float pitchCents = 0.0f;
    bool bypass = false;                       // PLY-044: silent, settings kept
    StretchMode stretch = StretchMode::Off;    // TIME STRETCH: OFF / RESAMPLE / MASTER TEMPO (stretch/StretchMode.h)
};

// The ParamSpec ids the voice implements, as one enum for the engine's ParamTarget switch.
enum class PlaybackField : std::uint8_t
{
    Pitch,
    Start,
    LoopStart,
    Length,
    Loop,
    TimeStretch,
    Reverse,
    Bypass,
    Count // not a playback parameter
};

// "playback.pitch" → PlaybackField::Pitch …; Count for any other id (message thread, set-up).
PlaybackField playbackFieldOf(std::string_view id) noexcept;

// One field of `p` from a ParamChange value / lock value in wire units (above).
void setField(PlaybackParams& p, PlaybackField field, float value) noexcept;

// The playback parameters of every scene and track (16 × 16): a scene switch is an index change, never a copy.
class PlaybackParamBlock
{
public:
    static constexpr int kScenes = 16, kTracks = 16;

    // scope = (scene << 4) | track (ParamChange::scope); false for an out-of-range scope or field.
    bool set(PlaybackField field, std::uint16_t scope, float value) noexcept;
    PlaybackParams& at(int scene, int track) noexcept { return params_[scene & 15][track & 15]; }
    const PlaybackParams& at(int scene, int track) const noexcept { return params_[scene & 15][track & 15]; }

private:
    PlaybackParams params_[kScenes][kTracks]; // default-initialised (no braces: GCC 11 build time)
};

// Step-lock slots the voice takes from a trigger (sequencer.md "Lockable parameters", ParamIndex order; the same
// order as services/store's kLockSlotNames).
namespace lockslot
{
constexpr int Start = 1, LoopStart = 2, Length = 3, Pitch = 4, TimeStretch = 7, Loop = 8, Reverse = 21;
constexpr int kPlaybackSlots[] = {Start, LoopStart, Length, Pitch, TimeStretch, Loop, Reverse};
} // namespace lockslot

enum class LockChange : std::uint8_t
{
    Revert = 0, // the value applies to this note only
    Hold = 1,   // the value stays for the following notes until another lock of the slot
    Glide = 2   // treated as Hold (the voice does not ramp)
};

// Per track: resolves a trigger's lock slots on top of the track's parameters (SEQ-021). Locked values apply to this
// note; they revert at the next trigger unless their change type is Hold, which keeps them for later notes.
class PlaybackLocks
{
public:
    // mask: bit s = slot s locked; values / change indexed by slot (LockSet layout); null pointers = no locks.
    PlaybackParams resolve(const PlaybackParams& base, std::uint64_t mask, const float* values,
                           const std::uint8_t* change) noexcept;
    void reset() noexcept { held_ = 0; }
    std::uint64_t heldMask() const noexcept { return held_; }

private:
    std::uint64_t held_ = 0;
    float heldValue_[32] {};
};
} // namespace engine::voice
