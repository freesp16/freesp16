// engine/trigger/TriggerEvent.h — every trigger that reaches a track, whatever its source.
// A frozen contract: the trigger sources, the router and the track sources all build against it.
#pragma once

#include <cstdint>

// clang-format off: the block below is the frozen contract text
namespace engine::trigger
{
enum class TriggerKind : std::uint8_t { Full, Half, LockOnly, Release, Stop };   // SEQ-011 codes 15/11/8; gate off; choke/panic
enum class TriggerSource : std::uint8_t { Pad, Midi, Sequencer, Retrigger, Repeat };
struct LockSet {                       // view, owned by the sender for the whole block (the sequencer's pattern object)
    std::uint64_t mask = 0;            // bit s = slot s locked; slots 0–22 = core::ParamIndex, 23–28 FX1, 29–34 FX2
    const float* values = nullptr;     // native units (frames, cents, 0–127, enum index)
    const std::uint8_t* change = nullptr;   // per slot: 0 Revert, 1 Hold, 2 Glide (SEQ-021)
    bool get(int slot, float& v) const noexcept { if (!(mask >> slot & 1u)) return false; v = values[slot]; return true; }
};
struct TriggerEvent {
    std::uint8_t track;        // 0–15
    std::uint8_t velocity;     // 1–127 (Release/Stop: 0)
    std::int8_t semitones;     // pad scale / MIDI note offset, added in cents by the voice (PLY-022)
    TriggerKind kind;  TriggerSource source;
    std::uint8_t frame;        // 0–63 within the block
    bool ducks;                // Ducker feed (SEQ-027)
    const LockSet* locks;      // null = no locks (pads)
};
}
// clang-format on
