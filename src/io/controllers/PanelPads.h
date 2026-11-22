// UTILITY's pad preferences for the reference hardware's own pads (02-features/settings.md SET-015, pads-and-scales.md
// PAD-002, 07-ui-design/screens/utility.md MACHINE SETTING): PAD CURVE (`pads.velocityCurve`, FIXED with
// `pads.fixedVelocity`), PAD THRESHOLD (`pads.threshold`) and PAD SENSITIVITY (`pads.sensitivity`), 1–16 with
// 5 = "(Default)". The panel decodes velocity in its reader thread (UI-118), so the message thread hands the values
// over here, lock-free, and the reader picks them up between two frames.
//
// Our mapping onto the decoder (io::PadCurve; the reference's own laws are unknown, no velocity readout):
//   curve        index 0…6 = Hard 3 … Linear … Soft 3, 7 = FIXED (the pads.velocityCurve enum)
//   threshold    5 = the calibration's hit threshold; each step ±8 pressure words (1 = 32 words lower, 16 = 88 higher)
//   sensitivity  5 = the calibration (velocity 127 at the ceiling word); the gain on the word above the floor is
//                2^((s − 5) / 4) below 5 and 2^((s − 5) / 11) above (1 = half, 16 = double), done by moving the ceiling
// The MIDI pad controllers keep their own per-port response (CONTROLLERS page, ControllerHub); these are the panel's.
//
// Threads: publishPanelPads on the message thread; takePanelPads on the panel's reader thread (one relaxed load per
// frame while nothing changed). A read that overlaps a publish is repeated at the next frame (the generation moved).
#pragma once

#include "io/controllers/PadCurve.h"

#include <cstdint>

namespace io
{
struct PanelPadPrefs
{
    int curve = 3;           // pads.velocityCurve: 0 Hard 3 … 3 Linear … 6 Soft 3, 7 FIXED
    int fixedVelocity = 100; // pads.fixedVelocity 1…127
    int threshold = 5;       // pads.threshold 1…16
    int sensitivity = 5;     // pads.sensitivity 1…16
    friend bool operator== (const PanelPadPrefs&, const PanelPadPrefs&) = default;
};

// The decoder's calibration and response for `prefs` over `base` (the panel's calibration).
void applyPanelPadPrefs(const PanelPadPrefs& prefs, const PadCalibration& base, PadCalibration& calibration,
                        PadResponse& response) noexcept;

void publishPanelPads(const PanelPadPrefs&) noexcept; // message thread
// True (and `out` filled, `seen` updated) when a publish happened since `seen`. Start with seen = 0.
bool takePanelPads(std::uint32_t& seen, PanelPadPrefs& out) noexcept;
} // namespace io
