// Value lists of 03-ui/enumerations.md that the model stores as plain integers (those fields are `int`):
// the enum gives each stored integer its meaning, EnumStrings.h its stored and display strings. Underlying types are
// fixed and values are the integers the model holds (never renumber; append). Also the reference codes and the small
// per-value facts the lists carry (scale intervals, retrigger fractions, which outputs an assign mode offers).
// Pure C++20, no allocation; the engine may include this folder (module-layout.md).
#pragma once

#include "core/model/enums/Enums.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace core::model
{
// PadScale::scaleType, MidiScale::scaleTypeMidi: SET SCALE's 22 scales in the manual's order and names (addendum
// "Selection of scale", No. 1–22; CHROMATIC default). Schema 5 renumbered the list (MigrationV5.cpp).
enum class ScaleType : std::uint8_t
{
    Chromatic,
    Ionian,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Aeolian,
    Locrian,
    MajorPentatonic,
    MinorPentatonic,
    WholeTone,
    Diminish,
    CombinationDiminish,
    Altered,
    MajorBlues,
    MinorBlues,
    RagaBhairav,
    RagaGamanasrama,
    RagaTodi,
    Hawaiian,
    Ryukyu,
    JapaneseMiyakobushi
};

// TrackSequencerSettings::stepShiftArea (enumerations.md "Timing and record values": SHIFT AREA BAR, ALL*).
enum class StepShiftArea : std::uint8_t { Bar, All };

// TrackSequencerSettings::retriggerSpeed: the interval between repeats as a fraction of a step (SEQ-030, our list,
// ascending; default 1/4 step, M p.40). Displayed as note values (a step is a 1/16 note).
enum class RetriggerSpeed : std::uint8_t { Eighth, Sixth, Quarter, Third, Half, Whole };

// MidiTrack::mode (enumerations.md "MIDI and sync": MIDI mode NOTES, DRUM MAP; our fill-in, MID-045).
enum class MidiTrackMode : std::uint8_t { Notes, DrumMap };

// SceneRouting::headphoneSource (enumerations.md "Routing and outputs": HP CUE; stored CueMaster, Cue12 … Cue8 [DSP]).
enum class HeadphoneCue : std::uint8_t
{
    Master,
    Out12,
    Out34,
    Out56,
    Out78,
    Mono1,
    Mono2,
    Mono3,
    Mono4,
    Mono5,
    Mono6,
    Mono7,
    Mono8
};

// ProjectSettings::afsSendMode (enumerations.md AFS order "Insert Order": PRE FX*, POST FX).
enum class AfsSendMode : std::uint8_t { PreFx, PostFx };

// Track and scene colours (enumerations.md "Colours", our palette; track n and scene n default to index n − 1).
enum class PaletteColour : std::uint8_t
{
    Red,
    Vermilion,
    Orange,
    Amber,
    Gold,
    Yellow,
    Lime,
    Green,
    Jade,
    Cyan,
    SkyBlue,
    Periwinkle,
    Violet,
    Orchid,
    Pink,
    Rose
};

constexpr int kPaletteColours = 16;
constexpr int kChokeGroups = 8;        // 0 = none ("--"), 1–8 (TRK-020)
constexpr double kMinTempo = 20.0;     // SYN-002
constexpr double kMaxTempo = 300.0;
constexpr int kMinSwing = 50, kMaxSwing = 75; // stored 50–75 %, shown 0–50 % (SEQ-004)
constexpr int kMaxRetriggerCount = 16;  // RETRIGGER CNT 1–16 (SEQ-030)

// Semitone intervals of a scale from its root (enumerations.md "Scales"); Chromatic = all twelve.
std::span<const std::uint8_t> scaleIntervals(ScaleType) noexcept;

// The retrigger interval in steps (Quarter → 0.25).
constexpr double retriggerStepFraction(RetriggerSpeed s) noexcept
{
    switch (s)
    {
        case RetriggerSpeed::Eighth: return 1.0 / 8.0;
        case RetriggerSpeed::Sixth: return 1.0 / 6.0;
        case RetriggerSpeed::Quarter: return 0.25;
        case RetriggerSpeed::Third: return 1.0 / 3.0;
        case RetriggerSpeed::Half: return 0.5;
        case RetriggerSpeed::Whole: break;
    }
    return 1.0;
}

// Reference step trigger codes (SEQ-011, OQ-09): FULL 15, HALF 11, PARAM 8. Codes 9, 10, 12 (accepted by the
// reference, no known producer) and anything else have no TriggerType: the importer keeps them as "?n" (UI-501).
constexpr int referenceTriggerCode(TriggerType t) noexcept
{
    switch (t)
    {
        case TriggerType::Full: return 15;
        case TriggerType::Half: return 11;
        case TriggerType::LockOnly: break;
    }
    return 8;
}
constexpr std::optional<TriggerType> triggerTypeFromReferenceCode(int code) noexcept
{
    switch (code)
    {
        case 15: return TriggerType::Full;
        case 11: return TriggerType::Half;
        case 8: return TriggerType::LockOnly;
        default: break;
    }
    return std::nullopt;
}

// Which per-track outputs an assign mode offers (enumerations.md "Routing and outputs", MIX-030/031):
// M + 6CH: -- (master only), OUT3-4, OUT5-6, OUT7-8, 3 … 8; 8CH: OUT1-2 … OUT7-8, 1 … 8 (default OUT1-2).
constexpr bool outputAssignAvailable(OutputAssign o, AssignMode mode) noexcept
{
    if (mode == AssignMode::MasterPlus6)
        return o != OutputAssign::Out12 && o != OutputAssign::Mono1 && o != OutputAssign::Mono2;
    return o != OutputAssign::Master;
}
constexpr OutputAssign defaultOutputAssign(AssignMode mode) noexcept
{
    return mode == AssignMode::MasterPlus6 ? OutputAssign::Master : OutputAssign::Out12;
}
// HP CUE: 8CH offers every cue, M + 6CH all but OUT1-2, 1 and 2.
constexpr bool headphoneCueAvailable(HeadphoneCue c, AssignMode mode) noexcept
{
    return mode == AssignMode::EightChannel ||
           (c != HeadphoneCue::Out12 && c != HeadphoneCue::Mono1 && c != HeadphoneCue::Mono2);
}

// The effect types a slot lists, in SELECT FX grid order (enumerations.md "Effects", FX-040; NONE first).
std::span<const EffectType> insertEffectTypes() noexcept; // NONE, CHORUS, FLANGER, 2 BAND EQ, FILTER, DELAY, …
std::span<const EffectType> busEffectTypes() noexcept;    // NONE, DELAY, REVERB, CHORUS, FLANGER, 2 BAND EQ, …
bool effectAllowedOnInsert(EffectType) noexcept;
bool effectAllowedOnBus(EffectType) noexcept;
// The effect panel's header name: the type list's name, except FILTER → MULTI MODE FILTER [OBS].
std::string_view effectHeaderName(EffectType) noexcept;
} // namespace core::model
