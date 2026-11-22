// SET SLICE (07-ui-design/screens/playback.md "SET SLICE", "GRID SLICE popup"; 03-ui/screens.md "SLICE", UI-298,
// UI-344, UI-403; UID-708…710; spec 02-features/slicing.md SLC-001…006; moved here from
// PlaybackScreen.cpp, which laid it out behind `ui.setSlice`). Template rail+content, context bar trackContext,
// breadcrumb HOME › TRACK MENU › PLAYBACK › SET SLICE.
//   rail     ADD SLICE (a point at the playhead inside the current slice, else its mid-point) · DELETE SLICE (disabled
//            in place for slice 1 and grid points: only added points are deleted, UID-341) · GRID SLICE (ListPopup
//            2 … 64, current ✓; a count change discards moved / added points after "Reset N moved slice points?")
//   body     the selected track's waveform with a line per slice start (current `accent`), numbered boxes above
//            (outline; a moved / added point 2 lpx) and below (filled) for the AREA's 16 slices, the current slice's
//            band `accent-weak`; overview strip with a tick per slice; zoom gauge. The rotary zooms about the current
//            slice (SHIFT: vertical); a tap on the editor selects the slice under it; a tap on the overview moves the
//            window; a pad hit in SLICE mode selects its slice (ui/screens/setslice/SliceSync.h SliceLive).
//   strip    SLICE No. (1–N) · POSITION (moves the current slice's start, ‹ ›: coarse = frames / 128 per detent with
//            zero-crossing snap within ±5 ms, SHIFT = 1 frame, detents coalesce) · START · END (read-only frames, time
//            sub-readout) · AREA (which 16 slices the pads play, ‹ ›) · — (reserved: DETECT, SLC-009).
// Every edit is one core/commands/slice command (undoable); the engine hears it through the CommandBus bridge hook
// (SliceSync posts the new slice table).
#include "core/commands/slice/SliceCommands.h"
#include "core/util/playback/Grid.h"
#include "services/analysis/SliceDetect.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/playback/SampleViews.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/screens/setslice/SliceSync.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/StripOwner.h"
#include "ui/widgets/WaveformEditor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ui
{
namespace
{
namespace L = layout;
namespace sc = core::commands::slice;
namespace cs = core::slice;
using Kind = io::ControlEvent::Kind;

constexpr std::string_view kCountLabels[] = {"2", "4", "8", "16", "32", "64"};
constexpr RectF kPlot {232, 152, 944, 360};

class SetSliceBody final : public Widget, public RailOwner, public StripOwner
{
public:
    explicit SetSliceBody(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return {224, L::contentTop, 1040, L::contentH}; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (! focusSet_) // ADD SLICE holds the rail focus on entry (set-slice.svg)
        {
            focusSet_ = true;
            if (ctx.focus.focused < 0)
                ctx.focus.focused = 0;
        }
        const auto m = model();
        WaveformEditor::drawEditor(dl, ctx, WaveformEditor::kEditor, m);
        WaveformEditor::drawOverview(dl, ctx, WaveformEditor::kOverview, m);
        WaveformEditor::drawZoomGauge(dl, ctx, WaveformEditor::kGaugeX, WaveformEditor::kGaugeY, zoomLevel());
    }

    bool dirty(double) const override
    {
        if (state_ == nullptr)
            return false;
        const auto& live = ui::slice::sliceLive();
        return m1::feedsOf(shell_).position(state_->selectedTrack) >= 0
               || live.lastHitMs[static_cast<std::size_t>((state_->selectedTrack - 1) & 15)] != seenHit_;
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
        if (touch::isTouch(e) && e.value == touch::kDown)
            return tap(touch::x(e), touch::y(e));
        return touch::isTouch(e) && contains(WaveformEditor::kEditor, touch::x(e), touch::y(e));
    }

    // ---- rail ----
    // The rotary moves the rail from entry (ADD SLICE focused); turning past GRID SLICE gives it to the waveform zoom,
    // SHIFT + push toggles between the two (Shell).
    bool railFocusEnter(int) override { return true; }
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        const auto v = view();
        if (index == 0)
            st.disabled = v.frames <= 0 || ! sc::canAdd(*v.settings, v.frames);
        else if (index == 1)
            st.disabled = ! sc::canDelete(*v.settings, v.selected, v.frames);
    }
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        const auto v = view();
        auto& session = m2::Session::instance();
        if (index == 0)
        {
            if (v.frames > 0 && sc::canAdd(*v.settings, v.frames))
            {
                const std::int64_t at = sc::addFrame(*v.settings, v.selected, playFrame(v), v.frames);
                session.execute(sc::addPoint(session.project(), v.scene, v.track, at, v.frames));
                // The new point starts the slice after the current one: select it.
                const auto pts = cs::slicePoints(sc::settings(session.project(), v.scene, v.track), v.frames);
                for (int k = 0; k < pts.count; ++k)
                    if (pts.start(k) == at)
                        selected_ = k;
            }
            return true; // disabled: nothing
        }
        if (index == 1)
        {
            if (sc::canDelete(*v.settings, v.selected, v.frames))
            {
                session.execute(sc::deletePoint(session.project(), v.scene, v.track, v.selected, v.frames));
                selected_ = v.selected - 1;
            }
            return true;
        }
        if (index == 2)
        {
            openGridPopup();
            return true;
        }
        return false;
    }

    // ---- strip ----
    void stripColumn(int column, ParamColumnModel& c) const override
    {
        const auto v = view();
        if (v.frames <= 0 || v.pts.count == 0)
        {
            c.disabled = column != 0;
            if (column == 0)
                c.valueText = "1";
            return;
        }
        const std::int64_t a = v.pts.start(v.selected), b = v.pts.end(v.selected);
        switch (column)
        {
            case 0:
                std::snprintf(text_[0].data(), text_[0].size(), "%d", v.selected + 1);
                c.value = static_cast<float>(v.selected + 1);
                c.valueText = text_[0].data();
                break;
            case 1:
            case 4:
                c.valueText = "\xE2\x80\xB9 \xE2\x80\xBA"; // ‹ › (relative)
                c.stepped = true;
                break;
            case 2:
            case 3:
            {
                const std::int64_t f = column == 2 ? a : b;
                const auto k = static_cast<std::size_t>(column);
                std::snprintf(text_[k].data(), text_[k].size(), "%lld", static_cast<long long>(f));
                c.value = static_cast<float>(f);
                c.valueText = text_[k].data();
                c.subReadout = timeText(f, v.rate, sub_[k - 2]);
                c.readOnly = true;
                break;
            }
            default: break;
        }
    }
    bool stripTurn(int column, const core::ParamSpec&, int detents, bool fine) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return true;
        const auto v = view();
        auto& session = m2::Session::instance();
        switch (column)
        {
            case 0: selected_ = std::clamp(v.selected + detents, 0, std::max(0, v.pts.count - 1)); return true;
            case 1:
            {
                if (v.frames <= 0)
                    return true;
                std::int64_t target = v.pts.start(v.selected) + (fine ? 1 : core::playback::coarseStep(v.frames)) * detents;
                const auto& live = ui::slice::sliceLive();
                if (! fine && live.buffer) // SLC-006: coarse moves land on a zero crossing within ±5 ms
                    if (const auto* buf = live.buffer(v.scene, v.track); buf != nullptr && buf->frames == v.frames)
                    {
                        const auto z = services::analysis::nearestZeroCrossing(*buf, target,
                                                                               services::analysis::windowFrames(*buf));
                        if (z != v.pts.start(v.selected))
                            target = z;
                    }
                session.execute(sc::movePoint(session.project(), v.scene, v.track, v.selected, target, v.frames));
                return true;
            }
            case 4:
            {
                const int area = cs::clampArea(v.settings->windowOffset + detents, v.pts.count);
                if (area != v.settings->windowOffset)
                {
                    session.execute(sc::setArea(session.project(), v.scene, v.track, area, v.frames));
                    selected_ = area * cs::kPadsPerArea; // the AREA's first slice
                }
                return true;
            }
            default: return true; // START / END are readouts
        }
    }

    // The editor model now (also used by the tests).
    WaveformModel model() const
    {
        WaveformModel m;
        if (shell_ == nullptr || state_ == nullptr)
            return m;
        const auto v = view();
        m.colour = theme::trackColour(m1::padColour(*shell_, state_->selectedTrack));
        m.overview = v.sample.empty() ? nullptr : v.sample.overview;
        m.frames = v.frames;
        m.start = 0;
        m.end = v.frames;
        m.gridCount = 0;
        m.slices = v.pts.count;
        m.currentSlice = v.pts.count > 0 ? v.selected + 1 : 0;
        for (int k = 0; k < v.pts.count; ++k)
        {
            points_[static_cast<std::size_t>(k)] = v.pts.start(k);
            const auto& pt = v.pts.point[static_cast<std::size_t>(k)];
            if (pt.moved || pt.grid < 0)
                m.slicesEdited |= std::uint64_t {1} << k;
        }
        points_[static_cast<std::size_t>(v.pts.count)] = v.frames;
        if (v.pts.count > 0)
            m.slicePoints = {points_.data(), static_cast<std::size_t>(v.pts.count + 1)};
        m.firstBox = cs::clampArea(v.settings->windowOffset, v.pts.count) * cs::kPadsPerArea;
        m.play = playFrame(v);
        m.zoomX = zoomX_;
        m.zoomY = zoomY_;
        m.zoomCentre = centre_;
        return m;
    }
    int selectedSlice() const { return view().selected; }

