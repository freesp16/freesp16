#include "engine/ribbon/RibbonEngine.h"

#include <cmath>
#include <utility>

namespace engine::ribbon
{
namespace
{
constexpr double kEpsPpq = 1e-9; // the sequencer's window epsilon (engine/sequencer/Sequencer.cpp)

constexpr std::string_view kIds[RibbonEngine::kParams] = {"ribbon.mode",       "ribbon.hold",       "ribbon.pitchTop",
                                                          "ribbon.pitchBottom", "ribbon.userTarget", "ribbon.userTop",
                                                          "ribbon.userBottom", "ribbon.applyTo"};

// The sequencer's / scene switcher's frame of a PPQ t in the block (engine/scene/ScenePatternSwitcher.h frameAt).
int frameAt(double t, double p0, double ppqPerFrame) noexcept
{
    if (! (ppqPerFrame > 0.0))
        return 0;
    const double f = (t - p0) / ppqPerFrame + 1e-6;
    const int i = f < 0.0 ? 0 : static_cast<int>(f);
    return i > kBlockFrames - 1 ? kBlockFrames - 1 : i;
}

int roundInt(float v) noexcept { return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f); }
} // namespace

void RibbonEngine::bindParams(const std::function<core::ParamId(std::string_view)>& resolve) // rt-ok: set-up only
{
    if (! resolve)
        return;
    for (int i = 0; i < kParams; ++i)
        ids_[static_cast<std::size_t>(i)] = resolve(kIds[i]);
    pitchId_ = resolve("playback.pitch");
}

void RibbonEngine::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    repeatActive_ = false;
    phase_ = 0.0;
    // The router forgets its held pads in prepare (their releases never come): so does the ribbon.
    heldCount_.fill(0);
    held_ = sticky_ = targets_ = stoppedRepeats_ = 0;
    changed();
}

bool RibbonEngine::onParam(const ParamChange& c) noexcept
{
    if (! c.param.valid())
        return false;
    int which = -1;
    for (int i = 0; i < kParams; ++i)
        if (ids_[static_cast<std::size_t>(i)] == c.param)
            which = i;
    if (which < 0)
        return false;
    const int set = (c.scope >> 3) & 1, item = c.scope & 7;
    UserItem it = items_[set][item];
    switch (which)
    {
        case kMode: setMode(static_cast<Mode>(roundInt(c.value) & 3)); break;
        case kHold: setHold(c.value >= 0.5f); break;
        case kPitchTop: setPitchRange(bottomCents_, roundInt(c.value)); break;
        case kPitchBottom: setPitchRange(roundInt(c.value), topCents_); break;
        case kUserTarget:
            it.target = c.value < 0.0f || c.value >= 65535.0f ? core::ParamId {}
                                                               : core::ParamId {static_cast<std::uint16_t>(roundInt(c.value))};
            setItem(set, item, it);
            break;
        case kUserTop:
            it.top01 = c.value;
            setItem(set, item, it);
            break;
        case kUserBottom:
            it.bottom01 = c.value;
            setItem(set, item, it);
            break;
        case kApplyTo:
        {
            const int a = roundInt(c.value);
            setApplyTo(a <= 0 ? ApplyTo::HeldPads : a == 1 ? ApplyTo::SelectedTrack : ApplyTo::AllTracks);
            break;
        }
        default: break;
    }
    return true;
}

bool RibbonEngine::onLive(const TriggerNow& t, const trigger::PadState& pads, int heldTrack) noexcept
{
    if (t.pad == core::ribbon::kStripPad)
    {
        onStrip(t.velocity, t.note != 0);
        return true;
    }
    if (t.pad == core::ribbon::kPressurePad)
    {
        // The pad's track as the router routes a press (TriggerRouter::pushLive): PLAY = the pad, SLICE / SCALE = the
        // selected track, TRACK / MUTE = none.
        const int pad = t.track & 15;
        switch (pads.mode)
        {
        case trigger::PadMode::Play:
            onPressure(heldTrack >= 0 ? heldTrack : pad, t.velocity);
            break;
        case trigger::PadMode::Slice:
        case trigger::PadMode::Scale:
            onPressure(heldTrack >= 0 ? heldTrack : pads.selected & 15, t.velocity);
            break;
        default:
            break;
        }
        return true;
    }
    return false;
}

