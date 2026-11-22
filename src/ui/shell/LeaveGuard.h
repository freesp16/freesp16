// A body that must be asked before its screen is left (sampling.md "Entry": leaving SAMPLING/EDIT with an
// unsaved take asks `Discard the recording?`, UID-784). The shell finds it once per screen entry (dynamic_cast on the
// body, like RailOwner) and asks it before BACK, HOME or an open() that leaves the screen (never while a popup is
// open: BACK then closes the popup). Returning false keeps the screen; the body typically shows a Message and leaves
// itself (ShellApi::back / home / open) once the user decided — the guard must let that second call through.
#pragma once

#include <string_view>

namespace ui
{
class LeaveGuard
{
public:
    virtual ~LeaveGuard() = default;
    // target: "" = BACK, "home" = HOME, else the screen id open() is about to show.
    virtual bool mayLeave(std::string_view target) = 0;
};
} // namespace ui
