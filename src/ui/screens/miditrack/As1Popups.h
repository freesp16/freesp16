// TORAIZ AS-1 track: the BANK SELECT / PROG CHANGE value texts (As1Popups.cpp, the pickers `as1.bankSelect` and
// `as1.progChange`; the mapping is core::midi::as1's, UNVERIFIED).
#pragma once

#include <string_view>

namespace ui
{
std::string_view as1BankText(int msb, int lsb) noexcept; // "U1" … "F5", "---" = unset / not an AS-1 bank
std::string_view as1ProgramText(int shown) noexcept;     // "P01" … "P99" (shown 1–99), "---" = none
} // namespace ui
