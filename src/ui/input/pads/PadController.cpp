#include "ui/input/pads/PadController.h"

#include "core/commands/CommandBusAccess.h"
#include "core/commands/track/TrackCommands.h"
#include "core/model/Project.h"
#include "core/util/scales/Scales.h"
#include "io/controllers/LampRules.h"
#include "ui/theme/Theme.h"

#include <algorithm>

namespace ui::input::pads
{
namespace
{
namespace cmd = core::commands;

io::PadMode lampMode(PadMode m) noexcept
{
    switch (m)
    {
        case PadMode::Play: return io::PadMode::None;
        case PadMode::Track: return io::PadMode::Track;
        case PadMode::Mute: return io::PadMode::Mute;
        case PadMode::Slice: return io::PadMode::Slice;
        case PadMode::Scale: return io::PadMode::Scale;
    }
    return io::PadMode::None;
}

constexpr io::Control kModeKeys[] = {io::Control::PadModeTrack, io::Control::PadModeMute, io::Control::PadModeSlice,
                                     io::Control::PadModeScale};
constexpr double kLampFrameMs = 1000.0 / 60.0; // lamp frames limited to 60 Hz
} // namespace

PadController::PadController(core::CommandBus& bus, engine::EngineBridge& bridge, Selection& selection,
                             const core::ParamRegistry& params)
    : bus_(bus), bridge_(bridge), selection_(selection), params_(params)
{
    lastHit_.fill(-1e12);
    lastPadHit_.fill(-1e12);
}

int PadController::scene() const noexcept
{
    return cmd::hasProject(bus_) ? cmd::project(bus_).settings.currentScene & 15 : 0;
}

void PadController::post(std::string_view id, float value)
{
    if (const auto pid = params_.idOf(id); pid && pid->valid())
        bridge_.post(engine::ParamChange {*pid, 0xFFFF, value, 0});
}

void PadController::select(int track)
{
    selection_.select(track & 15);
    sync();
}

void PadController::sync()
{
    const int s = selection_.selected() & 15;
    if (s == postedSelection_)
        return;
    postedSelection_ = s;
    post("pads.selected", static_cast<float>(s));
}

void PadController::onModeKey(PadMode m)
{
    mode_ = (m == mode_ || m == PadMode::Play) ? PadMode::Play : m; // UI-104: the lit key turns its mode off
    if (muteDown_ && m != PadMode::Mute)
        modeBeforeMute_ = mode_; // another mode key while MUTE is held: the release keeps it
    postMode();
}

void PadController::tick(double nowMs)
{
    if (heldMode_ == PadMode::Play)
        return;
    if (heldSinceMs_ < 0)
        heldSinceMs_ = nowMs; // timed on the UI clock from the first frame after the press
    else if (nowMs - heldSinceMs_ >= holdMs_)
    {
        const auto m = heldMode_;
        heldMode_ = PadMode::Play;
        onModeKey(m);
    }
}

void PadController::onShift(bool held)
{
    if (held == shift_)
        return;
    shift_ = held;
    post("pads.shift", held ? 1.0f : 0.0f);
}

void PadController::onPad(int pad, int rawVelocity, bool shift, bool pressed, double nowMs, bool sentDirect)
{
    if (pad < 0 || pad > 15)
        return;
    sync();
    if (! pressed)
    {
        // Releases always come from here (no source pushes them): the engine sends it to the track the press played.
        bridge_.post(engine::TriggerNow {static_cast<std::uint8_t>(pad), 0, 60, static_cast<std::int8_t>(pad)});
        return;
    }
    onShift(shift);
    bool sounds = false;
    int track = pad;
    if (muteDown_) // MUTE held: solo (SHIFT: mute the others), never a sound
    {
        soloUsed_ = true;
        heldMode_ = PadMode::Play; // OVER 1sec: the held key no longer switches the mode
        if (cmd::hasProject(bus_))
        {
            const auto& p = cmd::project(bus_);
            if (shift)
            {
                if (auto group = cmd::track::muteOthers(p, scene(), pad))
                    bus_.execute(std::move(group));
            }
            else
                bus_.execute(cmd::track::toggleSolo(p, scene(), pad));
        }
        return;
    }
    if (shift && mode_ != PadMode::Mute)
        select(pad); // PAD-004: SHIFT + pad selects in every mode but MUTE
    else
        switch (mode_)
        {
            case PadMode::Play:
                select(pad); // pad n plays and selects track n
                sounds = true;
                break;
            case PadMode::Track: select(pad); break; // no sound (PAD-004; our choice)
            case PadMode::Mute:
                if (cmd::hasProject(bus_))
                {
                    const auto& p = cmd::project(bus_);
                    if (shift)
                    {
                        if (auto group = cmd::track::muteOthers(p, scene(), pad))
                            bus_.execute(std::move(group));
                    }
                    else
                        bus_.execute(cmd::track::toggleMute(p, scene(), pad));
                }
                break;
            case PadMode::Slice:
            case PadMode::Scale:
                track = selection_.selected() & 15;
                sounds = true;
                break;
        }
    if (! sounds)
        return;
    lastHit_[static_cast<std::size_t>(track)] = nowMs;
    lastPadHit_[static_cast<std::size_t>(pad)] = nowMs;
    if (! sentDirect)
    {
        const int v = applyVelocityCurve(rawVelocity, curve_, fixed_);
        if (v > 0)
            bridge_.post(engine::TriggerNow {static_cast<std::uint8_t>(pad), static_cast<std::uint8_t>(v), 60,
                                             static_cast<std::int8_t>(pad)});
    }
}

bool PadController::handle(const io::ControlEvent& e)
{
    using Kind = io::ControlEvent::Kind;
    if (e.control == io::Control::Shift && (e.kind == Kind::Press || e.kind == Kind::Release))
    {
        onShift(e.kind == Kind::Press);
        return false;
    }
    for (int i = 0; i < 4; ++i)
        if (e.control == kModeKeys[i] && (e.kind == Kind::Press || e.kind == Kind::Release))
        {
            const auto m = static_cast<PadMode>(i + 1);
            if (m == PadMode::Mute)
            {
                onMuteKey(e.kind == Kind::Press, e.timeMs);
                return e.kind == Kind::Press || holdMs_ > 0;
            }
            if (e.kind == Kind::Release)
            {
                if (heldMode_ == m)
                    heldMode_ = PadMode::Play; // let go before the hold time: no change (PAD-007; our choice)
                return holdMs_ > 0;
            }
            if (holdMs_ > 0)
            {
                heldMode_ = m;
                heldSinceMs_ = -1;
            }
            else
                onModeKey(m);
            return true;
        }
    if (io::isPad(e.control) && (e.kind == Kind::Press || e.kind == Kind::Release))
    {
        const bool direct = e.source == io::kSourcePanel || e.source == io::kSourceKeyboard || e.source >= io::kSourceMidi;
        onPad(io::indexOf(e.control) - 1, e.value, shift_, e.kind == Kind::Press, e.timeMs, direct);
        return true;
    }
    return false;
}

void PadController::onMuteKey(bool pressed, double nowMs)
{
    if (pressed)
    {
        if (muteDown_)
            return; // a repeated press without its release
        muteDown_ = true;
        soloUsed_ = false;
        muteDownMs_ = nowMs;
        modeBeforeMute_ = mode_;
        if (holdMs_ > 0)
        {
            heldMode_ = PadMode::Mute;
            heldSinceMs_ = -1;
            postMode();
        }
        else
            onModeKey(PadMode::Mute);
        return;
    }
    if (! muteDown_)
        return;
    muteDown_ = false;
    if (heldMode_ == PadMode::Mute)
        heldMode_ = PadMode::Play; // let go before the hold time: no change (PAD-007; our choice)
    if (soloUsed_)
        mode_ = modeBeforeMute_; // a solo gesture, not a mode change
    postMode();
}

void PadController::observeHits(const engine::PlayheadFeed& ph, double nowMs) noexcept
{
    for (std::size_t t = 0; t < 16; ++t)
        if (hitsSeen_ && ph.trackHits[t] != seenHits_[t])
            lastHit_[t] = std::max(lastHit_[t], nowMs);
    seenHits_ = ph.trackHits;
    hitsSeen_ = true;
}

void PadController::paintLamps(io::LampSink& sink, double nowMs, const engine::MeterFeed&) const
{
    if (nowMs - lastPaintMs_ < kLampFrameMs)
        return;
    lastPaintMs_ = nowMs;
    const bool model = cmd::hasProject(bus_);
    const int s = scene();
    const int selected = selection_.selected() & 15;
    const auto m = muteDown_ ? io::PadMode::Solo : lampMode(mode_);
    // SLICE / SCALE: every pad shows the selected track (MUTE held shows the tracks again).
    const bool onSelected = ! muteDown_ && (mode_ == PadMode::Slice || mode_ == PadMode::Scale);
    bool anySolo = false;
    if (model)
        for (const auto& tr : cmd::project(bus_).scenes[static_cast<std::size_t>(s)].tracks)
            anySolo = anySolo || tr.solo;
    for (int pad = 0; pad < 16; ++pad)
    {
        const int t = onSelected ? selected : pad;
        io::PadLamp lamp;
        // The LED colour (ui::theme::ledColour) before the flash blends it, so a hit fades white → the LED's own
        // hue, not → the screen palette's (which only maps to the LED colour when it is exact).
        lamp.rgb = theme::ledColour(theme::trackColour(t + 1)) & 0xFFFFFFu;
        lamp.hasContent = true;
        if (model)
        {
            const auto& tr = cmd::project(bus_).scenes[static_cast<std::size_t>(s)].tracks[static_cast<std::size_t>(t)];
            lamp.rgb = theme::ledColour(theme::trackColour((tr.colour & 15) + 1)) & 0xFFFFFFu;
            lamp.muted = cmd::track::isMuted(cmd::project(bus_), s, t);
            lamp.soloed = tr.solo;
            lamp.anySolo = anySolo;
            lamp.hasContent = tr.mode != core::model::TrackMode::Sample || ! tr.playback.sample.empty();
            if (mode_ == PadMode::Scale)
                lamp.root = core::scales::isRootPad(pad, core::scales::scaleMask(tr.padScale.scaleType),
                                                    tr.padScale.windowOffset);
        }
        if (hasContent)
            lamp.hasContent = hasContent(t);
        lamp.lastHitMs = onSelected ? lastPadHit_[static_cast<std::size_t>(pad)] : lastHit_[static_cast<std::size_t>(t)];
        lamp.selected = t == selected;
        if (mode_ == PadMode::Slice) // UI-152: the pads holding a slice of the selected track
            lamp.inSlices = (slicePad ? slicePad(selected, pad) : pad) >= 0;
        const auto out = io::resolve(io::padLamp(m, lamp), nowMs);
        sink.setLamp(io::pad(pad + 1), out.rgb, out.level);
    }
    for (int i = 0; i < 4; ++i) // UI-156: mode keys white while on
    {
        const bool on = static_cast<int>(mode_) == i + 1;
        if (muteDown_ && kModeKeys[i] == io::Control::PadModeMute) // MUTE held: solo, the key blinks fast
        {
            const auto out = io::resolve({io::LampMode::BlinkFast, io::lampcolour::white, muteDownMs_}, nowMs);
            sink.setLamp(kModeKeys[i], out.rgb, out.level);
            continue;
        }
        sink.setLamp(kModeKeys[i], io::lampcolour::white, on ? 255 : 0);
    }
    sink.flush();
}

void forwardTrackChanges(const core::ChangeSet& changes, const core::model::Project& p, engine::EngineBridge& bridge,
                         const core::ParamRegistry& params)
{
    cmd::track::forEachTrackChange(changes, p, [&](int scene, int track, std::string_view id, float value) {
        if (const auto pid = params.idOf(id); pid && pid->valid())
            bridge.post(engine::ParamChange {*pid, static_cast<std::uint16_t>((scene << 4) | track), value, 0});
    });
}
} // namespace ui::input::pads
