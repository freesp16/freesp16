// The touch strip's live state for TOUCH STRIP SETTING's StripPreview (UID-884): written once per UI frame by
// the composition root from io::StripRouter (mode, contact, position — app/Document.cpp), read by the screen. Process-
// wide like the other screen hooks (until UiState carries it). Message thread; no allocation.
#pragma once

namespace ui::m4
{
struct StripLive
{
    int mode = 0;          // core::ribbon::Mode: 0 PITCH, 1 REPEAT, 2 USER1, 3 USER2
    bool contact = false;  // a finger on the strip
    int position = 0;      // 0 (bottom) … 127 (top)
};
inline StripLive& stripLive() noexcept
{
    static StripLive live;
    return live;
}
} // namespace ui::m4
