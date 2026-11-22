// INPUT trace of the imx6 control sources: one log line per logical event, in the app's log format
// ("[sp16-mod] <PREFIX> INPUT <source> <control> <kind> <value>", e.g. "INPUT panel play press 0"), so the emulator's
// console.txt shows that every control reaches the app. Printed on the UI thread from poll(), never on a reader or
// the audio thread. Setting input_trace=0 turns it off (on by default: a few lines per human action).
#pragma once

#include "io/controllers/ControlNames.h"

#include "ProjectIdentity.h"

#include <cstdio>
#include <string_view>

namespace platform::imx6
{
inline void traceInput(std::string_view source, const io::ControlEvent& e) noexcept
{
    char text[96];
    io::describe(e, text, sizeof text);
    std::printf("[sp16-mod] %s INPUT %.*s %s\n", identity::envPrefix, static_cast<int>(source.size()), source.data(),
                text);
    std::fflush(stdout);
}
} // namespace platform::imx6