private:
    struct View
    {
        int scene = 0, track = 0; // 0-based
        m1::SampleView sample {};
        std::int64_t frames = 0;
        double rate = 44100;
        const core::model::SliceSettings* settings = nullptr;
        cs::SlicePoints pts {};
        int selected = 0;
    };

    View view() const
    {
        View v;
        auto& session = m2::Session::instance();
        const auto& p = session.project();
        v.scene = p.settings.currentScene & 15;
        v.track = state_ != nullptr ? (state_->selectedTrack - 1) & 15 : 0;
        v.sample = m1::SampleViews::instance().of(v.track + 1);
        v.frames = v.sample.empty() ? 0 : v.sample.frames;
        v.rate = v.sample.sampleRate > 0 ? v.sample.sampleRate : 44100.0;
        v.settings = &sc::settings(p, v.scene, v.track);
        v.pts = cs::slicePoints(*v.settings, v.frames);
        // A pad hit in SLICE mode selects its slice (SLICE No. "also set by hitting a pad").
        const auto& live = ui::slice::sliceLive();
        const double hit = live.lastHitMs[static_cast<std::size_t>(v.track)];
        if (hit != seenHit_ || v.track != seenTrack_)
        {
            if (hit != seenHit_ && live.lastSlice[static_cast<std::size_t>(v.track)] >= 0)
                selected_ = live.lastSlice[static_cast<std::size_t>(v.track)];
            seenHit_ = hit;
            seenTrack_ = v.track;
        }
        v.selected = std::clamp(selected_, 0, std::max(0, v.pts.count - 1));
        return v;
    }
    std::int64_t playFrame(const View& v) const
    {
        const float pos = m1::feedsOf(shell_).position(v.track + 1);
        return pos >= 0 && v.frames > 0 ? static_cast<std::int64_t>(pos * static_cast<float>(v.frames)) : -1;
    }
    static std::string_view timeText(std::int64_t f, double rate, std::array<char, 24>& buf)
    {
        const double sec = static_cast<double>(f) / rate;
        const int minutes = static_cast<int>(sec / 60.0);
        std::snprintf(buf.data(), buf.size(), "%d:%06.3f", minutes, sec - 60.0 * minutes);
        return buf.data();
    }
    float zoomLevel() const noexcept
    {
        const auto v = view();
        const double maxZoom = std::max(2.0, static_cast<double>(v.frames) / 944.0);
        return static_cast<float>(std::log2(zoomX_) / std::log2(maxZoom));
    }
    void zoom(int detents, bool vertical)
    {
        if (vertical)
        {
            zoomY_ = std::clamp(zoomY_ * std::pow(2.0, detents / 4.0), 1.0, 8.0);
            return;
        }
        const auto v = view();
        const double maxZoom = std::max(1.0, static_cast<double>(v.frames) / 944.0);
        zoomX_ = std::clamp(zoomX_ * std::pow(2.0, detents / 2.0), 1.0, maxZoom);
        if (v.frames > 0 && v.pts.count > 0) // about the current slice's start (SLC-006)
            centre_ = static_cast<double>(v.pts.start(v.selected)) / static_cast<double>(v.frames);
    }
    bool tap(float x, float y)
    {
        const auto v = view();
        const auto ov = WaveformEditor::kOverview;
        if (contains(ov, x, y) && v.frames > 0)
        {
            centre_ = static_cast<double>(std::clamp((x - ov.x - 24) / (ov.w - 48), 0.0f, 1.0f));
            return true;
        }
        if (! contains(WaveformEditor::kEditor, x, y))
            return false;
        if (v.frames <= 0)
            return true;
        const std::int64_t f = WaveformEditor::frameAt(model(), kPlot, x);
        for (int k = v.pts.count - 1; k >= 0; --k)
            if (f >= v.pts.start(k))
            {
                selected_ = k;
                break;
            }
        return true;
    }
    void openGridPopup()
    {
        const auto v = view();
        ListRequest req;
        req.title = "GRID SLICE";
        const int count = cs::gridCountOf(*v.settings);
        for (std::size_t k = 0; k < cs::kGridCounts.size(); ++k)
        {
            req.items.push_back({kCountLabels[k], {}, {}, cs::kGridCounts[k]});
            if (cs::kGridCounts[k] == count)
                req.current = static_cast<int>(k);
        }
        const int scene = v.scene, track = v.track;
        req.onChoose = [this, scene, track](int, const ListItem& it) { chooseCount(scene, track, it.value); };
        shell_->showList(std::move(req));
    }
    void chooseCount(int scene, int track, int count)
    {
        auto& session = m2::Session::instance();
        const auto& s = sc::settings(session.project(), scene, track);
        if (cs::gridCountOf(s) == count && s.grids.empty())
            return;
        const int edited = cs::editedPoints(s);
        if (edited == 0)
        {
            session.execute(sc::setCount(session.project(), scene, track, count));
            selected_ = 0;
            return;
        }
        MessageRequest r; // UI-296: a destructive change asks first (undoable all the same)
        r.icon = glyph::Kind::Caution;
        r.title = "GRID SLICE";
        char line[64];
        std::snprintf(line, sizeof line, "Reset %d moved slice point%s?", edited, edited == 1 ? "" : "s");
        r.lines = {line};
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {"RESET", ButtonStyle::Destructive}};
        r.defaultButton = 0;
        r.onResult = [this, scene, track, count](int button)
        {
            if (button != 1)
                return;
            auto& ses = m2::Session::instance();
            ses.execute(sc::setCount(ses.project(), scene, track, count));
            selected_ = 0;
        };
        shell_->showMessage(std::move(r));
    }

    ShellApi* shell_;
    UiState* state_;
    double zoomX_ = 1, zoomY_ = 1, centre_ = 0.5;
    mutable int selected_ = 0, seenTrack_ = -1;
    mutable double seenHit_ = 0;
    mutable bool focusSet_ = false;
    mutable std::array<std::int64_t, cs::kMaxSlices + 1> points_ {};
    mutable std::array<std::array<char, 24>, 4> text_ {};
    mutable std::array<std::array<char, 24>, 2> sub_ {};
};

std::unique_ptr<Widget> makeSliceEditor(const ScreenSpec&, const WidgetArgs& a) { return std::make_unique<SetSliceBody>(a); }

constexpr RailItemSpec kSliceRail[] = {{RailItemSpec::Kind::Button, "ADD SLICE"},
                                       {RailItemSpec::Kind::Button, "DELETE SLICE"},
                                       {RailItemSpec::Kind::Button, "GRID SLICE"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("SliceEditor", &ui::makeSliceEditor)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.playback.setSlice",
                                    .title = "SET SLICE",
                                    .parent = "track.playback",
                                    .body = ui::BodyTemplate::RailContent,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = ui::kSliceRail,
                                    .bodyComponent = "SliceEditor",
                                    .strip = {"SET SLICE",
                                              {"slice.selected", "slice.position", "slice.start", "slice.end",
                                               "slice.area", ""}},
                                    .meter = {}}))
