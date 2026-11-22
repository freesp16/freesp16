// Stored and display strings of every model enum (03-ui/enumerations.md; UI-500…504, OQ-10: one table per enum).
//
//   stored   the string a file holds: the reference project-file string where enumerations.md knows it (`OneShot`,
//            `2BandEQ`, `Master`/`4Assign`, `CueMaster`), otherwise Sixteen's own PascalCase name. The native
//            codec writes it; both codecs read it (and `alias`, a second accepted spelling such as the 1.60 form
//            `CHROMA` or the native-format sketch's `1/16`).
//   display  what the UI shows (UI-500: never a raw code). `shortDisplay` is the tile / target-list form, "" = display.
//
// Each table lists its entries in value order (entries[i].value == i), so value → string is an index (no search, no
// allocation: usable from paint code). String → value is a short linear scan (decode, message thread / workers).
// UI-501: a stored string that is in no table is kept by the codec (Extras) and shown as `?<stored>`
// (formatUnknownEnum). UI-502: a table whose list is our own fill-in has `fillIn` set and a `version` that is bumped
// whenever the list changes (then the importer mapping, resources/interop/sp16-mapping.json, is updated too).
#pragma once

#include "core/model/enums/Enums.h"
#include "core/model/enums/ValueLists.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace core::model
{
struct EnumEntry
{
    std::uint8_t value;
    std::string_view stored;
    std::string_view display;
    std::string_view shortDisplay {}; // "" = display
    std::string_view alias {};        // another accepted stored spelling on read, "" = none

    constexpr std::string_view shortOrDisplay() const noexcept { return shortDisplay.empty() ? display : shortDisplay; }
};

struct EnumTable
{
    std::string_view name; // C++ enum name, e.g. "TrackMode"
    int version = 1;       // UI-502
    bool fillIn = false;   // the list is (partly) our own fill-in
    std::span<const EnumEntry> entries;

    const EnumEntry* byValue(int value) const noexcept
    {
        return value >= 0 && static_cast<std::size_t>(value) < entries.size() ? &entries[static_cast<std::size_t>(value)]
                                                                               : nullptr;
    }
    // Exact stored string first, then an alias.
    const EnumEntry* byStored(std::string_view stored) const noexcept;
    // Display name (case-sensitive), for UI list pickers and tests.
    const EnumEntry* byDisplay(std::string_view display) const noexcept;
};

// Every model enum (X(Type)): the tables are defined in EnumStrings.cpp.
#define APP_MODEL_ENUMS(X)                                                                                             \
    X(TrackMode)                                                                                                       \
    X(TriggerMode)                                                                                                     \
    X(StretchMode)                                                                                                     \
    X(EnvelopeCurve)                                                                                                   \
    X(EffectType)                                                                                                      \
    X(LfoShape)                                                                                                        \
    X(LfoTriggerMode)                                                                                                  \
    X(LfoTriggerOn)                                                                                                    \
    X(TriggerType)                                                                                                     \
    X(LockChange)                                                                                                      \
    X(QuantizeUnit)                                                                                                    \
    X(FxMode)                                                                                                          \
    X(AssignMode)                                                                                                      \
    X(OutputAssign)                                                                                                    \
    X(AfsRouteMode)                                                                                                    \
    X(ScaleType)                                                                                                       \
    X(StepShiftArea)                                                                                                   \
    X(RetriggerSpeed)                                                                                                  \
    X(MidiTrackMode)                                                                                                   \
    X(HeadphoneCue)                                                                                                    \
    X(AfsSendMode)                                                                                                     \
    X(PaletteColour)

template <typename E>
const EnumTable& enumTable() noexcept; // specialised for every enum of APP_MODEL_ENUMS only

#define APP_DETAIL_DECLARE_ENUM_TABLE(E) template <> const EnumTable& enumTable<E>() noexcept;
APP_MODEL_ENUMS(APP_DETAIL_DECLARE_ENUM_TABLE)
#undef APP_DETAIL_DECLARE_ENUM_TABLE

std::span<const EnumTable* const> allEnumTables() noexcept; // every table, in APP_MODEL_ENUMS order

template <typename E>
std::size_t enumCount() noexcept
{
    return enumTable<E>().entries.size();
}
template <typename E>
bool isValidEnum(E v) noexcept
{
    return static_cast<std::size_t>(v) < enumCount<E>();
}
// "" for a value outside the table.
template <typename E>
std::string_view storedName(E v) noexcept
{
    const auto* e = enumTable<E>().byValue(static_cast<int>(v));
    return e != nullptr ? e->stored : std::string_view {};
}
// "?" for a value outside the table (UI-501 shows an unknown imported value as `?<stored>` via formatUnknownEnum).
template <typename E>
std::string_view displayName(E v) noexcept
{
    const auto* e = enumTable<E>().byValue(static_cast<int>(v));
    return e != nullptr ? e->display : std::string_view {"?"};
}
template <typename E>
std::string_view shortDisplayName(E v) noexcept
{
    const auto* e = enumTable<E>().byValue(static_cast<int>(v));
    return e != nullptr ? e->shortOrDisplay() : std::string_view {"?"};
}
template <typename E>
std::optional<E> enumFromStored(std::string_view stored) noexcept
{
    const auto* e = enumTable<E>().byStored(stored);
    return e != nullptr ? std::optional<E> {static_cast<E>(e->value)} : std::nullopt;
}
// For the int-typed model fields (ScaleType in PadScale::scaleType, …): the enum, or nullopt when out of range.
template <typename E>
std::optional<E> enumFromInt(int v) noexcept
{
    return v >= 0 && static_cast<std::size_t>(v) < enumCount<E>() ? std::optional<E> {static_cast<E>(v)}
                                                                    : std::nullopt;
}

// UI-501: writes "?<stored>" into buf (NUL-terminated, truncated to bufSize); returns the length written.
int formatUnknownEnum(std::string_view stored, char* buf, int bufSize) noexcept;
} // namespace core::model
