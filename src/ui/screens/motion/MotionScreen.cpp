// MOTION (TRACK MENU › MOTION bar): the recorded motion of the selected track in the current pattern, one lane per
// parameter that holds motion (a lock on at least one step), drawn as the value the track plays on each step joined
// by straight lines — locked steps as dots in the track colour, unlocked steps on the track's own value (dim).
// Template full, context bar trackContext.
//   - Header: MOTION RECORD (track.motionRec, red when enabled), the scene / pattern / length.
//   - Lanes (5 visible, the list scrolls with the selection): a touch on a lane's graph selects it and draws — every
//     step the finger crosses gets the value at the finger's height (one undo step per stroke, editMotion coalesces);
//     a touch on the name selects the lane only. The rotary walks lanes and buttons; a push on a lane zooms it.
//   - Zoom (only from the rotary push; a touch never zooms): the lane fills the body under the header, with its value
//     scale and step numbers. The rotary moves the cursor step, a push adds the cursor step's point (the value the
//     track plays there) or removes it when it is there, a touch draws. BACK returns to the lanes (LeaveGuard); the
//     zoom also ends when the lane goes (CLEAR, undo) or the track changes.
//   - Knob strip: STEP moves the cursor along the pattern, the lane's parameter writes the cursor step's value.
//   - ADD PARAMETER lists the lockable parameters without motion (track slots, and the knobs of the effects FX1 / FX2
//     hold); the added lane is empty until a point is drawn. CLEAR STEP removes the cursor step's point, CLEAR
//     PARAMETER the lane, CLEAR ALL (asks) every lane. Everything is undoable (core::commands::sequencer).
#include "core/commands/sequencer/SequencerCommands.h"
#include "core/params/ParamRegistry.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/motion/MotionLanes.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Feeds.h"
#include "ui/shell/Layout.h"
#include "ui/shell/LeaveGuard.h"
#include "ui/shell/Popup.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/Toggle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace seqcmd = ::core::commands::sequencer;
using Kind = io::ControlEvent::Kind;

constexpr int kSlots = core::model::kLockSlots;
constexpr int kVisibleLanes = 5;
constexpr float kLaneY = 208, kLaneH = 64, kLaneGap = 0;
constexpr float kNameX = 32, kPlotX = 264, kPlotR = 1240, kPlotPad = 8;
constexpr RectF kRecToggle{1056, 152, 208, 48};
constexpr int kButtons = 4;
constexpr std::string_view kButtonLabels[kButtons] = {"ADD PARAMETER", "CLEAR STEP", "CLEAR PARAMETER", "CLEAR ALL"};
constexpr RectF buttonRect(int i) noexcept
{
    return {16 + 316 * static_cast<float>(i), 544, 300, 64};
}
constexpr RectF laneRect(int row) noexcept
{
    return {16, kLaneY + (kLaneH + kLaneGap) * static_cast<float>(row), 1248, kLaneH - 4};
}
// The zoomed lane: under the header to the bottom of the body; its plot leaves a gutter for the value scale (left)
// and a row for the step numbers (below).
constexpr RectF kZoomRect{16, kLaneY, 1248, L::contentBottom - kLaneY};
constexpr float kZoomScaleW = 120, kZoomStepsH = 32;
constexpr float nan() noexcept
{
    return std::numeric_limits<float>::quiet_NaN();
}

void toast(ShellApi& shell, std::string_view text)
{
    ToastRequest t;
    t.text.assign(text);
    shell.showToast(std::move(t));
}

class MotionBody final : public Widget, public StripOwner, public LeaveGuard
{
public:
    explicit MotionBody(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state)
    {
        stepSpec_ = a.params != nullptr ? a.params->find("motion.step") : nullptr;
        if (state_ != nullptr)
            track_ = state_->selectedTrack;
    }

    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        refresh();
        const auto& p = ctx.palette;
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        const auto colour = theme::trackColour(m1::padColour(sh, t));

        // Header.
        header_.format("TRACK %d", t);
        dl.fill({16, 152, 104, 48}, colour, tk::radiusS);
        dl.text(68, 176, Align::Centre, TextStyleId::Label, p.onAccent, header_.view());
        dl.text(136, 176, Align::Left, TextStyleId::Title, p.text1, "MOTION");
        info_.format("SCENE %02d \xC2\xB7 PATTERN %02d \xC2\xB7 %d STEPS", state_->scene, state_->pattern, length_);
        dl.text(296, 176, Align::Left, TextStyleId::Caption, p.text2, info_.view());
        const bool rec = m1::trackFlag(sh, "track.motionRec", t);
        Toggle::draw(dl, ctx, kRecToggle, {{}, rec, false, false, focus_ == kRecItem, p.rec});
        dl.text(kRecToggle.x + kRecToggle.w / 2, kRecToggle.y + 26, Align::Centre, TextStyleId::Label, p.text1,
                rec ? "MOTION RECORD: ON" : "MOTION RECORD: OFF");

