// AMP ENVELOPE (07-ui-design/screens/amp-envelope-lfo.md; UI-230…234; design ids UID-725…). Template rail+content,
// context bar trackContext. EnvelopeEditor over the graph panel; rail slot 1 empty (reserved for PRESET, ENV-012 P2),
// SETTING (slot 2, opens the SETTING popup: Attack Curve, Release Curve, Retrigger) and SYNC (slot 3, env.sync): the
// shell's rail with a hole at slot 1 (ui/shell/RailOwner.h), SETTING runs this screen's popup (railActivate). Strip
// VELOCITY, ATTACK, HOLD, RELEASE, TRIGGER MODE.
// Times come from core::ampenv::envSeconds (display index 0–127, 128 = INFINITY). The time window auto-fits so the
// release ends at ≈ 85 % on entry / track change; with UTILITY › AMP ENV AUTO ZOOM ON it also re-fits 600 ms after the
// last change (never during a drag: the axis is frozen from touch down to up), animated `base`; OFF (the default) it
// stays and only widens at once when the envelope outgrows it. The rotary moves the active handle A → H → R; push opens
// SETTING; dragging A / H / R horizontally sets ATTACK / HOLD / RELEASE (past the right end = INF), A vertically sets
// VELOCITY.
// UID-726…731: with SYNC the time axis is labelled in note values (UID-728), and a playhead dot rides the envelope of
// the track's last trigger (amp-envelope-lfo.md "Playing"). The graph widget is the complete EnvelopeEditor (no
// separate `EnvelopeGraph` widget).
#include "core/util/ampenv/EnvTime.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/widgets/EnvelopeEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace env = ::core::ampenv;
using Kind = io::ControlEvent::Kind;
constexpr double kRefitMs = 600; // amp-envelope-lfo.md: re-fit 600 ms after the last change
constexpr int kRailSetting = 1;  // rail: hole (PRESET, P2) · SETTING · SYNC
constexpr const char* kStageIds[] = {"env.attack", "env.hold", "env.release"};