void RibbonEngine::onStrip(int position, bool contact) noexcept
{
    const int p = core::ribbon::clampPosition(position);
    if (! contact)
    {
        if (contact_ && hold_) // HOLD: the last touched value stays (RIB-005)
        {
            latched_ = true;
            latchedPosition_ = position_;
        }
        contact_ = false;
        changed();
        return;
    }
    if (! contact_)
        stoppedRepeats_ = 0;
    if (! contact_ || p != position_)
        changed();
    contact_ = true;
    position_ = p;
}

void RibbonEngine::onPressure(int track, int pressure) noexcept
{
    if (track < 0 || track >= kTracks)
        return;
    pressure_[static_cast<std::size_t>(track)] =
        static_cast<std::int16_t>(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure));
}

void RibbonEngine::onPitchBend(int track, int value14) noexcept
{
    if (track < 0 || track >= kTracks || mode_ != Mode::Pitch) // RIB-007: PITCH mode only
        return;
    const auto bit = static_cast<std::uint16_t>(1u << track);
    const float cents = core::ribbon::bendCents(value14, bottomCents_, topCents_);
    bend_[static_cast<std::size_t>(track)] = modulation::pitchOffset01(cents);
    bendMask_ = static_cast<std::uint16_t>(cents != 0.0f ? (bendMask_ | bit) : (bendMask_ & ~bit));
    bendDirty_ = true;
}

void RibbonEngine::releaseAll() noexcept
{
    latched_ = false;
    sticky_ = 0;
    changed();
}

void RibbonEngine::setMode(Mode m) noexcept
{
    if (m == mode_)
        return;
    mode_ = m;
    hold_ = false; // RIB-005: changing MODE cancels HOLD
    releaseAll();
    repeatActive_ = false;
    if (m != Mode::Pitch && bendMask_ != 0) // incoming pitch bend acts in PITCH mode only
    {
        bendMask_ = 0;
        bendDirty_ = true;
    }
}

void RibbonEngine::setHold(bool on) noexcept
{
    if (on == hold_)
        return;
    hold_ = on;
    if (! on)
        releaseAll(); // `releaseParameters`: values return to base
}

void RibbonEngine::setApplyTo(ApplyTo a) noexcept
{
    if (a == applyTo_)
        return;
    applyTo_ = a;
    changed();
}

void RibbonEngine::setPitchRange(int bottomCents, int topCents) noexcept
{
    bottomCents_ = bottomCents < -2400 ? -2400 : (bottomCents > 0 ? 0 : bottomCents);
    topCents_ = topCents < 0 ? 0 : (topCents > 2400 ? 2400 : topCents);
    changed();
}

void RibbonEngine::setItem(int set, int item, const UserItem& it) noexcept
{
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    UserItem& dst = items_[set & 1][item & 7];
    dst.target = it.target;
    dst.bottom01 = clamp01(it.bottom01);
    dst.top01 = clamp01(it.top01);
    changed();
}

int RibbonEngine::effectivePosition() const noexcept
{
    return contact_ ? position_ : (hold_ && latched_ ? latchedPosition_ : -1);
}

std::uint8_t RibbonEngine::ledMask() const noexcept
{
    const int p = effectivePosition();
    return core::ribbon::ledMask(mode_, p < 0 ? 0 : p, p >= 0);
}

double RibbonEngine::repeatIntervalFrames(double bpm) const noexcept
{
    const int p = effectivePosition();
    if (mode_ != Mode::Repeat || p < 0)
        return 0.0;
    const double b = bpm > 0.0 ? bpm : 120.0;
    return core::ribbon::repeatQuarters(core::ribbon::repeatZone(p)) * 60.0 / b * sampleRate_;
}

