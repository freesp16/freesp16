// Durations the spec states in prose but tokens.json does not carry (so they cannot come from the generated theme):
// each cites its requirement. Everything animated otherwise uses the motion tokens (02-tokens.md "Motion").
#pragma once

namespace ui::timing
{
inline constexpr double recentlyTouchedMs = 600;   // UID-160: accent underline after the last change
inline constexpr double toastMs = 2000;            // UID-908: toasts hide after 2 s
inline constexpr double progressDelayMs = 300;     // UID-907 / "Progress rule": nothing for the first 300 ms
inline constexpr double midiActivityMs = 100;      // frame.md StatusIndicators: accent for 100 ms per message
inline constexpr double caretBlinkPeriodMs = 1000; // controls.md TextField: caret blinking 1 Hz
inline constexpr double doubleTapMs = 400;         // UID-912 / UI-123: second tap on a column within 400 ms
} // namespace ui::timing