class AmpEnvelopeBody final : public Widget, public RailOwner
{
public:
    explicit AmpEnvelopeBody(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    EnvelopeModel model(double nowMs) const
    {
        EnvelopeModel m;
        if (shell_ == nullptr || state_ == nullptr)
            return m;
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        m = shapeOf([&](const char* id, float fallback) { return m1::trackParam(sh, id, t, fallback); });
        m.colour = theme::trackColour(m1::padColour(sh, t));
        m.gate = m1::trackFlag(sh, "track.triggerMode", t);
        m.activeHandle = active_;
        m.bpm = static_cast<float>(std::max(20.0, std::strtod(state_->bpm.text, nullptr)));
        // Playing: the dot rides the envelope of the track's last trigger as the shell's feeds saw it
        // (EngineFeeds::lastHitMs; a UI frame late at most), until the release ends (or the window, at INFINITY).
        const double hit = std::max(m1::feedsOf(shell_).lastHitMs(t),
                                    state_->tracks[static_cast<std::size_t>((t - 1) & 15)].lastHitMs);
        const float since = static_cast<float>((nowMs - hit) / 1000.0);
        const float end = m.holdInf || m.releaseInf ? 1e9f : std::max(m.attackS, m.holdS) + m.releaseS;
        m.playheadS = hit > 0 && since >= 0 && since <= end && since <= window_ ? since : -1;
        playing_ = m.playheadS >= 0;
        // Step hold: the locked shape (UID-166).
        if (state_->stepHeld > 0)
        {
            bool any = false;
            ghost_ = shapeOf(
                [&](const char* id, float fallback)
                {
                    const auto* spec = sh.params().find(id);
                    if (spec == nullptr)
                        return fallback;
                    float v = 0;
                    if (sh.values().lockOf(*spec, state_->trackScope(), state_->stepHeld, v))
                    {
                        any = true;
                        return v;
                    }
                    return m1::trackParam(sh, id, t, fallback);
                });
            if (any)
                m.lockGhost = &ghost_;
        }
        // Window: fitted; re-fitted kRefitMs after the last change, animated over motion.base.
        const float values[4] = {m.attackS, m.holdS, m.releaseS, m.holdInf ? 1.0f : (m.releaseInf ? 2.0f : 0.0f)};
        bool changed = false;
        for (int k = 0; k < 4; ++k)
            if (values[k] != last_[k])
            {
                changed = true;
                last_[k] = values[k];
            }
        if (changed)
        {
            changedAtMs_ = nowMs;
            if (window_ <= 0)
                window_ = from_ = to_ = EnvelopeEditor::fitWindow(m);
        }
        const float fit = EnvelopeEditor::fitWindow(m);
        if (state_->selectedTrack != fitTrack_) // another track: its own fit at once
        {
            fitTrack_ = state_->selectedTrack;
            window_ = from_ = to_ = fit;
        }
        const auto* autoZoom = shell_->params().find("ui.envAutoZoom");
        if (autoZoom != nullptr && shell_->values().get(*autoZoom, 0) >= 0.5f) // UTILITY › AMP ENV AUTO ZOOM ON
        {
            if (drag_ < 0 && fit != to_ && nowMs - changedAtMs_ >= kRefitMs) // never re-fit under a dragging finger
            {
                from_ = window_;
                to_ = fit;
                animFromMs_ = nowMs;
            }
        }
        else if (drag_ < 0 && fit > to_) // OFF (default): the axis stays; it only widens, at once, when outgrown
            window_ = from_ = to_ = fit;
        const float e = theme::ease(theme::progress(nowMs, animFromMs_, state_->reduceMotion ? 0 : tk::motionBaseMs));
        window_ = from_ + (to_ - from_) * e;
        m.windowS = window_;
        return m;
    }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr)
            return;
        const auto m = model(ctx.nowMs);
        EnvelopeEditor::draw(dl, ctx, m);
    }

    // The rotary moves the active handle; SHIFT + push gives the rail (SETTING · SYNC) the focus and turning past SYNC
    // returns it here (Shell).
    bool railFocusEnter(int) override { return true; }
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (index != kRailSetting || shell_ == nullptr || state_ == nullptr)
            return false; // SYNC: the shell toggles env.sync
        openSetting();
        return true;
    }

    bool dirty(double nowMs) const override
    {
        return nowMs - changedAtMs_ < kRefitMs + tk::motionBaseMs + 40 || window_ != to_ || playing_;
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            active_ = std::clamp(active_ + (e.value > 0 ? 1 : -1), 0, 2);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            openSetting();
            return true;
        }
        if (! touch::isTouch(e))
            return false;
        const float x = touch::x(e), y = touch::y(e);
        if (e.value == touch::kUp)
        {
            if (drag_ >= 0)
                changedAtMs_ = e.timeMs; // the axis re-fits kRefitMs after the finger lifts
            drag_ = -1;
            return true;
        }
        const auto m = model(e.timeMs);
        if (e.value == touch::kDrag && drag_ >= 0)
        {
            dragTo(drag_, x, y, m);
            return true;
        }
        if (e.value != touch::kDown)
            return false;
        for (int h = 0; h < 3; ++h)
        {
            const auto r = EnvelopeEditor::handleRect(m, h);
            if (contains({r.x - 12, r.y - 12, r.w + 24, r.h + 24}, x, y))
            {
                active_ = drag_ = h;
                from_ = to_ = window_; // freeze the time axis for the whole drag (a re-fit fed back into the value)
                return true;
            }
        }
        return contains(EnvelopeEditor::kPanel, x, y);
    }