float RibbonEngine::pitchCents() const noexcept
{
    const int p = effectivePosition();
    return p < 0 ? 0.0f : core::ribbon::pitchCents(p, bottomCents_, topCents_);
}

void RibbonEngine::stopRepeats() noexcept
{
    if (mode_ == Mode::Repeat && effectivePosition() >= 0)
    {
        stoppedRepeats_ = held_;
        repeatActive_ = false;
    }
}

void RibbonEngine::swapTracks(int a, int b) noexcept
{
    const auto swapBits = [a, b](std::uint16_t& mask)
    {
        const auto difference = ((mask >> a) ^ (mask >> b)) & 1u;
        mask ^= static_cast<std::uint16_t>((difference << a) | (difference << b));
    };
    std::swap(heldCount_[a], heldCount_[b]);
    std::swap(velocity_[a], velocity_[b]);
    std::swap(semitones_[a], semitones_[b]);
    std::swap(pressure_[a], pressure_[b]);
    std::swap(sincePress_[a], sincePress_[b]);
    swapBits(held_);
    swapBits(sticky_);
    swapBits(targets_);
    swapBits(stoppedRepeats_);

    // The ModMatrix rows move with their producers' cached contributions.
    std::swap(pub_[a], pub_[b]);
    std::swap(pubCount_[a], pubCount_[b]);
    std::swap(bend_[a], bend_[b]);
    std::swap(bendPub_[a], bendPub_[b]);
    swapBits(pubMask_);
    swapBits(retry_);
    swapBits(bendMask_);
    swapBits(bendPubMask_);
}

int RibbonEngine::collectHits(const BlockContext& ctx, std::span<trigger::TriggerEvent, kMaxHits> out) noexcept
{
    const int p = effectivePosition();
    const auto repeatTracks = static_cast<std::uint16_t>(held_ & ~stoppedRepeats_);
    const bool active = mode_ == Mode::Repeat && p >= 0 && repeatTracks != 0;
    for (auto& s : sincePress_)
        s += kBlockFrames;
    if (! active)
    {
        repeatActive_ = false;
        return 0;
    }
    if (! repeatActive_)
    {
        repeatActive_ = true;
        phase_ = 0.0; // free run: the first repeat one interval after REPEAT became active
    }
    const double bpm = ctx.bpm > 0.0 ? ctx.bpm : 120.0;
    const double quarters = core::ribbon::repeatQuarters(core::ribbon::repeatZone(p));
    const double interval = quarters * 60.0 / bpm * sampleRate_;

    int hitFrame = -1;
    double hitAt = 0.0; // the exact hit time in frames from the block start (the phase keeps the fraction)
    if (ctx.playing && ctx.ppqEnd > ctx.ppqStart) // on the transport grid, like the sequencer's steps
    {
        const double p0 = ctx.ppqStart, p1 = ctx.ppqEnd;
        const double ppf = (p1 - p0) / (ctx.frames > 0 ? ctx.frames : kBlockFrames);
        const double at = std::ceil((p0 - kEpsPpq) / quarters) * quarters;
        if (at < p1 - kEpsPpq)
        {
            hitFrame = frameAt(at, p0, ppf);
            hitAt = (at - p0) / ppf;
        }
    }
    else // stopped: free-running from the activation, rate changes from the last hit
    {
        const double t = interval - phase_;
        hitAt = t < 0.0 ? 0.0 : t;
        if (hitAt < static_cast<double>(kBlockFrames))
            hitFrame = static_cast<int>(hitAt);
    }
    phase_ = hitFrame >= 0 ? static_cast<double>(kBlockFrames) - hitAt : phase_ + kBlockFrames;
    if (hitFrame < 0)
        return 0;

    int n = 0;
    for (int t = 0; t < kTracks; ++t)
    {
        if (! (repeatTracks >> t & 1u))
            continue;
        const auto ti = static_cast<std::size_t>(t);
        if (sincePress_[ti] - kBlockFrames + hitFrame < 0.5 * interval) // the pad's own hit is that note (no flam)
            continue;
        trigger::TriggerEvent& e = out[static_cast<std::size_t>(n++)];
        e.track = static_cast<std::uint8_t>(t);
        const int pr = pressure_[ti];
        e.velocity = static_cast<std::uint8_t>(pr >= 0 ? (pr > 0 ? pr : 1) : (velocity_[ti] > 0 ? velocity_[ti] : 1));
        e.semitones = semitones_[ti]; // the held pad's scale note (0 in PLAY / SLICE)
        e.kind = trigger::TriggerKind::Full;
        e.source = trigger::TriggerSource::Repeat;
        e.frame = static_cast<std::uint8_t>(hitFrame);
        e.ducks = false; // the router sets it
        e.locks = nullptr;
    }
    return n;
}

