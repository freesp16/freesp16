// The enum tables (03-ui/enumerations.md; OQ-10: one table per enum). Evidence tags per row follow enumerations.md;
// "ours" marks Sixteen's own stored string or display name where the reference value is unknown.
#include "core/model/enums/EnumStrings.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace core::model
{
namespace
{
using E = EnumEntry;

// Tracks (enumerations.md "Track type", "Trigger mode").
constexpr E kTrackMode[] = {
    {0, "Sample", "SAMPLE", "SMPL"}, // [FMT/MAN]
    {1, "Midi", "MIDI", "MIDI"},     // [FMT/OBS]
    {2, "Thru", "THRU", "THRU"},     // [FMT/MAN]
    {3, "Instrument", "INSTRUMENT", "INST"}, // [OBS] fourth mode; the UI shows the profile's name (MID-060, ours)
};
constexpr E kTriggerMode[] = {
    {0, "OneShot", "ONE SHOT"}, // [FMT/OBS/MAN]; 1.60 writes `OneShot`
    {1, "Gate", "GATE"},        // stored name unknown: ours
};
// Sample playback (TIME STRETCH; short forms RESMPL, M.TMP [MAN/OBS]).
constexpr E kStretchMode[] = {
    {0, "Off", "OFF"},
    {1, "Resample", "RESAMPLE", "RESMPL"},
    {2, "MasterTempo", "MASTER TEMPO", "M.TMP"},
};
// Amp envelope curves (LINEAR*, CURVE; [CLS] `…Curve_Linear`).
constexpr E kEnvelopeCurve[] = {
    {0, "Linear", "LINEAR"},
    {1, "Curve", "CURVE"},
};
// Effects: stored type names [DSP] (`Delay` = the send delay, `InsertDelay` the insert one); display = the SELECT FX
// list names (both delays are DELAY; the header name of FILTER is MULTI MODE FILTER, effectHeaderName()).
constexpr E kEffectType[] = {
    {0, "Off", "NONE"},
    {1, "Chorus", "CHORUS"},
    {2, "Flanger", "FLANGER"},
    {3, "Phaser", "PHASER"},
    {4, "2BandEQ", "2 BAND EQ", "2BAND EQ"},
    {5, "MultiModeFilter", "FILTER"},
    {6, "InsertDelay", "DELAY"},
    {7, "Delay", "DELAY", {}, "SendDelay"},
    {8, "Reverb", "REVERB"},
    {9, "LoFi", "LO-FI"},
    {10, "Distortion", "DISTORTION", "DIST"},
    {11, "Compressor", "COMPRESSOR", "COMP"},
    {12, "Ducker", "DUCKER"},
    {13, "VintagePitch", "VINTAGE PITCH"}, // ours: not an SP-16 effect (the SP-16 export writes Off)
};
// LFO shapes: only `Sine` was seen stored [FMT]; the others are ours. The enum has SawUp/SawDown and lacks
// PULSE25/PULSE12 of the 1.60 list: SawDown is the falling SAW of LFO-001.
constexpr E kLfoShape[] = {
    {0, "Sine", "SINE"},
    {1, "Triangle", "TRIANGLE", "TRI"},
    {2, "SawUp", "SAW UP", "SAW UP"},          // ours (not in the 1.60 list)
    {3, "SawDown", "SAW", "SAW", "Saw"},       // falling saw (LFO-001)
    {4, "Square", "SQUARE", "SQUARE"},         // pulse 50 %
    {5, "Random", "RANDOM", "RANDOM"},         // sample & hold
};
// LFO SETTING Mode: 1.60 shows `Trigger` (default) [OBS]; FREE is ours; ONE SHOT is Sixteen's (LFO-007).
constexpr E kLfoTriggerMode[] = {
    {0, "Free", "FREE"},
    {1, "Trigger", "TRIGGER", {}, "Retrigger"},
    {2, "OneShot", "ONE SHOT"},
};
// LFO SETTING Trigger On: the enum's values (pad / step / both) are Sixteen's; 1.60 shows FULL /
// FULL & HALF.
constexpr E kLfoTriggerOn[] = {
    {0, "Pad", "PAD"},
    {1, "Step", "STEP"},
    {2, "Both", "PAD & STEP", "BOTH"},
};
// Step trigger types (SEQ-011): reference codes 15 / 11 / 8 in ValueLists.h (referenceTriggerCode).
constexpr E kTriggerType[] = {
    {0, "Full", "FULL"},
    {1, "Half", "HALF"},
    {2, "LockOnly", "PARAM", {}, "Param"},
};
// Lock change to the next step (SEQ-021, ours: Revert default, Hold, Glide; native-format sketch `"Revert"`).
constexpr E kLockChange[] = {
    {0, "Revert", "REVERT"},
    {1, "Hold", "HOLD"},
    {2, "Glide", "GLIDE"},
};
// Quantise boundaries: PATTERN QUANTIZE STEP, BAR, PATTERN* [MAN] (M p.42); pad (record) quantise OFF … [FMT].
// Aliases = the native-format sketch's spellings (`1/16`, `1bar`).
constexpr E kQuantizeUnit[] = {
    {0, "Off", "OFF"},
    {1, "Step", "STEP", "1/16", "1/16"},
    {2, "Beat", "BEAT", "1/4", "1/4"},
    {3, "Bar", "BAR", {}, "1bar"},
    {4, "Pattern", "PATTERN"},
};
// FX mode per scene (OQ-12): BOTH is Sixteen's default (FX-009); SEND / MASTER parity modes [DSP] (stored
// `EffectModeSend` / `EffectModeMaster` in the reference: accepted as aliases).
constexpr E kFxMode[] = {
    {0, "Both", "BOTH"},
    {1, "Send", "SEND", {}, "EffectModeSend"},
    {2, "Master", "MASTER", {}, "EffectModeMaster"},
};
// ASSIGN MODE [DSP/MAN] (M p.23); 1.60 writes `M+6CH`.
constexpr E kAssignMode[] = {
    {0, "Master", "M + 6CH", "M+6CH", "M+6CH"},
    {1, "4Assign", "8CH", "8CH", "8CH"},
};
// OUT ASSIGN per track: reference values unknown, stored names ours; display [MAN] (M p.23, p.46).
constexpr E kOutputAssign[] = {
    {0, "Master", "--"},     {1, "Out12", "OUT1-2"}, {2, "Out34", "OUT3-4"}, {3, "Out56", "OUT5-6"},
    {4, "Out78", "OUT7-8"},  {5, "Mono1", "1"},      {6, "Mono2", "2"},      {7, "Mono3", "3"},
    {8, "Mono4", "4"},       {9, "Mono5", "5"},      {10, "Mono6", "6"},     {11, "Mono7", "7"},
    {12, "Mono8", "8"},
};
// AFS route mode: the enum merges the reference's route (MASTER / TRACK n) and order (PRE / POST FX), our choice
// (the order also lives in afsSendMode, AfsSendMode below).
constexpr E kAfsRouteMode[] = {
    {0, "Off", "OFF"},
    {1, "Pre", "PRE FX"},
    {2, "Post", "POST FX"},
    {3, "Master", "MASTER"},
};
// Scales: the manual's list and names (addendum "Selection of scale", No. 1–22); stored = the name without spaces
// (the SP-16 importer also matches the reference's strings loosely, Sp16Import.cpp). 1.60 shows CHROMATIC as `CHROMA`
// [OBS] (alias). Version 2: the 14-scale fill-in replaced (native schema 5, MigrationV5.cpp).
constexpr E kScaleType[] = {
    {0, "Chromatic", "CHROMATIC", "CHROMA", "CHROMA"},
    {1, "Ionian", "IONIAN"},
    {2, "Dorian", "DORIAN"},
    {3, "Phrygian", "PHRYGIAN"},
    {4, "Lydian", "LYDIAN"},
    {5, "Mixolydian", "MIXOLYDIAN", "MIXOLYD"},
    {6, "Aeolian", "AEOLIAN"},
    {7, "Locrian", "LOCRIAN"},
    {8, "MajorPentatonic", "MAJOR PENTATONIC", "MAJ PENTA"},
    {9, "MinorPentatonic", "MINOR PENTATONIC", "MIN PENTA"},
    {10, "WholeTone", "WHOLE TONE", "WHOLE"},
    {11, "Diminish", "DIMINISH"},
    {12, "CombinationDiminish", "COMBINATION DIMINISH", "COMB DIM"},
    {13, "Altered", "ALTERED"},
    {14, "MajorBlues", "MAJOR BLUES", "MAJ BLUES"},
    {15, "MinorBlues", "MINOR BLUES", "MIN BLUES"},
    {16, "RagaBhairav", "RAGA BHAIRAV", "BHAIRAV"},
    {17, "RagaGamanasrama", "RAGA GAMANASRAMA", "GAMANASRAMA"},
    {18, "RagaTodi", "RAGA TODI", "TODI"},
    {19, "Hawaiian", "HAWAIIAN"},
    {20, "Ryukyu", "RYUKYU"},
    {21, "JapaneseMiyakobushi", "JAPANESE MIYAKOBUSHI", "MIYAKOBUSHI"},
};
// SHIFT AREA BAR, ALL* [MAN] (M p.40), default [OBS].
constexpr E kStepShiftArea[] = {
    {0, "Bar", "BAR"},
    {1, "All", "ALL"},
};
// RETRIGGER SPD (SEQ-030, our list): stored as the step fraction, displayed as the note value (1 step = 1/16).
constexpr E kRetriggerSpeed[] = {
    {0, "1/8", "1/128"}, {1, "1/6", "1/96"}, {2, "1/4", "1/64"}, {3, "1/3", "1/48"}, {4, "1/2", "1/32"}, {5, "1", "1/16"},
};
// MIDI mode (`midimode` int [FMT]; values our fill-in, MID-045).
constexpr E kMidiTrackMode[] = {
    {0, "Notes", "NOTES"},
    {1, "DrumMap", "DRUM MAP"},
};
// HP CUE (stored [DSP], display [MAN] M p.23, default MASTER [OBS]).
constexpr E kHeadphoneCue[] = {
    {0, "CueMaster", "MASTER"}, {1, "Cue12", "OUT1-2"}, {2, "Cue34", "OUT3-4"}, {3, "Cue56", "OUT5-6"},
    {4, "Cue78", "OUT7-8"},     {5, "Cue1", "1"},       {6, "Cue2", "2"},       {7, "Cue3", "3"},
    {8, "Cue4", "4"},           {9, "Cue5", "5"},       {10, "Cue6", "6"},      {11, "Cue7", "7"},
    {12, "Cue8", "8"},
};
// AFS "Insert Order" PRE FX*, POST FX [DSP] (reference strings are mapped by the importer).
constexpr E kAfsSendMode[] = {
    {0, "PreFx", "PRE FX"},
    {1, "PostFx", "POST FX"},
};
// Palette (enumerations.md "Colours", our palette); the UI shows pad colours as #01 … #16.
constexpr E kPaletteColour[] = {
    {0, "Red", "RED", "#01"},           {1, "Vermilion", "VERMILION", "#02"}, {2, "Orange", "ORANGE", "#03"},
    {3, "Amber", "AMBER", "#04"},       {4, "Gold", "GOLD", "#05"},           {5, "Yellow", "YELLOW", "#06"},
    {6, "Lime", "LIME", "#07"},         {7, "Green", "GREEN", "#08"},         {8, "Jade", "JADE", "#09"},
    {9, "Cyan", "CYAN", "#10"},         {10, "SkyBlue", "SKY BLUE", "#11"},   {11, "Periwinkle", "PERIWINKLE", "#12"},
    {12, "Violet", "VIOLET", "#13"},    {13, "Orchid", "ORCHID", "#14"},      {14, "Pink", "PINK", "#15"},
    {15, "Rose", "ROSE", "#16"},
};

// Table metadata: name, version (UI-502), fill-in flag.
#define APP_DETAIL_TABLE(Type, version, fillIn)                                                                        \
    constexpr EnumTable kTable##Type {#Type, version, fillIn, k##Type};

APP_DETAIL_TABLE(TrackMode, 1, true)
APP_DETAIL_TABLE(TriggerMode, 1, true)
APP_DETAIL_TABLE(StretchMode, 1, true)
APP_DETAIL_TABLE(EnvelopeCurve, 1, false)
APP_DETAIL_TABLE(EffectType, 1, false)
APP_DETAIL_TABLE(LfoShape, 1, true)
APP_DETAIL_TABLE(LfoTriggerMode, 1, true)
APP_DETAIL_TABLE(LfoTriggerOn, 1, true)
APP_DETAIL_TABLE(TriggerType, 1, false)
APP_DETAIL_TABLE(LockChange, 1, true)
APP_DETAIL_TABLE(QuantizeUnit, 1, true)
APP_DETAIL_TABLE(FxMode, 1, true)
APP_DETAIL_TABLE(AssignMode, 1, false)
APP_DETAIL_TABLE(OutputAssign, 1, true)
APP_DETAIL_TABLE(AfsRouteMode, 1, true)
APP_DETAIL_TABLE(ScaleType, 2, false)
APP_DETAIL_TABLE(StepShiftArea, 1, false)
APP_DETAIL_TABLE(RetriggerSpeed, 1, true)
APP_DETAIL_TABLE(MidiTrackMode, 1, true)
APP_DETAIL_TABLE(HeadphoneCue, 1, false)
APP_DETAIL_TABLE(AfsSendMode, 1, false)
APP_DETAIL_TABLE(PaletteColour, 1, true)
#undef APP_DETAIL_TABLE

// Compile-time checks: entries are in value order, and every table has as many entries as its enum has values
// (the last enumerator + 1).
template <std::size_t N>
constexpr bool inValueOrder(const E (&entries)[N])
{
    for (std::size_t i = 0; i < N; ++i)
        if (entries[i].value != i || entries[i].stored.empty() || entries[i].display.empty())
            return false;
    return true;
}
#define APP_DETAIL_CHECK(Type, Last)                                                                                   \
    static_assert(inValueOrder(k##Type), #Type ": entries out of value order or empty");                               \
    static_assert(std::size(k##Type) == static_cast<std::size_t>(Type::Last) + 1, #Type ": table size");
APP_DETAIL_CHECK(TrackMode, Instrument)
APP_DETAIL_CHECK(TriggerMode, Gate)
APP_DETAIL_CHECK(StretchMode, MasterTempo)
APP_DETAIL_CHECK(EnvelopeCurve, Curve)
APP_DETAIL_CHECK(EffectType, VintagePitch)
APP_DETAIL_CHECK(LfoShape, Random)
APP_DETAIL_CHECK(LfoTriggerMode, OneShot)
APP_DETAIL_CHECK(LfoTriggerOn, Both)
APP_DETAIL_CHECK(TriggerType, LockOnly)
APP_DETAIL_CHECK(LockChange, Glide)
APP_DETAIL_CHECK(QuantizeUnit, Pattern)
APP_DETAIL_CHECK(FxMode, Master)
APP_DETAIL_CHECK(AssignMode, EightChannel)
APP_DETAIL_CHECK(OutputAssign, Mono8)
APP_DETAIL_CHECK(AfsRouteMode, Master)
APP_DETAIL_CHECK(ScaleType, JapaneseMiyakobushi)
APP_DETAIL_CHECK(StepShiftArea, All)
APP_DETAIL_CHECK(RetriggerSpeed, Whole)
APP_DETAIL_CHECK(MidiTrackMode, DrumMap)
APP_DETAIL_CHECK(HeadphoneCue, Mono8)
APP_DETAIL_CHECK(AfsSendMode, PostFx)
APP_DETAIL_CHECK(PaletteColour, Rose)
#undef APP_DETAIL_CHECK

#define APP_DETAIL_TABLE_PTR(Type) &kTable##Type,
constexpr const EnumTable* kAllTables[] = {APP_MODEL_ENUMS(APP_DETAIL_TABLE_PTR)};
#undef APP_DETAIL_TABLE_PTR

// Scale intervals (semitones from the root). The manual names the scales only; the intervals are the usual ones
// (Diminish = whole-half, Combination Diminish = half-whole).
constexpr std::uint8_t kChromatic[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::uint8_t kIonian[] = {0, 2, 4, 5, 7, 9, 11};
constexpr std::uint8_t kDorian[] = {0, 2, 3, 5, 7, 9, 10};
constexpr std::uint8_t kPhrygian[] = {0, 1, 3, 5, 7, 8, 10};
constexpr std::uint8_t kLydian[] = {0, 2, 4, 6, 7, 9, 11};
constexpr std::uint8_t kMixolydian[] = {0, 2, 4, 5, 7, 9, 10};
constexpr std::uint8_t kAeolian[] = {0, 2, 3, 5, 7, 8, 10};
constexpr std::uint8_t kLocrian[] = {0, 1, 3, 5, 6, 8, 10};
constexpr std::uint8_t kMajorPentatonic[] = {0, 2, 4, 7, 9};
constexpr std::uint8_t kMinorPentatonic[] = {0, 3, 5, 7, 10};
constexpr std::uint8_t kWholeTone[] = {0, 2, 4, 6, 8, 10};
constexpr std::uint8_t kDiminish[] = {0, 2, 3, 5, 6, 8, 9, 11};
constexpr std::uint8_t kCombinationDiminish[] = {0, 1, 3, 4, 6, 7, 9, 10};
constexpr std::uint8_t kAltered[] = {0, 1, 3, 4, 6, 8, 10};
constexpr std::uint8_t kMajorBlues[] = {0, 2, 3, 4, 7, 9};
constexpr std::uint8_t kMinorBlues[] = {0, 3, 5, 6, 7, 10};
constexpr std::uint8_t kRagaBhairav[] = {0, 1, 4, 5, 7, 8, 11};
constexpr std::uint8_t kRagaGamanasrama[] = {0, 1, 4, 6, 7, 9, 11};
constexpr std::uint8_t kRagaTodi[] = {0, 1, 3, 6, 7, 8, 11};
constexpr std::uint8_t kHawaiian[] = {0, 2, 3, 5, 7, 9, 11};
constexpr std::uint8_t kRyukyu[] = {0, 4, 5, 7, 11};
constexpr std::uint8_t kJapaneseMiyakobushi[] = {0, 1, 5, 7, 8};
constexpr std::span<const std::uint8_t> kScales[] = {kChromatic,
                                                     kIonian,
                                                     kDorian,
                                                     kPhrygian,
                                                     kLydian,
                                                     kMixolydian,
                                                     kAeolian,
                                                     kLocrian,
                                                     kMajorPentatonic,
                                                     kMinorPentatonic,
                                                     kWholeTone,
                                                     kDiminish,
                                                     kCombinationDiminish,
                                                     kAltered,
                                                     kMajorBlues,
                                                     kMinorBlues,
                                                     kRagaBhairav,
                                                     kRagaGamanasrama,
                                                     kRagaTodi,
                                                     kHawaiian,
                                                     kRyukyu,
                                                     kJapaneseMiyakobushi};
static_assert(std::size(kScales) == std::size(kScaleType));

// SELECT FX grid order (enumerations.md "Effects", FX-040).
constexpr EffectType kInsertTypes[] = {
    EffectType::Off,           EffectType::Chorus,          EffectType::Flanger,
    EffectType::ParametricEq2, EffectType::MultiModeFilter, EffectType::InsertDelay,
    EffectType::LoFi,          EffectType::Compressor,      EffectType::Distortion,
    EffectType::Phaser,        EffectType::Ducker,          EffectType::VintagePitch};
constexpr EffectType kBusTypes[] = {
    EffectType::Off,        EffectType::SendDelay,     EffectType::Reverb,          EffectType::Chorus,
    EffectType::Flanger,    EffectType::ParametricEq2, EffectType::MultiModeFilter, EffectType::LoFi,
    EffectType::Compressor, EffectType::Distortion,    EffectType::Phaser,          EffectType::VintagePitch};

template <std::size_t N> constexpr bool contains(const EffectType (&list)[N], EffectType t) noexcept
{
    for (auto x : list)
        if (x == t)
            return true;
    return false;
}
} // namespace

const EnumEntry* EnumTable::byStored(std::string_view stored) const noexcept
{
    for (const auto& e : entries)
        if (e.stored == stored)
            return &e;
    if (! stored.empty())
        for (const auto& e : entries)
            if (e.alias == stored)
                return &e;
    return nullptr;
}

const EnumEntry* EnumTable::byDisplay(std::string_view display) const noexcept
{
    for (const auto& e : entries)
        if (e.display == display)
            return &e;
    return nullptr;
}

#define APP_DETAIL_DEFINE_ENUM_TABLE(Type)                                                                             \
    template <>                                                                                                        \
    const EnumTable& enumTable<Type>() noexcept                                                                        \
    {                                                                                                                  \
        return kTable##Type;                                                                                           \
    }
APP_MODEL_ENUMS(APP_DETAIL_DEFINE_ENUM_TABLE)
#undef APP_DETAIL_DEFINE_ENUM_TABLE

std::span<const EnumTable* const> allEnumTables() noexcept { return kAllTables; }

int formatUnknownEnum(std::string_view stored, char* buf, int bufSize) noexcept
{
    if (buf == nullptr || bufSize <= 0)
        return 0;
    const int n = std::snprintf(buf, static_cast<std::size_t>(bufSize), "?%.*s", static_cast<int>(stored.size()),
                                stored.data());
    return std::clamp(n, 0, bufSize - 1);
}

std::span<const std::uint8_t> scaleIntervals(ScaleType s) noexcept
{
    const auto i = static_cast<std::size_t>(s);
    return i < std::size(kScales) ? kScales[i] : kScales[0];
}

std::span<const EffectType> insertEffectTypes() noexcept { return kInsertTypes; }
std::span<const EffectType> busEffectTypes() noexcept { return kBusTypes; }
bool effectAllowedOnInsert(EffectType t) noexcept { return contains(kInsertTypes, t); }
bool effectAllowedOnBus(EffectType t) noexcept { return contains(kBusTypes, t); }

std::string_view effectHeaderName(EffectType t) noexcept
{
    return t == EffectType::MultiModeFilter ? std::string_view {"MULTI MODE FILTER"} : displayName(t);
}
} // namespace core::model
