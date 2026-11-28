// PLAYBACK, SET SLICE and SET SCALE (07-ui-design/screens/playback.md; UI-235…239, UI-344, UID-700…714). Template
// rail+content, context bar trackContext (BACK + track box). PLAYBACK: WaveformEditor + OverviewStrip + ZoomGauge
// of the selected track's sample; rail SET SLICE · SET SCALE · REVERSE · GRID SNAP; strip PITCH, START, LOOP START,
// LENGTH, LOOP, TRIGGER MODE (the voice parameters; PITCH disabled under RESAMPLE by the column rule).
//   - START / LOOP START / LENGTH are always stored in sample frames (the engine's unit); END = START + LENGTH.
//     GRID SNAP snaps a dragged START / LOOP START / END flag to the drawn slice grid (the equal grid of N =
//     gridCount, or the moved / added points, SLC-011), PLY-004. An earlier build stored grid numbers 1…N
//     under GRID SNAP, which the engine read as frames (the flags never snapped on the unit).
//   - The active cursor is the flag whose value changed last (encoder, lock or drag); the rotary zooms in time about
//     it, SHIFT + rotary zooms vertically (UI-237); a tap on the overview moves the window (M p.36); a drag on a flag
//     moves it with snapping (UI-209).
//   - While a step is held, locked S / L / E positions show as mod ghost flags (UID-707).
// SET SCALE lives in ui/screens/setscale/ (ScaleMap + PadMap, and SET SCALE (MIDI)); SET SLICE in
// ui/screens/setslice/ (UID-708…710). The grid behind the waveform is the track's slice grid
// (SliceSettings: gridCount, drawn at its moved / added points, SLC-011); GRID SNAP snaps to that drawn grid.
#include "core/commands/slice/SliceCommands.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/playback/SampleViews.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/WaveformEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
constexpr int kGridCount = 16; // SliceSettings.gridCount default (the slices of a track without a model)
constexpr const char* kCursorIds[] = {"playback.start", "playback.loopStart", "playback.length"};

class PlaybackBody final : public Widget, public RailOwner, public StripOwner
{
public:
    // START / LOOP START / LENGTH step 128 frames a detent (their ParamSpec spans 2^24 frames): the panel encoder's
    // acceleration is capped by the selected track's sample instead.
    long long stripSteps(int, const core::ParamSpec& spec, bool fine) const override
    {
        if (std::find(std::begin(kCursorIds), std::end(kCursorIds), spec.id) == std::end(kCursorIds))
            return -1;
        if (fine || state_ == nullptr || ! (spec.coarseStep > 0))
            return 0;
        const auto frames = m1::SampleViews::instance().of(state_->selectedTrack).frames;
        return std::llround(static_cast<double>(frames) / static_cast<double>(spec.coarseStep));
    }

    // The rail (SET SLICE · SET SCALE · REVERSE · GRID SNAP) is the shell's; the rotary zooms the waveform (UI-237), so
    // the rail gets the focus with SHIFT + push (Shell) and turning past GRID SNAP returns it here.
    bool railFocusEnter(int) override { return true; }

    PlaybackBody(const WidgetArgs& a, bool slices) noexcept : shell_(a.shell), state_(a.state), slices_(slices) {}
    RectF bounds() const noexcept override { return {224, L::contentTop, 1040, L::contentH}; }

