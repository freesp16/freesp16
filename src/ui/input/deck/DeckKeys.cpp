#include "ui/input/deck/DeckKeys.h"

#include "core/util/musictime/MusicTime.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/shell/ParamAccess.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ui::input::deck
{
namespace
{
constexpr double kSameBpm = 0.05; // the engine's tempo grid is 0.1 BPM
constexpr std::string_view kBusFx = "mixer.busFx";
} // namespace

DeckKeys::DeckKeys(ParamAccess& values, fx::FxSlotAccess& slots, Hooks hooks, const core::ParamRegistry& params)
    : values_(values), fx_(slots), hooks_(std::move(hooks)), params_(params)
{
}

double DeckKeys::faderOffset(int raw) noexcept
{
    const int d = std::clamp(raw, 0, 1023) - kFaderCentre;
    if (std::abs(d) <= kFaderDeadZone)
        return 0.0;
    const double span = kFaderHalf - kFaderDeadZone;
    return std::clamp((d > 0 ? d - kFaderDeadZone : d + kFaderDeadZone) / span, -1.0, 1.0);
}

float DeckKeys::value(std::string_view id, float fallback) const
{
    const auto* spec = params_.find(id);
    return spec != nullptr ? values_.get(*spec, 0) : fallback;
}

void DeckKeys::set(std::string_view id, float v)
{
    if (const auto* spec = params_.find(id))
        values_.set(*spec, 0, spec->clamp(v));
}

bool DeckKeys::following() const
{
    return value("midi.syncSource", 0.0f) >= 0.5f && value("midi.in.clock", 1.0f) >= 0.5f;
}

double DeckKeys::offset() const noexcept
{
    return reset_ ? 0.0 : faderOffset(raw_);
}

double DeckKeys::target() const noexcept
{
    const double t = base_ * (1.0 + offset() * kRanges[range_] / 100.0);
    return core::musictime::clampBpm(std::round(t * 10.0) / 10.0);
}

double DeckKeys::liveTempo() const noexcept
{
    return offset() != 0.0 && ! following() ? target() : 0.0; // a followed clock's tempo is the engine's
}

void DeckKeys::apply(bool force)
{
    if (! started_ || following())
        return;
    const double t = target();
    if (! force && std::abs(t - posted_) < kSameBpm)
        return;
    if (hooks_.setTempo)
        hooks_.setTempo(t);
    posted_ = t;
    postedAt_ = feedBlock_;
}

void DeckKeys::frame(const engine::PlayheadFeed& feed)
{
    feedBlock_ = feed.blockIndex;
    if (feed.blockIndex == 0)
        return; // the engine is not running yet
    if (! started_)
    {
        started_ = true;
        base_ = posted_ = feed.bpm;
        if (offset() != 0.0 && ! following())
            apply(true); // a fader read before the engine ran (the panel sends it again only when it moves)
        return;
    }
    if (following())
    {
        // The clock sets the tempo. posted_ keeps the tempo the fader last set, for when the clock lets go.
        followed_ = true;
        handBackAt_ = feed.blockIndex;
        return;
    }
    if (followed_)
    {
        // The clock let go (SYNC, or SYNC SOURCE in UTILITY): once the feed shows the internal tempo again, that is
        // the fader's own last tempo (the base stays) or another one (the new base, the fader applies on top).
        if (feed.blockIndex < handBackAt_ + kSettleBlocks)
            return;
        followed_ = false;
        if (std::abs(feed.bpm - posted_) < kSameBpm)
            return;
        base_ = posted_ = feed.bpm;
        if (offset() != 0.0)
            apply(true);
        return;
    }
    if (offset() == 0.0)
    {
        base_ = posted_ = feed.bpm; // the fader leaves the tempo alone: whatever plays is the base
        return;
    }
    if (feed.blockIndex < postedAt_ + kSettleBlocks)
        return; // our last SetTempo may not be in this snapshot yet
    if (std::abs(feed.bpm - posted_) >= kSameBpm && value("transport.nudge", 0.0f) == 0.0f)
    {
        base_ = feed.bpm; // someone else set the tempo: it is the new base, the fader applies on top
        apply(true);
    }
}

bool DeckKeys::masterFxOn() const
{
    const int scene = hooks_.scene ? hooks_.scene() : 1;
    return ! fx::isNone(fx_.effect(fx::Slot::Master, scene, 1)) && ! fx_.bypass(fx::Slot::Master, scene, 1) &&
           fx::active(fx::Slot::Master, fx_.mode(scene));
}

bool DeckKeys::handle(const io::ControlEvent& e)
{
    using Kind = io::ControlEvent::Kind;
    const bool press = e.kind == Kind::Press, edge = press || e.kind == Kind::Release;
    const auto open = [this](std::string_view id)
    {
        if (hooks_.open)
            hooks_.open(id);
    };
    switch (e.control)
    {
    case io::Control::TempoFader:
        if (e.kind != Kind::Value)
            return false;
        raw_ = e.value;
        apply(false);
        return true;
    case io::Control::TempoRange:
        if (press)
        {
            range_ = (range_ + 1) % static_cast<int>(std::size(kRanges));
            apply(false);
        }
        return edge;
    case io::Control::TempoReset:
        if (press)
        {
            reset_ = ! reset_;
            apply(false);
        }
        return edge;
    case io::Control::Sync:
        if (press)
        {
            const bool follow = ! following();
            if (follow)
                set("midi.in.clock", 1.0f);
            set("midi.syncSource", follow ? 1.0f : 0.0f); // frame() hands the tempo back to the fader
        }
        return edge;
    case io::Control::Master:
        if (press)
            set("midi.out.clock", value("midi.out.clock", 1.0f) >= 0.5f ? 0.0f : 1.0f);
        return edge;
    case io::Control::FxOn:
        if (press)
        {
            const int scene = hooks_.scene ? hooks_.scene() : 1;
            if (fx::isNone(fx_.effect(fx::Slot::Master, scene, 1)))
                open(kBusFx); // nothing to turn on: choose an effect first
            else
                fx_.setBypass(fx::Slot::Master, scene, 1, ! fx_.bypass(fx::Slot::Master, scene, 1));
        }
        return edge;
    case io::Control::FxSelect:
        if (press)
            open(kBusFx);
        return edge;
    case io::Control::FxLevel:
    {
        if (e.kind != Kind::Value)
            return false;
        const int scene = hooks_.scene ? hooks_.scene() : 1;
        const auto effect = fx_.effect(fx::Slot::Master, scene, 1);
        if (! fx::isNone(effect))
            if (const auto* spec = params_.find("master." + std::string(effect) + ".mix"))
                values_.set(*spec, static_cast<std::uint16_t>((scene - 1) & 15),
                            spec->clamp(static_cast<float>(std::clamp<int>(e.value, 0, 1023)) / 1023.0f));
        return true;
    }
    case io::Control::LiveSampling:
        if (press && value("live.mode", 0.0f) < 0.5f)
        {
            open("track.menu"); // as SHIFT + REC: LIVE SAMPLING is reached from the TRACK MENU
            open("sampling.live");
        }
        return edge;
    case io::Control::PadModeHotSlice:
        return edge; // not assigned yet
    default:
        return false;
    }
}

void DeckKeys::lamps(io::LampFrame& f) const
{
    const auto lit = [](bool on)
    { return io::LampState{on ? io::LampMode::On : io::LampMode::Off, io::lampcolour::white, 0}; };
    f[io::Control::Sync] = lit(following());
    f[io::Control::Master] = lit(value("midi.out.clock", 1.0f) >= 0.5f);
    f[io::Control::TempoReset] = lit(reset_);
    f[io::Control::FxOn] = lit(masterFxOn());
    f[io::Control::LiveSampling] = lit(value("live.mode", 0.0f) >= 0.5f);
}
} // namespace ui::input::deck
