// TOUCH STRIP SETTING (PITCH RANGE): the UTILITY row and its ListPopup (see PitchRange.h).
#include "ui/screens/touchstrip/PitchRange.h"

#include "core/commands/ribbon/RibbonCommands.h"
#include "core/model/Project.h"
#include "ui/descriptors/SettingsSpec.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/PopupFactory.h"
#include "ui/widgets/ListPopup.h"

#include <array>
#include <cstdio>
#include <memory>

namespace ui::m4
{
namespace
{
constexpr std::array<int, 13> kSemitones{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 24};
constexpr std::string_view kLabels[] = {"1 Bipolar Semitone",   "2 Bipolar Semitones",  "3 Bipolar Semitones",
                                        "4 Bipolar Semitones",  "5 Bipolar Semitones",  "6 Bipolar Semitones",
                                        "7 Bipolar Semitones",  "8 Bipolar Semitones",  "9 Bipolar Semitones",
                                        "10 Bipolar Semitones", "11 Bipolar Semitones", "12 Bipolar Semitones",
                                        "24 Bipolar Semitones"};

std::unique_ptr<PopupWidget> makePitchRange(const PopupSpec& spec, ShellApi&)
{
    ListRequest r;
    r.title = spec.title;
    for (std::size_t i = 0; i < kSemitones.size(); ++i)
        r.items.push_back({kLabels[i], {}, {}, kSemitones[i]});
    auto& session = m2::Session::instance();
    r.current = pitchRangeIndex(session.project().ribbon);
    r.onChoose = [](int, const ListItem& it)
    {
        auto& s = m2::Session::instance();
        if (auto c = core::commands::ribbon::setPitchRange(s.project(), -100 * it.value, 100 * it.value))
            s.execute(std::move(c)); // null: the range is already ±N (nothing to change)
    };
    return std::make_unique<ListPopup>(std::move(r));
}
} // namespace

int pitchRangeIndex(const core::model::RibbonSettings& r) noexcept
{
    if (r.pitchTopCents != -r.pitchBottomCents || r.pitchTopCents % 100 != 0)
        return -1;
    for (std::size_t i = 0; i < kSemitones.size(); ++i)
        if (kSemitones[i] * 100 == r.pitchTopCents)
            return static_cast<int>(i);
    return -1;
}

int pitchRangeText(const core::model::RibbonSettings& r, char* out, std::size_t size) noexcept
{
    if (out == nullptr || size == 0)
        return 0;
    const int i = pitchRangeIndex(r);
    const int n = i >= 0 ? std::snprintf(out, size, "%.*s", static_cast<int>(kLabels[static_cast<std::size_t>(i)].size()),
                                         kLabels[static_cast<std::size_t>(i)].data())
                         : std::snprintf(out, size, "\xE2\x88\x92%d / +%d cents", -r.pitchBottomCents, r.pitchTopCents);
    return n < 0 ? 0 : (n < static_cast<int>(size) ? n : static_cast<int>(size) - 1);
}

int pitchRangeText(char* out, std::size_t size) noexcept
{
    return pitchRangeText(m2::Session::instance().project().ribbon, out, size);
}
} // namespace ui::m4

APP_REGISTER_POPUP((ui::PopupSpec{.kind = ui::PopupSpec::Kind::List,
                                  .id = ui::m4::kPitchRangePopup,
                                  .title = "TOUCH STRIP SETTING (PITCH RANGE)"}))
APP_REGISTER_POPUP_FACTORY(ui::m4::kPitchRangePopup, &ui::m4::makePitchRange)
// The UTILITY row itself is the settings list's (ui/screens/utility/Utility.cpp, "settings.pitchRange": its 13 values
// open a ListPopup there, UID-878); this popup stays registered for screens that open it by id.