    // The editor model of the selected track now (also used by the tests).
    WaveformModel model(double nowMs) const
    {
        WaveformModel m;
        if (shell_ == nullptr || state_ == nullptr)
            return m;
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        view_ = m1::SampleViews::instance().of(t);
        m.colour = m1::padInk(sh, t);
        m.overview = view_.empty() ? nullptr : view_.overview;
        m.frames = view_.frames;
        m.gridSnap = m1::trackFlag(sh, "playback.gridSnap", t);
        m.gridCount = kGridCount;
        { // the track's slice grid (SET SLICE › GRID SLICE) and its points (SLC-011)
            auto& session = m2::Session::instance();
            const auto& p = session.project();
            const auto& sl = core::commands::slice::settings(p, p.settings.currentScene, t - 1);
            m.gridCount = core::slice::gridCountOf(sl);
            const auto pts = core::slice::slicePoints(sl, view_.frames);
            if (pts.count > 0 && ! sl.grids.empty())
            {
                for (int k = 0; k < pts.count; ++k)
                    points_[static_cast<std::size_t>(k)] = pts.start(k);
                points_[static_cast<std::size_t>(pts.count)] = pts.frames;
                m.slicePoints = {points_.data(), static_cast<std::size_t>(pts.count + 1)};
            }
        }
        m.loop = m1::trackFlag(sh, "playback.loop", t);
        m.reverse = m1::trackFlag(sh, "playback.reverse", t);
        const float vs = m1::trackParam(sh, "playback.start", t), vl = m1::trackParam(sh, "playback.loopStart", t),
                    vn = m1::trackParam(sh, "playback.length", t, static_cast<float>(m.frames));
        m.start = toFrames(vs, m);
        m.loopStart = std::max(m.start, toFrames(vl, m));
        m.end = std::clamp<std::int64_t>(m.start + static_cast<std::int64_t>(vn), m.start, m.frames);
        if (m.end <= m.start)
            m.end = m.frames;
        m.loopStart = std::min(m.loopStart, std::max<std::int64_t>(m.start, m.end - 1));
        const float pos = m1::feedsOf(shell_).position(t);
        m.play = pos >= 0 && m.frames > 0 ? static_cast<std::int64_t>(pos * static_cast<float>(m.frames)) : -1;
        // Active cursor: the value that changed last (UID-704).
        const float now[3] = {vs, vl, vn};
        for (int k = 0; k < 3; ++k)
        {
            if (now[k] != last_[k] && seen_)
                active_ = k == 2 ? 2 : k;
            last_[k] = now[k];
        }
        seen_ = true;
        m.activeCursor = active_;
        // Step hold: the locked positions as ghosts.
        if (state_->stepHeld > 0)
            for (int k = 0; k < 3; ++k)
                if (const auto* spec = sh.params().find(kCursorIds[k]))
                {
                    float v = 0;
                    if (sh.values().lockOf(*spec, state_->trackScope(), state_->stepHeld, v))
                    {
                        m.ghostMask |= static_cast<std::uint8_t>(1u << k);
                        m.lockGhosts[static_cast<std::size_t>(k)] =
                            k == 2 ? std::min(m.frames, m.start + static_cast<std::int64_t>(v)) : toFrames(v, m);
                    }
                }
        m.zoomX = zoomX_;
        m.zoomY = zoomY_;
        m.zoomCentre = centre_;
        if (slices_)
        {
            m.slices = kGridCount;
            m.currentSlice = currentSlice_;
            m.gridCount = 0;
        }
        (void)nowMs;
        return m;
    }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto m = model(ctx.nowMs);
        WaveformEditor::drawEditor(dl, ctx, WaveformEditor::kEditor, m);
        WaveformEditor::drawOverview(dl, ctx, WaveformEditor::kOverview, m);
        WaveformEditor::drawZoomGauge(dl, ctx, WaveformEditor::kGaugeX, WaveformEditor::kGaugeY, zoomLevel());
    }

    bool dirty(double) const override
    {
        return state_ != nullptr && m1::feedsOf(shell_).position(state_->selectedTrack) >= 0;
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            zoom(e.value, state_->shift);
            return true;
        }
        if (touch::isTouch(e))
            return touchAt(e);
        return false;
    }

