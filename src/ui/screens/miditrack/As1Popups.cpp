// TORAIZ AS-1 track: the BANK SELECT and PROG CHANGE pickers TRACK MENU's AS-1 header actions open (the manual's 7.1
// "Preset switch", addendum p.40-41: "Turn the rotary selector to select a Bank ... and push it to fix"). ListPopups
// `as1.bankSelect` (U1–U5, F1–F5) and `as1.progChange` (P01–P99), titled per scene as the MIDI track's popups, over
// the same midi.bankMsb / bankLsb / program values (per scene and track, sent on the scene switch, MID-050). The
// messages behind the names are core::midi::as1's (UNVERIFIED, As1Program.h).
#include "core/util/midi/As1Program.h"
#include "ui/descriptors/PopupSpec.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/Popup.h"
#include "ui/shell/PopupFactory.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/ListPopup.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui
{
namespace as1 = core::midi::as1;

// The PROG CHANGE value text "P01" … "P99" of a shown program (1–99); "---" for none.
std::string_view as1ProgramText(int shown) noexcept
{
    static const auto table = []
    {
        std::array<std::array<char, 4>, as1::kPrograms + 1> t {};
        std::snprintf(t[0].data(), 4, "---");
        for (int p = 1; p <= as1::kPrograms; ++p)
            std::snprintf(t[static_cast<std::size_t>(p)].data(), 4, "P%02d", p);
        return t;
    }();
    return table[static_cast<std::size_t>(shown >= 1 && shown <= as1::kPrograms ? shown : 0)].data();
}
// The BANK SELECT value text "U1" … "F5" of the stored MSB / LSB; "---" for none of them.
std::string_view as1BankText(int msb, int lsb) noexcept
{
    const int bank = as1::bankOf(msb, lsb);
    return bank < 0 ? std::string_view("---") : as1::kBankNames[bank];
}

namespace
{
int stored(ShellApi& sh, std::string_view id)
{
    return static_cast<int>(std::lround(m1::trackParam(sh, id, sh.state().selectedTrack, -1)));
}
void store(ShellApi& sh, std::string_view id, int v)
{
    m1::setTrackParam(sh, id, sh.state().selectedTrack, static_cast<float>(v));
}

std::string_view title(bool bank, int scene)
{
    static const auto titles = []
    {
        std::array<std::array<std::string, 16>, 2> t;
        for (int s = 0; s < 16; ++s)
        {
            char b[48];
            std::snprintf(b, sizeof b, "BANK SELECT \xC2\xB7 SCENE %02d", s + 1);
            t[0][static_cast<std::size_t>(s)] = b;
            std::snprintf(b, sizeof b, "PROG CHANGE \xC2\xB7 SCENE %02d", s + 1);
            t[1][static_cast<std::size_t>(s)] = b;
        }
        return t;
    }();
    return titles[bank ? 0 : 1][static_cast<std::size_t>(scene & 15)];
}

std::unique_ptr<PopupWidget> makeAs1Bank(const PopupSpec&, ShellApi& sh)
{
    ListRequest req;
    req.title = title(true, sh.state().scene - 1);
    for (int b = 0; b < as1::kBanks; ++b)
        req.items.push_back({as1::kBankNames[b], {}, {}, b});
    req.current = as1::bankOf(stored(sh, "midi.bankMsb"), stored(sh, "midi.bankLsb"));
    req.onChoose = [&sh](int, const ListItem& it)
    {
        store(sh, "midi.bankMsb", as1::kBankMsb);
        store(sh, "midi.bankLsb", as1::bankLsb(it.value));
    };
    return std::make_unique<ListPopup>(std::move(req));
}

std::unique_ptr<PopupWidget> makeAs1Program(const PopupSpec&, ShellApi& sh)
{
    ListRequest req;
    req.title = title(false, sh.state().scene - 1);
    for (int p = 1; p <= as1::kPrograms; ++p)
        req.items.push_back({as1ProgramText(p), {}, {}, p});
    const int shown = as1::programShown(stored(sh, "midi.program"));
    req.current = shown > 0 ? shown - 1 : -1;
    req.onChoose = [&sh](int, const ListItem& it) { store(sh, "midi.program", as1::programChange(it.value)); };
    return std::make_unique<ListPopup>(std::move(req));
}
} // namespace
} // namespace ui

APP_REGISTER_POPUP((ui::PopupSpec {.kind = ui::PopupSpec::Kind::List, .id = "as1.bankSelect", .title = "BANK SELECT"}))
APP_REGISTER_POPUP((ui::PopupSpec {.kind = ui::PopupSpec::Kind::List, .id = "as1.progChange", .title = "PROG CHANGE"}))
APP_REGISTER_POPUP_FACTORY("as1.bankSelect", &ui::makeAs1Bank)
APP_REGISTER_POPUP_FACTORY("as1.progChange", &ui::makeAs1Program)
