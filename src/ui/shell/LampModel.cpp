#include "ui/shell/LampModel.h"

#include "ui/shell/UiState.h"
#include "ui/theme/Theme.h"

namespace ui
{
void LampModel::update(const UiState& s, double nowMs, PadColourFn padColour, const void* context,
                       const LampSource* screen) noexcept
{
    frame_ = {};
    io::ModeLamps mode;
    mode.playing = s.playing;
    mode.recArmed = s.recArmed;
    io::modeLamps(mode, frame_);
    bool anySolo = false;
    for (const auto& tr : s.tracks)
        anySolo = anySolo || tr.soloed;
    for (int t = 1; t <= 16; ++t)
    {
        const auto& tr = s.tracks[static_cast<std::size_t>(t - 1)];
        io::PadLamp pad;
        pad.hasContent = tr.hasContent;
        const int colour = padColour != nullptr ? padColour(context, t) : t;
        pad.rgb = theme::ledColour(theme::trackColour(colour)) & 0x00FFFFFFu; // blended in LED colours (a flash)
        pad.lastHitMs = tr.lastHitMs > -1e8 ? tr.lastHitMs : -1e12; // never hit: steady colour, no flash
        pad.selected = t == s.selectedTrack;
        pad.muted = tr.muted;
        pad.soloed = tr.soloed;
        pad.anySolo = anySolo;
        frame_[io::pad(t)] = io::padLamp(io::PadMode::None, pad);
    }
    if (s.stepHeld > 0) // the held step key lights white until the sequencer's step lamps (UI-153)
        frame_[io::step(s.stepHeld)] = {io::LampMode::On, io::lampcolour::white, nowMs};
    if (screen != nullptr)
        screen->lamps(frame_, nowMs);
}
} // namespace ui
