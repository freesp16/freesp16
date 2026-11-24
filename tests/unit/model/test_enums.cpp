// Enum tables (03-ui/enumerations.md, UI-500…504, OQ-10): every enum round-trips through its stored string, every value
// has a display name, the spec's display names and stored reference strings are where enumerations.md puts them.
#include "core/model/enums/EnumStrings.h"
#include "core/model/enums/ValueLists.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace core::model;

namespace
{
template <typename En>
void checkRoundTrip()
{
    const auto& table = enumTable<En>();
    INFO(std::string(table.name));
    REQUIRE(enumCount<En>() > 0);
    std::set<std::string_view> stored;
    for (std::size_t i = 0; i < enumCount<En>(); ++i)
    {
        const auto v = static_cast<En>(i);
        CHECK(isValidEnum(v));
        CHECK_FALSE(storedName(v).empty());
        CHECK_FALSE(displayName(v).empty());
        CHECK(displayName(v) != "?");
        CHECK(stored.insert(storedName(v)).second); // stored strings are unique within a table
        const auto back = enumFromStored<En>(storedName(v));
        REQUIRE(back.has_value());
        CHECK(*back == v);
        if (! table.entries[i].alias.empty())
            CHECK(enumFromStored<En>(table.entries[i].alias) == v);
        CHECK(enumFromInt<En>(static_cast<int>(i)) == v);
    }
    // aliases never shadow another entry's stored string
    for (const auto& e : table.entries)
        if (! e.alias.empty())
            CHECK((stored.count(e.alias) == 0 || enumFromStored<En>(e.alias) == static_cast<En>(e.value)));
    const auto outside = static_cast<En>(enumCount<En>());
    CHECK_FALSE(isValidEnum(outside));
    CHECK(storedName(outside).empty());
    CHECK(displayName(outside) == "?");
    CHECK_FALSE(enumFromStored<En>("no such value").has_value());
    CHECK_FALSE(enumFromStored<En>("").has_value());
    CHECK_FALSE(enumFromInt<En>(-1).has_value());
}
} // namespace

TEST_CASE("every model enum round-trips through its stored string", "[model][enum]")
{
#define CHECK_ENUM(E) checkRoundTrip<E>();
    APP_MODEL_ENUMS(CHECK_ENUM)
#undef CHECK_ENUM
    std::set<std::string_view> names;
    for (const auto* t : allEnumTables())
    {
        REQUIRE(t != nullptr);
        CHECK(names.insert(t->name).second);
        CHECK(t->version >= 1);
        for (std::size_t i = 0; i < t->entries.size(); ++i)
            CHECK(t->entries[i].value == i);
    }
    CHECK(names.size() == 22);
}

TEST_CASE("enum display names follow enumerations.md", "[model][enum]")
{
    CHECK(displayName(TrackMode::Sample) == "SAMPLE");
    CHECK(shortDisplayName(TrackMode::Sample) == "SMPL");
    CHECK(displayName(TrackMode::Plugin) == "PLUGIN");
    CHECK(shortDisplayName(TrackMode::Plugin) == "PLUG");
    CHECK(storedName(TrackMode::Plugin) == "Plugin");
    CHECK(displayName(TriggerMode::OneShot) == "ONE SHOT");
    CHECK(storedName(TriggerMode::OneShot) == "OneShot"); // 1.60 form [OBS]
    CHECK(displayName(StretchMode::MasterTempo) == "MASTER TEMPO");
    CHECK(shortDisplayName(StretchMode::MasterTempo) == "M.TMP");
    CHECK(shortDisplayName(StretchMode::Resample) == "RESMPL");
    CHECK(displayName(EnvelopeCurve::Linear) == "LINEAR");
    CHECK(displayName(EffectType::Off) == "NONE");
    CHECK(displayName(EffectType::ParametricEq2) == "2 BAND EQ");
    CHECK(storedName(EffectType::ParametricEq2) == "2BandEQ");
    CHECK(storedName(EffectType::SendDelay) == "Delay");
    CHECK(storedName(EffectType::InsertDelay) == "InsertDelay");
    CHECK(displayName(EffectType::LoFi) == "LO-FI");
    CHECK(effectHeaderName(EffectType::MultiModeFilter) == "MULTI MODE FILTER");
    CHECK(effectHeaderName(EffectType::Chorus) == "CHORUS");
    CHECK(displayName(TriggerType::LockOnly) == "PARAM");
    CHECK(displayName(AssignMode::MasterPlus6) == "M + 6CH");
    CHECK(storedName(AssignMode::EightChannel) == "4Assign");
    CHECK(enumFromStored<AssignMode>("M+6CH") == AssignMode::MasterPlus6); // 1.60 spelling (UI-504)
    CHECK(displayName(OutputAssign::Master) == "--");
    CHECK(displayName(OutputAssign::Out34) == "OUT3-4");
    CHECK(displayName(OutputAssign::Mono8) == "8");
    CHECK(storedName(HeadphoneCue::Master) == "CueMaster");
    CHECK(storedName(HeadphoneCue::Mono3) == "Cue3");
    CHECK(enumFromStored<FxMode>("EffectModeSend") == FxMode::Send);
    CHECK(displayName(ScaleType::Chromatic) == "CHROMATIC");
    CHECK(enumFromStored<ScaleType>("CHROMA") == ScaleType::Chromatic); // imported 1.60 form maps to CHROMATIC
    CHECK(displayName(LfoShape::SawDown) == "SAW");
    CHECK(displayName(LfoTriggerMode::Retrigger) == "TRIGGER");
    CHECK(enumFromStored<QuantizeUnit>("1/16") == QuantizeUnit::Step); // native-format sketch spelling
    CHECK(enumFromStored<QuantizeUnit>("1bar") == QuantizeUnit::Bar);
    CHECK(displayName(RetriggerSpeed::Quarter) == "1/64");
    CHECK(shortDisplayName(PaletteColour::Red) == "#01");
    CHECK(shortDisplayName(PaletteColour::Rose) == "#16");
    CHECK(displayName(StepShiftArea::All) == "ALL");
    CHECK(displayName(MidiTrackMode::DrumMap) == "DRUM MAP");
}