void RibbonEngine::observe(std::span<const trigger::TriggerEvent> events, const trigger::PadState& pads) noexcept
{
    for (const auto& e : events)
    {
        if (e.source != trigger::TriggerSource::Pad || e.track >= kTracks)
            continue;
        const auto t = static_cast<std::size_t>(e.track);
        if (e.kind == trigger::TriggerKind::Full)
        {
            stoppedRepeats_ = static_cast<std::uint16_t>(stoppedRepeats_ & ~(1u << t));
            if (heldCount_[t] < 255)
                ++heldCount_[t];
            velocity_[t] = e.velocity;
            semitones_[t] = e.semitones;
            pressure_[t] = -1;
            sincePress_[t] = static_cast<double>(kBlockFrames - e.frame); // at the end of this block
        }
        else if (e.kind == trigger::TriggerKind::Release && heldCount_[t] > 0)
            --heldCount_[t];
    }
    std::uint16_t held = 0;
    for (int t = 0; t < kTracks; ++t)
        if (heldCount_[static_cast<std::size_t>(t)] > 0)
            held = static_cast<std::uint16_t>(held | (1u << t));
    held_ = held;
    selected_ = static_cast<std::uint8_t>(pads.selected & 15);

    std::uint16_t targets = 0;
    if (effectivePosition() >= 0 && mode_ != Mode::Repeat)
    {
        switch (applyTo_)
        {
            case ApplyTo::HeldPads:
                if (hold_)
                    sticky_ = static_cast<std::uint16_t>(sticky_ | held_); // they keep the value under HOLD
                targets = static_cast<std::uint16_t>(held_ | (hold_ ? sticky_ : 0));
                break;
            case ApplyTo::SelectedTrack: targets = static_cast<std::uint16_t>(1u << selected_); break;
            case ApplyTo::AllTracks: targets = 0xFFFF; break;
        }
    }
    if (targets != targets_)
        changed();
    targets_ = targets;
}

int RibbonEngine::wanted(std::array<Out, kMaxOut>& out) const noexcept
{
    const int p = effectivePosition();
    if (p < 0)
        return 0;
    if (mode_ == Mode::Pitch)
    {
        if (! pitchId_.valid())
            return 0;
        out[0] = {pitchId_, modulation::pitchOffset01(core::ribbon::pitchCents(p, bottomCents_, topCents_)), false};
        return 1;
    }
    if (mode_ != Mode::User1 && mode_ != Mode::User2)
        return 0;
    const auto& set = items_[mode_ == Mode::User1 ? 0 : 1];
    int n = 0;
    for (const auto& it : set)
    {
        if (! it.target.valid())
            continue;
        bool dup = false; // two items on one target: the first wins
        for (int j = 0; j < n && ! dup; ++j)
            dup = out[static_cast<std::size_t>(j)].dest == it.target;
        if (! dup)
            out[static_cast<std::size_t>(n++)] = {it.target, core::ribbon::userValue(p, it.bottom01, it.top01), true};
    }
    return n;
}
} // namespace engine::ribbon
