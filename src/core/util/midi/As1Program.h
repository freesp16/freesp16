// TORAIZ AS-1 preset selection by Bank Select + Program Change (the manual's 7.1 "Preset switch", addendum p.40-41):
// BANK SELECT picks [U1]~[U5] (User Bank) or [F1]~[F5] (Factory Bank), PROG CHANGE a program of that bank, both per
// scene (sent when the scene becomes current, MID-050).
//
// UNVERIFIED: neither the manual nor our research notes give the messages behind U1…F5 or the program range. Our
// choice, to be checked on an AS-1: Bank Select MSB (CC 0) = 0, LSB (CC 32) = 0–4 for U1–U5 and 5–9 for F1–F5;
// programs shown 1–99 and sent as Program Change 0–98.
#pragma once

#include <string_view>

namespace core::midi::as1
{
inline constexpr int kBanks = 10;    // U1–U5, F1–F5
inline constexpr int kBankMsb = 0;   // CC 0
inline constexpr int kPrograms = 99; // per bank: shown 1–99
inline constexpr std::string_view kBankNames[kBanks] = {"U1", "U2", "U3", "U4", "U5", "F1", "F2", "F3", "F4", "F5"};

// The bank (0–9 = U1…F5) the stored MSB / LSB select; −1 = none of them (unset, or another device's numbers).
constexpr int bankOf(int msb, int lsb) noexcept
{
    return msb == kBankMsb && lsb >= 0 && lsb < kBanks ? lsb : -1;
}
// The CC 32 value of bank 0–9.
constexpr int bankLsb(int bank) noexcept
{
    return bank;
}
// The shown program number (1–99) of a stored Program Change (0–98); −1 = unset or out of the AS-1's range.
constexpr int programShown(int programChange) noexcept
{
    return programChange >= 0 && programChange < kPrograms ? programChange + 1 : -1;
}
// The Program Change (0–98) of a shown program (1–99).
constexpr int programChange(int shown) noexcept
{
    return shown - 1;
}

static_assert(bankOf(0, 0) == 0 && bankOf(0, 9) == 9 && bankOf(0, 10) == -1 && bankOf(1, 0) == -1);
static_assert(programShown(0) == 1 && programShown(98) == 99 && programShown(99) == -1 && programShown(-1) == -1);
} // namespace core::midi::as1
