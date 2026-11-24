// The LampModel of UI-150 (03-ui/control-surface.md Part 3; UID-175 "lamps and the screen agree"): one place
// that computes every lamp's state from the UI state the screen shows, once per frame on the message thread, so the
// panel, the virtual panel and any controller light exactly what the screen says. The Shell owns it and exposes the
// frame (Shell::lamps(), Root::lamps()); the composition root resolves it into the platform's LampSink with
//   io::apply(*root->lamps(), nowMs, brightness, *lampSink);
//
// Rules applied here (io/controllers/LampRules.h): transport and mode keys (UI-156: PLAY green while running, REC red
// while armed), the pads (UI-151, io::padLamp's ladder: dark when empty, faint when silent, dim, the selected track
// full, a soloed one breathing, a hit flashes white and fades back — the app's PadController paints over them with
// the same rule and the sequencer's hits), the held step key (white while held, our choice: the step-lamp sequence view
// of UI-153 comes with the sequencer). A screen whose body also derives from LampSource refines the frame after these
// rules (SCENE MANAGER's pattern keys, the sequencer's step lamps, the BAR keys), so the lamps follow the screen that
// is shown. Allocation-free.
#pragma once

#include "io/controllers/LampRules.h"

namespace ui
{
struct UiState;

class LampSource
{
public:
    virtual ~LampSource() = default;
    virtual void lamps(io::LampFrame& frame, double nowMs) const = 0;
};

class LampModel
{
public:
    // padColour(t) = palette index 1–16 of track t (the shell reads track.padColor); may be null (= t).
    using PadColourFn = int (*)(const void* context, int track1Based);
    void update(const UiState&, double nowMs, PadColourFn padColour, const void* context,
                const LampSource* screen) noexcept;
    const io::LampFrame& frame() const noexcept { return frame_; }

private:
    io::LampFrame frame_{};
};
} // namespace ui
