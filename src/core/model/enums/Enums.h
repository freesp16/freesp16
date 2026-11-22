// Enumerations of the document model (01-architecture/data-model.md, 03-ui/enumerations.md): the types the model
// headers need, with the values data-model.md names; EnumStrings.h adds the display strings and the
// enum <-> stored-string tables (OQ-10). Underlying types are fixed so the engine (which may include
// core/model/enums, module-layout.md) and files see stable sizes. Append new values; never renumber.
#pragma once

#include <cstdint>

namespace core::model
{
enum class TrackMode : std::uint8_t { Sample, Midi, Thru, Instrument };        // TRK, tracks.md
enum class TriggerMode : std::uint8_t { OneShot, Gate };                       // pad hits only (M p.18, p.38)
enum class StretchMode : std::uint8_t { Off, Resample, MasterTempo };          // PLY-030
enum class EnvelopeCurve : std::uint8_t { Linear, Curve };                     // ENV, attack/release curve
// Effect types: Off + the twelve live effects (02-features/effects/README.md); FX-001's popup-only names are P3
// extensions and are appended when they get a design.
enum class EffectType : std::uint8_t
{
    Off,
    Chorus,
    Flanger,
    Phaser,
    ParametricEq2,
    MultiModeFilter,
    InsertDelay,
    SendDelay,
    Reverb,
    LoFi,
    Distortion,
    Compressor,
    Ducker
};
enum class LfoShape : std::uint8_t { Sine, Triangle, SawUp, SawDown, Square, Random };   // our list
enum class LfoTriggerMode : std::uint8_t { Free, Retrigger, OneShot };                 // ours (OQ-19)
enum class LfoTriggerOn : std::uint8_t { Pad, Step, Both };                            // ours (OQ-19)
enum class TriggerType : std::uint8_t { Full, Half, LockOnly };                        // SEQ-011: stored 15 / 11 / 8
enum class LockChange : std::uint8_t { Revert, Hold, Glide };                          // SEQ-021
enum class QuantizeUnit : std::uint8_t { Off, Step, Beat, Bar, Pattern };              // pad / pattern quantise
enum class FxMode : std::uint8_t { Both, Send, Master };                               // OQ-12, per scene
enum class AssignMode : std::uint8_t { MasterPlus6, EightChannel };                    // M + 6CH / 8CH (MIX-030)
enum class OutputAssign : std::uint8_t { Master, Out12, Out34, Out56, Out78, Mono1, Mono2, Mono3, Mono4, Mono5,
                                         Mono6, Mono7, Mono8 };                         // OUT ASSIGN (MIX-031)
enum class AfsRouteMode : std::uint8_t { Off, Pre, Post, Master };                     // ours (OQ-19)
} // namespace core::model