private:
    static std::int64_t toFrames(float v, const WaveformModel& m) noexcept
    {
        return std::clamp<std::int64_t>(static_cast<std::int64_t>(v), 0, m.frames);
    }
    // The grid frame nearest to f while GRID SNAP is on (the grid drawn behind the waveform); END may reach the end.
    static std::int64_t snapFrame(std::int64_t f, const WaveformModel& m, bool end) noexcept
    {
        if (! m.gridSnap || m.frames <= 0)
            return f;
        if (m.slicePoints.size() >= 2) // the moved / added points; the last entry is the sample end
        {
            const std::size_t last = m.slicePoints.size() - (end ? 1 : 2);
            std::int64_t best = m.slicePoints[end ? 1 : 0];
            for (std::size_t k = end ? 1 : 0; k <= last; ++k)
                if (std::llabs(m.slicePoints[k] - f) < std::llabs(best - f))
                    best = m.slicePoints[k];
            return best;
        }
        const int n = std::max(1, m.gridCount);
        const auto g = std::clamp<std::int64_t>(std::llround(static_cast<double>(f) * n / static_cast<double>(m.frames)),
                                                end ? 1 : 0, end ? n : n - 1);
        return g * m.frames / n;
    }
    float zoomLevel() const noexcept
    {
        const double maxZoom = std::max(2.0, static_cast<double>(view_.frames) / 944.0);
        return static_cast<float>(std::log2(zoomX_) / std::log2(maxZoom));
    }
    void zoom(int detents, bool vertical)
    {
        if (vertical)
        {
            zoomY_ = std::clamp(zoomY_ * std::pow(2.0, detents / 4.0), 1.0, 8.0);
            return;
        }
        const auto m = model(0);
        const double maxZoom = std::max(1.0, static_cast<double>(m.frames) / 944.0);
        zoomX_ = std::clamp(zoomX_ * std::pow(2.0, detents / 2.0), 1.0, maxZoom);
        if (m.frames > 0) // zoom about the active cursor (UI-237)
        {
            const std::int64_t at = active_ == 0 ? m.start : active_ == 1 ? m.loopStart : m.end;
            centre_ = static_cast<double>(at) / static_cast<double>(m.frames);
        }
    }
    bool touchAt(const io::ControlEvent& e)
    {
        const float x = touch::x(e), y = touch::y(e);
        const auto m = model(0);
        if (e.value == touch::kUp)
        {
            drag_ = -1;
            return true;
        }
        if (e.value == touch::kDrag && drag_ >= 0)
        {
            moveCursor(drag_, WaveformEditor::frameAt(m, {232, 152, 944, 360}, x + grabDx_), m);
            return true;
        }
        if (e.value != touch::kDown)
            return false;
        const auto ov = WaveformEditor::kOverview;
        if (contains(ov, x, y) && m.frames > 0)
        {
            centre_ = static_cast<double>(
                std::clamp((x - ov.x - 24) / (ov.w - 48), 0.0f, 1.0f)); // tap = move the window there
            return true;
        }
        if (! contains(WaveformEditor::kEditor, x, y) || m.frames <= 0 || slices_)
            return contains(WaveformEditor::kEditor, x, y);
        const RectF plot{232, 152, 944, 360};
        const float xs = WaveformEditor::xOf(m, plot, m.start), xl = WaveformEditor::xOf(m, plot, m.loopStart),
                    xe = WaveformEditor::xOf(m, plot, m.end);
        const bool top = y < 152 + 180;
        int best = -1;
        float bestD = 28;
        const float cand[3] = {xs + 12, xl + 12, xe - 12};
        for (int k = 0; k < 3; ++k)
        {
            if (k == 1 && ! m.loop)
                continue;
            if ((k == 0) != top)
                continue;
            const float d = std::fabs(cand[k] - x);
            if (d < bestD)
                bestD = d, best = k;
        }
        if (best >= 0)
        {
            drag_ = best;
            active_ = best;
            grabDx_ = (best == 0 ? xs : best == 1 ? xl : xe) - x; // the marker keeps its offset from the finger
        }
        return true;
    }
    void moveCursor(int k, std::int64_t frame, const WaveformModel& m)
    {
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        frame = std::clamp<std::int64_t>(snapFrame(frame, m, k == 2), 0, m.frames);
        if (k == 2)
            m1::setTrackParam(sh, "playback.length", t, static_cast<float>(std::max<std::int64_t>(1, frame - m.start)));
        else
            m1::setTrackParam(sh, kCursorIds[k], t, static_cast<float>(frame));
    }

    ShellApi* shell_;
    UiState* state_;
    bool slices_;
    double zoomX_ = 1, zoomY_ = 1, centre_ = 0.5;
    int drag_ = -1, currentSlice_ = 1;
    float grabDx_ = 0;
    mutable m1::SampleView view_{};
    mutable std::array<std::int64_t, core::slice::kMaxSlices + 1> points_{};
    mutable float last_[3] = {0, 0, 0};
    mutable bool seen_ = false;
    mutable int active_ = 0;
};

std::unique_ptr<Widget> makeWaveformEditor(const ScreenSpec& s, const WidgetArgs& a)
{
    return std::make_unique<PlaybackBody>(a, s.id == "track.playback.setSlice");
}
constexpr RailItemSpec kPlaybackRail[] = {{RailItemSpec::Kind::Button, "SET SLICE", "track.playback.setSlice"},
                                          {RailItemSpec::Kind::Button, "SET SCALE", "track.playback.setScale"},
                                          {RailItemSpec::Kind::Toggle, "REVERSE", {}, "playback.reverse"},
                                          {RailItemSpec::Kind::Toggle, "GRID SNAP", {}, "playback.gridSnap"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("WaveformEditor", &ui::makeWaveformEditor)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.playback",
                                    .title = "PLAYBACK",
                                    .parent = "track.menu",
                                    .body = ui::BodyTemplate::RailContent,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = ui::kPlaybackRail,
                                    .bodyComponent = "WaveformEditor",
                                    .strip = {"PLAYBACK",
                                              {"playback.pitch", "playback.start", "playback.loopStart",
                                               "playback.length", "playback.loop", "track.triggerMode"}}}))
// SET SLICE: ui/screens/setslice/SetSliceScreen.cpp.