private:
    template <typename Get> EnvelopeModel shapeOf(Get get) const
    {
        EnvelopeModel m;
        const bool sync = get("env.sync", 0) >= 0.5f;
        const double bpm = std::max(20.0, std::strtod(state_->bpm.text, nullptr));
        const auto ia = env::clampIndex(env::EnvStage::Attack, get("env.attack", 0));
        const auto ih = env::clampIndex(env::EnvStage::Hold, get("env.hold", 128));
        const auto ir = env::clampIndex(env::EnvStage::Release, get("env.release", 0));
        m.sync = sync;
        m.attackS = env::envSeconds(env::EnvStage::Attack, ia, sync, bpm);
        m.holdInf = env::isInfinite(env::EnvStage::Hold, ih);
        m.releaseInf = env::isInfinite(env::EnvStage::Release, ir);
        m.holdS = m.holdInf ? 0 : env::envSeconds(env::EnvStage::Hold, ih, sync, bpm);
        m.releaseS = m.releaseInf ? 0 : env::envSeconds(env::EnvStage::Release, ir, sync, bpm);
        m.velocity = static_cast<int>(std::lround(get("env.velocity", 127)));
        m.a = get("env.attackCurve", 0) >= 0.5f ? EnvCurve::Curve : EnvCurve::Linear;
        m.r = get("env.releaseCurve", 0) >= 0.5f ? EnvCurve::Curve : EnvCurve::Linear;
        return m;
    }

    // Seconds → the nearest display index of a stage (a drag past the plot's right end = INFINITY).
    static float indexFor(env::EnvStage stage, float seconds, bool sync, double bpm, bool pastEnd) noexcept
    {
        if (pastEnd && env::hasInfinity(stage))
            return env::kInfinityIndex;
        int best = 0;
        float bestD = 1e9f;
        for (int i = 0; i <= env::kMaxFiniteIndex; ++i)
        {
            const float d = std::fabs(env::envSeconds(stage, static_cast<std::uint8_t>(i), sync, bpm) - seconds);
            if (d < bestD)
                bestD = d, best = i;
        }
        return static_cast<float>(best);
    }

    void dragTo(int h, float x, float y, const EnvelopeModel& m)
    {
        const int t = state_->selectedTrack;
        const double bpm = std::max(20.0, std::strtod(state_->bpm.text, nullptr));
        const auto& plot = EnvelopeEditor::kPlot;
        const float s = std::max(0.0f, (x - plot.x) / plot.w * m.windowS);
        const bool pastEnd = x > plot.right();
        if (h == 0)
        {
            m1::setTrackParam(*shell_, "env.attack", t, indexFor(env::EnvStage::Attack, s, m.sync, bpm, false));
            const float level = std::clamp((plot.bottom() - y) / plot.h, 0.0f, 1.0f);
            if (y < plot.y + plot.h * 0.9f)
                m1::setTrackParam(*shell_, "env.velocity", t, std::round(level * 127));
        }
        else if (h == 1)
            m1::setTrackParam(*shell_, kStageIds[1], t, indexFor(env::EnvStage::Hold, s, m.sync, bpm, pastEnd));
        else
            m1::setTrackParam(*shell_, kStageIds[2], t,
                              indexFor(env::EnvStage::Release, std::max(0.0f, s - std::max(m.attackS, m.holdS)), m.sync,
                                       bpm, pastEnd));
    }

    void openSetting()
    {
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        SettingRequest req;
        req.title = "SETTING";
        static constexpr const char* ids[] = {"env.attackCurve", "env.releaseCurve", "env.retrigger"};
        static constexpr std::string_view labels[] = {"Attack Curve", "Release Curve", "Retrigger"};
        for (int i = 0; i < 3; ++i)
        {
            SettingRow row;
            row.label = labels[i];
            if (const auto* spec = sh.params().find(ids[i]))
                row.options.assign(spec->enumLabels.begin(), spec->enumLabels.end());
            else
                row.options = {"OFF", "ON"};
            row.current = static_cast<int>(std::lround(m1::trackParam(sh, ids[i], t)));
            req.rows.push_back(std::move(row));
        }
        req.onCommit = [this, t](int row, int option, float)
        {
            if (row >= 0 && row < 3)
                m1::setTrackParam(*shell_, ids[row], t, static_cast<float>(option));
        };
        sh.showSetting(std::move(req));
    }

    ShellApi* shell_;
    UiState* state_;
    int active_ = 1, drag_ = -1;
    mutable EnvelopeModel ghost_{};
    mutable float last_[4] = {-1, -1, -1, -1};
    mutable double changedAtMs_ = -1e9, animFromMs_ = -1e9;
    mutable float window_ = 0, from_ = 0, to_ = 0;
    mutable int fitTrack_ = -1; // the track the window was fitted for
    mutable bool playing_ = false;
};

std::unique_ptr<Widget> makeEnvelopeEditor(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<AmpEnvelopeBody>(a);
}
} // namespace
} // namespace ui

namespace ui
{
namespace
{
constexpr RailItemSpec kAmpRail[] = {{RailItemSpec::Kind::Button, ""},
                                     {RailItemSpec::Kind::Button, "SETTING"},
                                     {RailItemSpec::Kind::Toggle, "SYNC", {}, "env.sync"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("EnvelopeEditor", &ui::makeEnvelopeEditor)
APP_REGISTER_SCREEN((ui::ScreenSpec{
    .id = "track.ampEnvelope",
    .title = "AMP ENVELOPE",
    .parent = "track.menu",
    .body = ui::BodyTemplate::RailContent,
    .contextBar = ui::ContextBarKind::TrackContext,
    .rail = ui::kAmpRail,
    .bodyComponent = "EnvelopeEditor",
    .strip = {"AMP ENVELOPE", {"env.velocity", "env.attack", "env.hold", "env.release", "track.triggerMode", ""}}}))
