// HOME (07-ui-design/screens/home.md; 03-ui/screens.md UI-210…217; UID-500…504): template content+meter, context bar
// menu, the TrackTile grid and the MASTER meter, knob strip VOLUME, PAN, PITCH, TIME STRETCH, LOOP, TRIGGER MODE of the
// selected track (THRU: columns 3–5 empty; MIDI / AS-1: 3–6 empty, 1–2 only while CC1 volume/pan exist).
// Replaces the shell's interim HOME (ui/shell/Fallbacks.cpp) by registering the id.
#include "ui/screens/home/TrackGrid.h"

#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/sequence/M2Commands.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/widgets/Meter.h"

#include <algorithm>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;

constexpr StripSpec kHomeStrip{
    "TRACK {track}",
    {"track.volume", "track.pan", "playback.pitch", "playback.timeStretch", "playback.loop", "track.triggerMode"}};
constexpr StripSpec kHomeThruStrip{"TRACK {track}", {"track.volume", "track.pan", "", "", "", "track.triggerMode"}};
constexpr StripSpec kHomeMidiStrip{"TRACK {track}", {}};
} // namespace

TrackGrid::TrackGrid(const WidgetArgs& a, TrackTileModel::Variant v) noexcept
    : shell_(a.shell), state_(a.state), variant_(v)
{
}

RectF TrackGrid::bounds() const noexcept
{
    return {16, L::contentTop, 1144, L::contentH};
}

TrackTileModel TrackGrid::model(int t, const UiContext& ctx) const noexcept
{
    TrackTileModel m;
    m.track = t;
    m.variant = variant_;
    if (state_ == nullptr)
        return m;
    const auto& s = state_->tracks[static_cast<std::size_t>(t - 1)];
    const auto& feeds = m1::feedsOf(shell_);
    m.summary = &s;
    m.selected = t == state_->selectedTrack;
    m.focused = m.selected && ctx.focus.focused < 0;
    const double since = ctx.nowMs - std::max(s.lastHitMs, feeds.lastHitMs(t));
    if (since >= 0 && since < tk::motionPadFlashMs)
        m.flash = static_cast<float>(1.0 - since / tk::motionPadFlashMs);
    m.levelDb = feeds.live() ? std::max(feeds.track(t).db[0], feeds.track(t).db[1]) : s.levelDb;
    m.loading = m1::loadProgress()[t - 1];
    if (shell_ != nullptr)
    {
        m.colour = m1::padColour(*shell_, t);
        if (variant_ == TrackTileModel::Variant::Setting)
        {
            auto& vel = vel_[static_cast<std::size_t>(t - 1)];
            auto& choke = choke_[static_cast<std::size_t>(t - 1)];
            const auto v = m1::trackLabel(*shell_, "track.padVelocity", t);
            const auto c = m1::trackLabel(*shell_, "track.chokeGroup", t);
            std::snprintf(vel, sizeof vel, "VEL %.*s", static_cast<int>(v.size()), v.data());
            std::snprintf(choke, sizeof choke, "CHOKE %.*s", static_cast<int>(c.size()), c.empty() ? "--" : c.data());
            m.velBadge = vel;
            m.chokeBadge = choke;
            static constexpr std::string_view numbers[] = {"#01", "#02", "#03", "#04", "#05", "#06", "#07", "#08",
                                                           "#09", "#10", "#11", "#12", "#13", "#14", "#15", "#16"};
            m.colourBadge = numbers[(m.colour - 1) & 15];
        }
    }
    return m;
}

void TrackGrid::bindStripForMode() const
{
    if (shell_ == nullptr || state_ == nullptr || variant_ != TrackTileModel::Variant::Home)
        return;
    const int mode = m1::trackMode(*shell_, state_->selectedTrack);
    if (mode == boundMode_)
        return;
    boundMode_ = mode;
    shell_->bindStrip(mode == 2 ? &kHomeThruStrip : (mode == 0 ? nullptr : &kHomeMidiStrip));
}

void TrackGrid::build(DrawList& dl, const UiContext& ctx) const
{
    bindStripForMode();
    for (int t = 1; t <= 16; ++t)
        TrackTile::draw(dl, ctx, L::trackTile(t), model(t, ctx));
}

bool TrackGrid::dirty(double nowMs) const
{
    if (state_ == nullptr)
        return false;
    const auto& feeds = m1::feedsOf(shell_);
    for (int t = 1; t <= 16; ++t)
    {
        if (nowMs - std::max(state_->tracks[static_cast<std::size_t>(t - 1)].lastHitMs, feeds.lastHitMs(t)) <
            tk::motionPadFlashMs + 20)
            return true;
        if (feeds.track(t).db[0] > -120.0f || feeds.track(t).db[1] > -120.0f || m1::loadProgress()[t - 1] >= 0)
            return true;
    }
    return false;
}

void TrackGrid::swapSelection(int picked)
{
    const int sceneIndex = state_->scene - 1;
    shell_->selectTrack(picked);
    if (sourceTrack_ == 0 || sourceScene_ != sceneIndex)
    {
        sourceTrack_ = picked;
        sourceScene_ = sceneIndex;
        ToastRequest notice;
        notice.text = "Tap the destination track";
        shell_->showToast(std::move(notice));
        return;
    }

    const int source = sourceTrack_ - 1;
    const int destination = picked - 1;
    sourceTrack_ = 0;
    if (source == destination)
        return;
    if (state_->tracks[static_cast<std::size_t>(source)].unsavedTake ||
        state_->tracks[static_cast<std::size_t>(destination)].unsavedTake)
    {
        ToastRequest notice;
        notice.text = "Save the temporary take before swapping tracks";
        shell_->showToast(std::move(notice));
        return;
    }

    m2::cmd::exchangeTrackSlots(sceneIndex, source, destination);
}

bool TrackGrid::handle(const io::ControlEvent& e, UiContext&)
{
    if (shell_ == nullptr || state_ == nullptr)
        return false;
    const bool home = variant_ == TrackTileModel::Variant::Home;
    if (e.control == io::Control::Selector && e.kind == Kind::Turn) // selection follows the rotary, wraps 1 → 16
    {
        shell_->selectTrack(((state_->selectedTrack - 1 + e.value) % 16 + 16) % 16 + 1);
        return true;
    }
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        // push = TRACK MENU of the selected track (UI-401); SHIFT + push = its BROWSE (UI-403; a track without
        // BROWSE — THRU, MIDI — opens its TRACK MENU)
        if (home)
            shell_->open(state_->shift && m1::trackMode(*shell_, state_->selectedTrack) == 0 ? "track.browse"
                                                                                           : "track.menu");
        else
            swapSelection(state_->selectedTrack);
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
        for (int t = 1; t <= 16; ++t)
            if (contains(L::trackTile(t), touch::x(e), touch::y(e)))
            {
                if (! home)
                    swapSelection(t);
                else if (state_->selectedTrack == t)
                    shell_->open("track.menu");
                else
                    shell_->selectTrack(t);
                return true;
            }
    return false;
}

namespace
{
std::unique_ptr<Widget> makeTrackTileGrid(const ScreenSpec& s, const WidgetArgs& a)
{
    return std::make_unique<TrackGrid>(a, s.id == "trackSetting" ? TrackTileModel::Variant::Setting
                                                                 : TrackTileModel::Variant::Home);
}
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("TrackTile", &ui::makeTrackTileGrid)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "home",
                                    .title = "HOME",
                                    .parent = {},
                                    .body = ui::BodyTemplate::ContentMeter,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "TrackTile",
                                    .strip = ui::kHomeStrip,
                                    .meter = "master"}))