        if (const int z = zoomLane(); z >= 0)
        {
            drawZoom(dl, ctx, z, colour);
            drawPlayhead(dl, ctx, zoomPlot(), zoomPlot().y, zoomPlot().h);
            return;
        }

        // Lanes.
        if (laneCount_ == 0)
        {
            dl.fill({16, kLaneY, 1248, kLaneH * kVisibleLanes - 4}, p.surface1, tk::radiusM);
            dl.text(640, kLaneY + 130, Align::Centre, TextStyleId::Label, p.text2, "NO MOTION ON THIS TRACK");
            dl.text(640, kLaneY + 162, Align::Centre, TextStyleId::Caption, p.text3,
                    "Enable MOTION RECORD, arm REC and play, then turn a knob \xE2\x80\x94 or ADD PARAMETER and draw");
        }
        for (int row = 0; row < kVisibleLanes; ++row)
        {
            const int lane = top_ + row;
            if (lane >= laneCount_)
                break;
            drawLane(dl, ctx, row, lane, colour);
        }
        // Playhead across the lanes.
        if (laneCount_ > 0)
        {
            const int rows = std::min(kVisibleLanes, laneCount_ - top_);
            drawPlayhead(dl, ctx, listPlot(laneRect(0)), kLaneY, kLaneH * static_cast<float>(rows) - 4);
        }
        // Scroll hint.
        if (laneCount_ > kVisibleLanes)
        {
            scroll_.format("%d\xE2\x80\x93%d / %d", top_ + 1, std::min(laneCount_, top_ + kVisibleLanes), laneCount_);
            dl.text(1040, 176, Align::Right, TextStyleId::Caption, p.text3, scroll_.view());
        }

        // Buttons.
        for (int i = 0; i < kButtons; ++i)
        {
            const bool disabled = i > 0 && laneCount_ == 0;
            Button::draw(dl, ctx, buttonRect(i),
                         {kButtonLabels[i], i == 3 ? ButtonStyle::Destructive : ButtonStyle::Plain, disabled, false,
                          focus_ == buttonItem(i)});
        }
    }

    bool dirty(double) const override { return state_ != nullptr && state_->playing; }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        refresh();
        const int zoom = zoomLane();
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (zoom >= 0)
                cursor_ = std::clamp(cursor_ + e.value, 0, length_ - 1);
            else
                moveFocus(e.value);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            if (zoom >= 0)
                togglePoint(zoom);
            else
                activate(focus_);
            return true;
        }
        if (! touch::isTouch(e))
            return false;
        const float x = touch::x(e), y = touch::y(e);
        if (e.value == touch::kUp)
        {
            drawLane_ = -1;
            return drawing_ ? (drawing_ = false, true) : false;
        }
        if (e.value == touch::kDrag)
        {
            if (! drawing_ || drawLane_ < 0)
                return false;
            drawAt(drawLane_, x, y);
            return true;
        }
        // Down.
        if (contains(kRecToggle, x, y))
        {
            if (zoom < 0)
                focus_ = kRecItem;
            activate(kRecItem);
            return true;
        }
        if (zoom >= 0) // the zoomed lane: a touch on it draws
        {
            if (! contains(kZoomRect, x, y))
                return contains(bounds(), x, y);
            drawing_ = true;
            drawLane_ = zoom;
            lastDrawStep_ = -1;
            drawAt(zoom, x, y);
            return true;
        }
        for (int i = 0; i < kButtons; ++i)
            if (contains(buttonRect(i), x, y))
            {
                activate(buttonItem(i));
                return true;
            }
        for (int row = 0; row < kVisibleLanes && top_ + row < laneCount_; ++row)
        {
            const auto r = laneRect(row);
            if (! contains(r, x, y))
                continue;
            const int lane = top_ + row;
            focus_ = lane;
            if (x >= kPlotX)
            {
                drawing_ = true;
                drawLane_ = lane;
                lastDrawStep_ = -1;
                drawAt(lane, x, y);
            }
            return true;
        }
        return contains(bounds(), x, y);
    }

    // ---- knob strip: STEP | the selected lane's parameter ----
    void stripColumn(int column, ParamColumnModel& c) const override
    {
        if (column == 0)
        {
            stepText_.format("%d / %d", cursor_ + 1, length_);
            c.valueText = stepText_.view();
            c.value = static_cast<float>(cursor_ + 1);
            return;
        }
        if (column == 1 && selectedLane() >= 0 && c.spec != nullptr)
        {
            float v = 0;
            c.locked = lockAt(laneSlot_[static_cast<std::size_t>(selectedLane())], cursor_, v);
            c.value = c.locked ? v : base(*c.spec);
        }
    }
    bool stripTurn(int column, const core::ParamSpec& spec, int detents, bool fine) override
    {
        refresh();
        if (column == 0)
        {
            cursor_ = std::clamp(cursor_ + detents, 0, length_ - 1);
            return true;
        }
        const int lane = selectedLane();
        if (column != 1 || lane < 0)
            return false;
        const int slot = laneSlot_[static_cast<std::size_t>(lane)];
        float v = 0;
        if (! lockAt(slot, cursor_, v))
            v = base(spec);
        write(slot, cursor_, spec.step(v, detents, fine));
        return true;
    }

    int laneCount() const noexcept { return laneCount_; }

    // ---- LeaveGuard: BACK on the zoomed lane returns to the lanes (HOME and the other screens leave) ----
    bool mayLeave(std::string_view target) override
    {
        if (! target.empty() || zoomSlot_ < 0)
            return true;
        zoomSlot_ = -1;
        drawing_ = false;
        drawLane_ = -1;
        return false;
    }

