// TOUCH STRIP SETTING (PITCH RANGE) (07-ui-design/screens/utility.md "Rows"; 03-ui/screens.md "PITCH range": not a
// page, the UTILITY value row; RIB-002). The value is "N Bipolar Semitones", N = 1…12, 24 (±N × 100 cents); the list
// has 13 entries (> 8), so push opens a ListPopup (utility.md "Editing state"). An asymmetric range (an imported
// project) shows as "−200 / +700 cents". The project's RibbonSettings are edited through the ui::m2 session's bus
// (core::commands::ribbon::setPitchRange, one undo step).
#pragma once

#include <cstddef>
#include <string_view>

namespace core::model
{
struct RibbonSettings;
}

namespace ui::m4
{
inline constexpr std::string_view kPitchRangePopup = "utility.touchStrip.pitchRange"; // the ListPopup's id
inline constexpr std::string_view kPitchRangeInfo = "ribbon.pitchRange";               // the row's value source

// The row's value text of `r` into `out` ("2 Bipolar Semitones", "−200 / +700 cents"); returns its length.
int pitchRangeText(const core::model::RibbonSettings& r, char* out, std::size_t size) noexcept;
// The same for the ui::m2 session's project (the UTILITY row).
int pitchRangeText(char* out, std::size_t size) noexcept;
// The list's entry for a symmetric range of ±N semitones (0-based index into 1…12, 24), −1 = none.
int pitchRangeIndex(const core::model::RibbonSettings& r) noexcept;
} // namespace ui::m4