TEST_CASE("unknown imported enum values show as ?<stored> (UI-501)", "[model][enum]")
{
    char buf[16];
    CHECK(formatUnknownEnum("Pulse25", buf, sizeof buf) == 8);
    CHECK(std::string(buf) == "?Pulse25");
    CHECK(formatUnknownEnum("AVeryLongStoredValue", buf, 8) == 7);
    CHECK(std::string(buf) == "?AVeryL");
    CHECK(formatUnknownEnum("x", nullptr, 0) == 0);
}

TEST_CASE("value lists: scales, retrigger, trigger codes, outputs, FX slot lists", "[model][enum]")
{
    CHECK(scaleIntervals(ScaleType::Chromatic).size() == 12);
    const auto major = scaleIntervals(ScaleType::Ionian);
    CHECK(std::vector<int>(major.begin(), major.end()) == std::vector<int> {0, 2, 4, 5, 7, 9, 11});
    CHECK(scaleIntervals(ScaleType::MinorPentatonic).size() == 5);
    for (std::size_t i = 0; i < enumCount<ScaleType>(); ++i)
    {
        const auto iv = scaleIntervals(static_cast<ScaleType>(i));
        REQUIRE_FALSE(iv.empty());
        CHECK(iv[0] == 0);
        for (std::size_t k = 1; k < iv.size(); ++k)
            CHECK(iv[k] > iv[k - 1]);
        CHECK(iv.back() < 12);
    }

    CHECK(retriggerStepFraction(RetriggerSpeed::Quarter) == 0.25);
    CHECK(retriggerStepFraction(RetriggerSpeed::Whole) == 1.0);
    for (std::size_t i = 1; i < enumCount<RetriggerSpeed>(); ++i)
        CHECK(retriggerStepFraction(static_cast<RetriggerSpeed>(i)) >
              retriggerStepFraction(static_cast<RetriggerSpeed>(i - 1)));

    STATIC_REQUIRE(referenceTriggerCode(TriggerType::Full) == 15);
    STATIC_REQUIRE(referenceTriggerCode(TriggerType::Half) == 11);
    STATIC_REQUIRE(referenceTriggerCode(TriggerType::LockOnly) == 8);
    for (auto t : {TriggerType::Full, TriggerType::Half, TriggerType::LockOnly})
        CHECK(triggerTypeFromReferenceCode(referenceTriggerCode(t)) == t);
    for (int code : {0, 9, 10, 12, 16})
        CHECK_FALSE(triggerTypeFromReferenceCode(code).has_value());

    CHECK(outputAssignAvailable(OutputAssign::Master, AssignMode::MasterPlus6));
    CHECK_FALSE(outputAssignAvailable(OutputAssign::Out12, AssignMode::MasterPlus6));
    CHECK_FALSE(outputAssignAvailable(OutputAssign::Mono2, AssignMode::MasterPlus6));
    CHECK(outputAssignAvailable(OutputAssign::Mono3, AssignMode::MasterPlus6));
    CHECK_FALSE(outputAssignAvailable(OutputAssign::Master, AssignMode::EightChannel));
    CHECK(defaultOutputAssign(AssignMode::EightChannel) == OutputAssign::Out12);
    CHECK_FALSE(headphoneCueAvailable(HeadphoneCue::Out12, AssignMode::MasterPlus6));
    CHECK(headphoneCueAvailable(HeadphoneCue::Out12, AssignMode::EightChannel));

    // FX-040 slot lists, SELECT FX grid order, NONE first; DELAY means the insert delay on inserts, the send delay on
    // buses; REVERB on both (on the inserts it is ours, after the reference types), DUCKER only on inserts; VINTAGE
    // PITCH (ours) last on both.
    REQUIRE(insertEffectTypes().size() == 13);
    REQUIRE(busEffectTypes().size() == 12);
    CHECK(insertEffectTypes().back() == EffectType::VintagePitch);
    CHECK(busEffectTypes().back() == EffectType::VintagePitch);
    CHECK(insertEffectTypes()[0] == EffectType::Off);
    CHECK(busEffectTypes()[1] == EffectType::SendDelay);
    CHECK(effectAllowedOnInsert(EffectType::InsertDelay));
    CHECK_FALSE(effectAllowedOnInsert(EffectType::SendDelay));
    CHECK(effectAllowedOnInsert(EffectType::Reverb));
    CHECK(insertEffectTypes()[11] == EffectType::Reverb);
    CHECK(effectAllowedOnBus(EffectType::Reverb));
    CHECK_FALSE(effectAllowedOnBus(EffectType::Ducker));
    CHECK_FALSE(effectAllowedOnBus(EffectType::InsertDelay));
    for (std::size_t i = 1; i < enumCount<EffectType>(); ++i)
    {
        const auto t = static_cast<EffectType>(i);
        CHECK((effectAllowedOnInsert(t) || effectAllowedOnBus(t))); // every live effect has a slot
    }
}