private:
    static constexpr int kRecItem = -1;
    int buttonItem(int i) const noexcept { return laneCount_ + i; }
    // The zoomed lane (its index now), -1 on the lanes.
    int zoomLane() const noexcept
    {
        for (int i = 0; zoomSlot_ >= 0 && i < laneCount_; ++i)
            if (laneSlot_[static_cast<std::size_t>(i)] == zoomSlot_)
                return i;
        return -1;
    }
    int selectedLane() const noexcept { return focus_ >= 0 && focus_ < laneCount_ ? focus_ : (laneCount_ > 0 ? std::min(lastLane_, laneCount_ - 1) : -1); }

    // Lanes (recorded slots + added ones), the pattern length, the strip binding; also keeps the focus valid.
    void refresh() const
    {
        auto& sh = *shell_;
        if (state_->selectedTrack != track_) // another track: its own lanes
        {
            track_ = state_->selectedTrack;
            added_ = 0;
            top_ = 0;
            fresh_ = true;
            zoomSlot_ = -1;
        }
        length_ = motion::patternLength(sh);
        cursor_ = std::clamp(cursor_, 0, length_ - 1);
        const auto* pat = motion::pattern(sh);
        const std::uint64_t recorded = pat != nullptr ? motion::recordedSlots(*pat, track_ - 1) : 0;
        added_ &= ~recorded; // a drawn lane is a recorded one now
        const std::uint64_t mask = recorded | added_;
        const int previous = selectedLane() >= 0 ? laneSlot_[static_cast<std::size_t>(selectedLane())] : -1;
        const int previousCount = laneCount_;
        laneCount_ = 0;
        for (int s = 0; s < kSlots; ++s)
            if ((mask >> s & 1u) != 0)
                if (const auto* spec = motion::specOf(sh, s))
                {
                    laneSlot_[static_cast<std::size_t>(laneCount_)] = s;
                    laneSpec_[static_cast<std::size_t>(laneCount_)] = spec;
                    ++laneCount_;
                }
        // Keep the focus on the same lane / button when lanes come and go; on entry: the first lane (or ADD).
        if (fresh_)
        {
            fresh_ = false;
            focus_ = laneCount_ > 0 ? 0 : buttonItem(0);
            lastLane_ = 0;
        }
        else if (focus_ >= previousCount && focus_ != kRecItem)
            focus_ = std::min(laneCount_ + (focus_ - previousCount), laneCount_ + kButtons - 1);
        else if (focus_ >= 0 && focus_ < previousCount && previous >= 0)
        {
            focus_ = laneCount_ > 0 ? std::min(focus_, laneCount_ - 1) : buttonItem(0);
            for (int i = 0; i < laneCount_; ++i)
                if (laneSlot_[static_cast<std::size_t>(i)] == previous)
                    focus_ = i;
        }
        if (const int z = zoomLane(); z >= 0)
            focus_ = z; // the zoomed lane is the selected one (the strip writes it)
        else
            zoomSlot_ = -1; // its lane went (CLEAR PARAMETER, undo)
        if (focus_ >= 0 && focus_ < laneCount_)
            lastLane_ = focus_;
        if (lastLane_ >= laneCount_)
            lastLane_ = std::max(0, laneCount_ - 1);
        const int sel = selectedLane();
        if (sel >= 0)
        {
            if (sel < top_)
                top_ = sel;
            else if (sel >= top_ + kVisibleLanes)
                top_ = sel - kVisibleLanes + 1;
        }
        top_ = std::clamp(top_, 0, std::max(0, laneCount_ - kVisibleLanes));
        bindStrip(sel >= 0 ? laneSpec_[static_cast<std::size_t>(sel)] : nullptr);
    }

    void bindStrip(const core::ParamSpec* spec) const
    {
        if (spec == boundSpec_ && bound_)
            return;
        boundSpec_ = spec;
        bound_ = true;
        stripIndex_ ^= 1;
        auto& strip = strips_[static_cast<std::size_t>(stripIndex_)];
        strip = {"MOTION", {stepSpec_ != nullptr ? stepSpec_->id : std::string_view {},
                            spec != nullptr ? spec->id : std::string_view {}, "", "", "", ""}, 0};
        shell_->bindStrip(&strip);
    }

    // MOTION RECORD, the lanes, then the buttons (only ADD PARAMETER while there is no lane).
    bool focusable(int f) const noexcept
    {
        if (f == kRecItem || (f >= 0 && f < laneCount_))
            return true;
        const int b = f - laneCount_;
        return b == 0 || (b > 0 && b < kButtons && laneCount_ > 0);
    }
    void moveFocus(int d)
    {
        int f = focus_;
        for (int n = 0; n < std::abs(d); ++n)
        {
            int next = f;
            do
                next += d > 0 ? 1 : -1;
            while (next >= kRecItem && next < laneCount_ + kButtons && ! focusable(next));
            if (next < kRecItem || next >= laneCount_ + kButtons)
                break;
            f = next;
        }
        focus_ = f;
        refresh();
    }

    void activate(int item)
    {
        auto& sh = *shell_;
        if (item == kRecItem)
        {
            m1::setTrackParam(sh, "track.motionRec", track_, m1::trackFlag(sh, "track.motionRec", track_) ? 0.0f : 1.0f);
            return;
        }
        if (item >= 0 && item < laneCount_) // a push on a lane zooms it
        {
            zoomSlot_ = laneSlot_[static_cast<std::size_t>(item)];
            drawing_ = false;
            drawLane_ = -1;
            refresh();
            return;
        }
        const int b = item - laneCount_;
        const int lane = selectedLane();
        focus_ = item;
        switch (b)
        {
        case 0:
            addParameter();
            return;
        case 1:
            if (lane >= 0)
                write(laneSlot_[static_cast<std::size_t>(lane)], cursor_, nan());
            return;
        case 2:
            if (lane >= 0)
            {
                const int slot = laneSlot_[static_cast<std::size_t>(lane)];
                added_ &= ~(std::uint64_t {1} << slot);
                m2::Session::instance().execute(seqcmd::clearMotion(state_->scene - 1, state_->pattern - 1,
                                                                    track_ - 1, std::uint64_t {1} << slot));
                toast(sh, "MOTION CLEARED");
            }
            return;
        case 3:
            if (laneCount_ > 0)
                confirmClearAll();
            return;
        default:
            return;
        }
    }

    // The cursor step's point on / off (a push on the zoomed lane).
    void togglePoint(int lane)
    {
        const int slot = laneSlot_[static_cast<std::size_t>(lane)];
        float v = 0;
        if (lockAt(slot, cursor_, v))
            write(slot, cursor_, nan());
        else
            write(slot, cursor_, base(*laneSpec_[static_cast<std::size_t>(lane)]));
    }

    void addParameter()
    {
        auto& sh = *shell_;
        std::uint64_t present = 0;
        for (int i = 0; i < laneCount_; ++i)
            present |= std::uint64_t {1} << laneSlot_[static_cast<std::size_t>(i)];
        ListRequest r;
        r.title = "ADD PARAMETER";
        r.grouped = true;
        for (int s = 0; s < kSlots; ++s)
        {
            if ((present >> s & 1u) != 0)
                continue;
            const auto* spec = motion::specOf(sh, s);
            if (spec == nullptr)
                continue;
            const std::string_view group = s < 23 ? spec->group : std::string_view(s < 29 ? "FX1" : "FX2");
            r.items.push_back({spec->label, group, {}, s});
        }
        if (r.items.empty())
        {
            toast(sh, "EVERY PARAMETER HAS A LANE");
            return;
        }
        std::stable_sort(r.items.begin(), r.items.end(),
                         [](const ListItem& a, const ListItem& b) { return a.group < b.group; });
        r.onChoose = [this](int, const ListItem& item)
        {
            added_ |= std::uint64_t {1} << item.value;
            refresh();
            for (int i = 0; i < laneCount_; ++i)
                if (laneSlot_[static_cast<std::size_t>(i)] == item.value)
                    focus_ = i;
            refresh();
        };
        sh.showList(std::move(r));
    }

    void confirmClearAll()
    {
        char line[80];
        std::snprintf(line, sizeof line, "Clear the motion of %d parameter%s on track %d, this pattern?", laneCount_,
                      laneCount_ == 1 ? "" : "s", track_);
        MessageRequest r;
        r.icon = glyph::Kind::Caution;
        r.title = "CLEAR ALL MOTION";
        r.lines = {line};
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {"CLEAR", ButtonStyle::Destructive}};
        r.defaultButton = 0;
        const int scene = state_->scene - 1, pattern = state_->pattern - 1, track = track_ - 1;
        r.onResult = [this, scene, pattern, track](int button)
        {
            if (button != 1)
                return;
            added_ = 0;
            m2::Session::instance().execute(
                seqcmd::clearMotion(scene, pattern, track, (std::uint64_t {1} << kSlots) - 1));
        };
        shell_->showMessage(std::move(r));
    }

    // ---- model ----
    bool lockAt(int slot, int step, float& v) const
    {
        const auto* pat = motion::pattern(*shell_);
        return pat != nullptr && seqcmd::lockOf(*pat, step, track_ - 1, static_cast<core::model::LockSlot>(slot), v);
    }
    float base(const core::ParamSpec& spec) const
    {
        return shell_->values().get(spec, motion::scopeOf(*shell_, spec));
    }
    void write(int slot, int step, float value) const
    {
        const int steps[1] = {step};
        const float values[1] = {value};
        m2::Session::instance().execute(seqcmd::editMotion(state_->scene - 1, state_->pattern - 1, track_ - 1, steps,
                                                           static_cast<core::model::LockSlot>(slot), values));
    }

    // ---- geometry ----
    // A lane's plot in the list (right of its name), the zoomed lane's plot (between the value scale and the step
    // numbers), and the plot the finger draws on.
    static constexpr RectF listPlot(const RectF& r) noexcept
    {
        return {kPlotX, r.y + kPlotPad, kPlotR - kPlotX, r.h - 2 * kPlotPad};
    }
    static constexpr RectF zoomPlot() noexcept
    {
        return {kZoomRect.x + kZoomScaleW, kZoomRect.y + 48, kZoomRect.w - kZoomScaleW - 16,
                kZoomRect.h - 48 - kZoomStepsH};
    }
    RectF plotOf(int lane) const noexcept { return zoomLane() == lane ? zoomPlot() : listPlot(laneRect(lane - top_)); }
    float stepX(const RectF& plot, int step) const noexcept
    {
        const float w = plot.w - 2 * kPlotPad;
        return plot.x + kPlotPad + w * (static_cast<float>(step) + 0.5f) / static_cast<float>(length_);
    }
    int stepAtX(const RectF& plot, float x) const noexcept
    {
        const float w = plot.w - 2 * kPlotPad;
        return std::clamp(static_cast<int>((x - plot.x - kPlotPad) / w * static_cast<float>(length_)), 0, length_ - 1);
    }

    // ---- drawing (finger) ----
    // The finger at (x, y) on a lane: that step gets the value at the finger's height, and the steps a fast swipe
    // jumped over since the last point get the values on the straight line between the two (one command).
    void drawAt(int lane, float x, float y)
    {
        if (lane < 0 || lane >= laneCount_)
            return;
        if (zoomLane() != lane && (lane < top_ || lane >= top_ + kVisibleLanes))
            return;
        const auto plot = plotOf(lane);
        const float y01 = 1.0f - (y - plot.y) / plot.h;
        const int step = stepAtX(plot, x);
        const auto& spec = *laneSpec_[static_cast<std::size_t>(lane)];
        const int slot = laneSlot_[static_cast<std::size_t>(lane)];
        std::array<int, core::model::kSteps> steps {};
        std::array<float, core::model::kSteps> values {};
        int n = 0;
        const int from = lastDrawStep_ >= 0 && lastDrawStep_ != step ? lastDrawStep_ : step;
        const int dir = step >= from ? 1 : -1;
        for (int s = from == step ? step : from + dir;; s += dir)
        {
            const float t = step == from ? 1.0f : static_cast<float>(s - from) / static_cast<float>(step - from);
            const float v = motion::valueAt(spec, lastDrawY_ + (y01 - lastDrawY_) * (lastDrawStep_ >= 0 ? t : 1.0f));
            float old = 0;
            if (! lockAt(slot, s, old) || old != v) // the same value: nothing to write there
            {
                steps[static_cast<std::size_t>(n)] = s;
                values[static_cast<std::size_t>(n++)] = v;
            }
            if (s == step)
                break;
        }
        cursor_ = step;
        lastDrawStep_ = step;
        lastDrawY_ = y01;
        if (n > 0)
            m2::Session::instance().execute(seqcmd::editMotion(
                state_->scene - 1, state_->pattern - 1, track_ - 1,
                std::span<const int>(steps.data(), static_cast<std::size_t>(n)), static_cast<core::model::LockSlot>(slot),
                std::span<const float>(values.data(), static_cast<std::size_t>(n))));
    }

    void drawLane(DrawList& dl, const UiContext& ctx, int row, int lane, std::uint32_t colour) const
    {
        const auto& p = ctx.palette;
        const auto r = laneRect(row);
        const auto& spec = *laneSpec_[static_cast<std::size_t>(lane)];
        const bool selected = lane == selectedLane();
        dl.fill(r, selected ? p.surface2 : p.surface1, tk::radiusS);

        // Name and the cursor step's value.
        dl.text(kNameX, r.y + 20, Align::Left, TextStyleId::Label, p.text1, spec.label);
        const bool locked = cursorText(lane, valueText_[static_cast<std::size_t>(row)], "%d: %.*s");
        dl.text(kNameX, r.y + 42, Align::Left, TextStyleId::Caption, locked ? colour : p.text3,
                valueText_[static_cast<std::size_t>(row)].view());

        drawPlot(dl, ctx, listPlot(r), lane, colour, selected, false);
        if (lane == focus_)
            draw::focusRing(dl, p, r, tk::radiusS);
    }

    // The zoomed lane: name and cursor value above, the value scale left, the step numbers below the plot.
    void drawZoom(DrawList& dl, const UiContext& ctx, int lane, std::uint32_t colour) const
    {
        const auto& p = ctx.palette;
        const auto r = kZoomRect;
        const auto& spec = *laneSpec_[static_cast<std::size_t>(lane)];
        dl.fill(r, p.surface2, tk::radiusS);
        dl.text(kNameX, r.y + 24, Align::Left, TextStyleId::Title, p.text1, spec.label);
        const bool locked = cursorText(lane, valueText_[0], "STEP %d: %.*s");
        dl.text(kNameX + 360, r.y + 24, Align::Left, TextStyleId::Label, locked ? colour : p.text2,
                valueText_[0].view());
        if (laneCount_ > 1)
        {
            scroll_.format("%d / %d", lane + 1, laneCount_);
            dl.text(r.right() - 240, r.y + 24, Align::Right, TextStyleId::Caption, p.text3, scroll_.view());
        }
        dl.text(r.right() - 16, r.y + 24, Align::Right, TextStyleId::Caption, p.text3, "BACK: ALL PARAMETERS");

        const auto plot = zoomPlot();
        // Value scale: top, middle, bottom of the parameter's range, level with the grid lines of drawPlot.
        for (int i = 0; i < 3; ++i)
        {
            const float y01 = 1.0f - 0.5f * static_cast<float>(i);
            char buf[24];
            const int n = spec.formatValue(motion::valueAt(spec, y01), buf, sizeof buf);
            scaleText_[static_cast<std::size_t>(i)].format("%.*s", std::max(0, n), buf);
            dl.text(plot.x - 12, plot.bottom() - y01 * plot.h, Align::Right, TextStyleId::Caption, p.text2,
                    scaleText_[static_cast<std::size_t>(i)].view());
        }
        // Step numbers: every step up to 16 steps, else every 4th (the beats).
        const int every = length_ <= 16 ? 1 : 4;
        for (int s = 0; s < length_; s += every)
        {
            stepNumbers_[static_cast<std::size_t>(s)].format("%d", s + 1);
            dl.text(stepX(plot, s), plot.bottom() + kZoomStepsH / 2, Align::Centre, TextStyleId::Caption,
                    s == cursor_ ? p.text1 : p.text3, stepNumbers_[static_cast<std::size_t>(s)].view());
        }
        drawPlot(dl, ctx, plot, lane, colour, true, true);
        draw::focusRing(dl, p, r, tk::radiusS);
    }

    // "<format>" of the cursor step: its point's value, else the track's own (returns whether the step has a point).
    bool cursorText(int lane, FixedText<40>& text, const char* format) const
    {
        const auto& spec = *laneSpec_[static_cast<std::size_t>(lane)];
        float v = 0;
        const bool locked = lockAt(laneSlot_[static_cast<std::size_t>(lane)], cursor_, v);
        char buf[24];
        const int n = spec.formatValue(locked ? v : base(spec), buf, sizeof buf);
        text.format(format, cursor_ + 1, std::max(0, n), buf);
        return locked;
    }

    // A lane's plot: bar lines (zoomed: beat lines and a value grid too), the cursor column, the track's own value
    // and the motion.
    void drawPlot(DrawList& dl, const UiContext& ctx, const RectF& plot, int lane, std::uint32_t colour, bool cursor,
                  bool zoom) const
    {
        const auto& p = ctx.palette;
        const int slot = laneSlot_[static_cast<std::size_t>(lane)];
        const auto& spec = *laneSpec_[static_cast<std::size_t>(lane)];
        dl.fill(plot, p.bg, tk::radiusXs);
        const float x0 = plot.x + kPlotPad, w = plot.w - 2 * kPlotPad;
        const float stepW = w / static_cast<float>(length_);
        if (zoom)
        {
            for (int g = 1; g < 4; ++g)
                draw::hline(dl, plot.x, plot.right(), plot.y + plot.h * static_cast<float>(g) / 4, p.border);
            for (int b = 4; b < length_; b += 4)
                if (b % 16 != 0)
                    draw::vline(dl, x0 + stepW * static_cast<float>(b), plot.y, plot.bottom(), p.border);
        }
        for (int b = 16; b < length_; b += 16)
            draw::vline(dl, x0 + stepW * static_cast<float>(b), plot.y, plot.bottom(), p.borderStrong);
        if (cursor)
            dl.fill({x0 + stepW * static_cast<float>(cursor_), plot.y, std::max(2.0f, stepW), plot.h},
                    theme::withOpacity(p.accent, 0.18f));
        auto yOf = [&](float value) { return plot.bottom() - spec.normalised(value) * plot.h; };
        const float baseY = yOf(base(spec));
        dl.fill({x0, baseY - 0.5f, w, 1}, theme::withOpacity(p.text3, 0.6f));

        // The value each step plays, joined by straight lines (columns of 2 lpx where the line slopes).
        std::array<float, core::model::kSteps> ys {};
        std::array<bool, core::model::kSteps> lk {};
        const auto* pat = motion::pattern(*shell_);
        for (int s = 0; s < length_; ++s)
        {
            float lv = 0;
            lk[static_cast<std::size_t>(s)] = pat != nullptr && seqcmd::lockOf(*pat, s, track_ - 1,
                                                                                static_cast<core::model::LockSlot>(slot), lv);
            ys[static_cast<std::size_t>(s)] = lk[static_cast<std::size_t>(s)] ? yOf(lv) : baseY;
        }
        const auto dim = theme::withOpacity(p.text3, 0.8f);
        const float thick = zoom ? 3.0f : 2.0f, dot = zoom ? 14.0f : 8.0f;
        for (int s = 0; s + 1 < length_; ++s)
        {
            const bool both = lk[static_cast<std::size_t>(s)] && lk[static_cast<std::size_t>(s + 1)];
            const bool any = lk[static_cast<std::size_t>(s)] || lk[static_cast<std::size_t>(s + 1)];
            if (! any)
                continue; // the track's own value: already the base line
            segment(dl, stepX(plot, s), ys[static_cast<std::size_t>(s)], stepX(plot, s + 1),
                    ys[static_cast<std::size_t>(s + 1)], both ? colour : dim, thick);
        }
        for (int s = 0; s < length_; ++s)
            if (lk[static_cast<std::size_t>(s)])
            {
                const float x = stepX(plot, s), y = ys[static_cast<std::size_t>(s)];
                dl.fill({x - dot / 2, y - dot / 2, dot, dot}, colour, dot / 2);
            }
    }

    // The playhead over `plot`'s steps, from y0 down h (the list: across the visible lanes).
    void drawPlayhead(DrawList& dl, const UiContext& ctx, const RectF& plot, float y0, float h) const
    {
        const auto& feeds = shell_->feeds();
        const auto& ph = feeds.playhead();
        if (state_->playing && feeds.live() && ph.scene == state_->scene - 1 && ph.pattern == state_->pattern - 1 &&
            ph.step >= 0 && ph.step < length_)
            dl.fill({stepX(plot, ph.step) - 1, y0, 2, h}, theme::withOpacity(ctx.palette.playhead, 0.7f));
    }

    // A straight line from (xa, ya) to (xb, yb), t lpx thick: one fill when flat, else one column per 2 lpx of rise
    // (bounded by the width), so a slope costs a few fills, not one per pixel.
    static void segment(DrawList& dl, float xa, float ya, float xb, float yb, std::uint32_t c, float t)
    {
        const float dx = xb - xa, dy = yb - ya;
        if (std::fabs(dy) < 1.0f)
        {
            dl.fill({xa, std::min(ya, yb) - t / 2, dx, t}, c);
            return;
        }
        const int n = std::clamp(static_cast<int>(std::fabs(dy) / 2.0f) + 1, 1, std::max(1, static_cast<int>(dx / 2.0f)));
        const float cw = dx / static_cast<float>(n);
        for (int i = 0; i < n; ++i)
        {
            const float y0 = ya + dy * static_cast<float>(i) / static_cast<float>(n);
            const float y1 = ya + dy * static_cast<float>(i + 1) / static_cast<float>(n);
            dl.fill({xa + cw * static_cast<float>(i), std::min(y0, y1) - t / 2, cw + 0.5f, std::fabs(y1 - y0) + t}, c);
        }
    }

    ShellApi* shell_;
    UiState* state_;
    const core::ParamSpec* stepSpec_ = nullptr;
    mutable int track_ = 1, length_ = 16, cursor_ = 0, focus_ = 0, lastLane_ = 0, top_ = 0;
    mutable int laneCount_ = 0;
    mutable bool fresh_ = true;
    mutable std::array<int, kSlots> laneSlot_ {};
    mutable std::array<const core::ParamSpec*, kSlots> laneSpec_ {};
    mutable std::uint64_t added_ = 0; // lanes added with ADD PARAMETER that hold no point yet
    mutable const core::ParamSpec* boundSpec_ = nullptr;
    mutable bool bound_ = false;
    mutable std::array<StripSpec, 2> strips_ {};
    mutable int stripIndex_ = 0;
    bool drawing_ = false;
    int drawLane_ = -1, lastDrawStep_ = -1;
    float lastDrawY_ = 0;
    mutable FixedText<16> header_ {}, scroll_ {}, stepText_ {};
    mutable FixedText<64> info_ {};
    mutable std::array<FixedText<40>, kVisibleLanes> valueText_ {};
    mutable std::array<FixedText<24>, 3> scaleText_ {};
    mutable std::array<FixedText<4>, core::model::kSteps> stepNumbers_ {};
    mutable int zoomSlot_ = -1; // the zoomed lane's lock slot, -1 on the lanes
};

std::unique_ptr<Widget> makeMotionBody(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<MotionBody>(a);
}

// The cursor column of the MOTION strip (never saved, never sent: the page handles its turns).
void addMotionParams(core::ParamRegistry& r)
{
    r.add({.id = "motion.step", .label = "STEP", .shortLabel = "STEP", .group = "MOTION",
           .type = core::ParamType::Integer, .minValue = 1, .maxValue = 64, .defaultValue = 1,
           .scope = core::ParamScope::Global, .storage = {.key = "session.motion.step"}});
}
} // namespace
} // namespace ui

APP_REGISTER_PARAMS("ui_motion", 9920, &ui::addMotionParams)
APP_REGISTER_WIDGET("MotionEditor", &ui::makeMotionBody)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.motion",
                                    .title = "MOTION",
                                    .parent = "track.menu",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = {},
                                    .bodyComponent = "MotionEditor",
                                    .strip = {"MOTION", {"motion.step", "", "", "", "", ""}}}))
