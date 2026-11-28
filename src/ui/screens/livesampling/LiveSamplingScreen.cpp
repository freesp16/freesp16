// LIVE SAMPLING (07-ui-design/screens/sampling.md "LIVE SAMPLING", UID-785…789; 03-ui/screens.md LIVE
// SAMPLING; SMP-020…027). Screen id `sampling.live` (the id TRACK MENU's LIVE SAMPLING action opens), parent TRACK
// MENU, the SAMPLING/EDIT geometry (rail · INPUT meter · editor · overview), SCENE | PATTERN enabled (live sampling
// follows pattern changes).
//   - Rail: REC (Rec Toggle: off / armed blinking — starts at the next pattern-quantize boundary — / recording),
//     SOFT LIMITER, UNLOAD (the take leaves the track, undoable), SAVE (the take to SAVE TO; disabled with no take).
//   - The RingView (ui/widgets/RingView.h: 6 bars back, 2 forward of NOW at 75 %, bar lines, the pending take region,
//     the countdown chip) and the TakeStrip (LAST TAKE, its window offset by NUDGE, `NUDGE +12 ms`).
//   - Knobs TRACK (live.track, the selected track on entry), LENGTH (live.length), LOOP AFTER REC, NUDGE (live.nudge:
//     shown in ms, one 64-frame step per detent, one frame with SHIFT; the take window follows live, SMP-024), SAVE TO,
//     MONITOR LEVEL. The SOURCE is SAMPLING/EDIT's (sub-readout under TRACK).
//   - An `UNSAVED TAKE` badge (top right of the ring view) stays until the take is saved (UID-789).
// While the screen is open live.mode is on (the engine's live sampler listens; the REC key is the screen's) and
// the input monitor is on (routing.monitor); both go off when it closes. The REC key arms / disarms like the rail item.
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/sampling/SamplingHost.h"
#include "ui/screens/sampling/SamplingParts.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/StripOwner.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/RingView.h"
#include "ui/widgets/WaveformEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
using core::sampling::LiveState;

constexpr int kRailRec = 0, kRailLimiter = 1, kRailUnload = 2, kRailSave = 3;
constexpr float kLiveBars[] = {0.25f, 0.5f, 1, 2, 4, 8}; // live.length (sampling.cpp liveLengthLabels)

class LiveSampling final : public Widget, public RailOwner, public StripOwner
{
public:
    explicit LiveSampling(const WidgetArgs& a) : shell_(a.shell), state_(a.state)
    {
        if (shell_ != nullptr && state_ != nullptr)
            set("live.track", static_cast<float>((state_->selectedTrack - 1) & 15));
        set("live.mode", 1);
        set("routing.monitor", 1);
    }
    ~LiveSampling() override
    {
        set("live.mode", 0);
        set("routing.monitor", 0);
    }
    RectF bounds() const noexcept override { return {224, L::contentTop, 1040, L::contentH}; }
    const RingView& ring() const noexcept { return ring_; }

    int scene() const noexcept { return state_ != nullptr ? (state_->scene - 1) & 15 : 0; }
    int target() const noexcept { return static_cast<int>(std::lround(get("live.track"))) & 15; }
    const core::sampling::CaptureHub* hub() const noexcept
    {
        const auto* h = m5::SamplingHost::current();
        return h != nullptr ? h->hub() : nullptr;
    }
    LiveState liveState() const noexcept
    {
        const auto* hb = hub();
        return hb != nullptr ? static_cast<LiveState>(hb->status.liveState.load(std::memory_order_acquire))
                             : LiveState::Idle;
    }
    bool hasTake() const noexcept
    {
        const auto* h = m5::SamplingHost::current();
        return h != nullptr && h->liveTrack() >= 0;
    }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        const auto& p = ctx.palette;
        const auto ls = liveState();
        state_->samplerRec = ls == LiveState::Recording || (ls == LiveState::Armed && std::fmod(ctx.nowMs, 1000.0) < 500.0);
        const auto* hb = hub();
        input_.update(hb, ctx.nowMs);
        m5::drawInputMeter(dl, ctx, input_, -120.0f);
        // The ring view: the hub's ring and live status, the transport's tempo and position.
        LiveSamplingFeed f;
        const auto& ph = m1::feedsOf(shell_).playhead();
        f.ring = hb != nullptr ? &hb->ring : nullptr;
        f.sampleRate = hb != nullptr ? hb->sampleRate : 44100.0;
        f.bpm = ph.bpm > 0 ? ph.bpm : 120.0;
        f.ppq = ph.ppq;
        f.playing = ph.playing || state_->playing;
        f.state = static_cast<int>(ls);
        if (hb != nullptr)
        {
            const auto& s = hb->status;
            f.takeStart = s.liveStart.load(std::memory_order_relaxed);
            f.takeLength = s.liveLength.load(std::memory_order_relaxed);
        }
        const int lengthIndex = std::clamp(static_cast<int>(std::lround(get("live.length"))), 0, 5);
        f.bars = kLiveBars[lengthIndex];
        f.colour = m1::padInk(*shell_, target() + 1);
        ring_.setFeed(f);
        ring_.update();
        ring_.build(dl, ctx);
        if (const auto* h = m5::SamplingHost::current(); h != nullptr && h->liveUnsaved())
        {
            constexpr RectF badge {1052, 164, 120, 24};
            dl.fill(badge, theme::withOpacity(p.warn, 0.16f), tk::radiusXs);
            dl.text(badge.x + badge.w / 2, badge.y + 12, Align::Centre, TextStyleId::Caption, p.warn, "UNSAVED TAKE");
        }
        TakeStrip::draw(dl, ctx, takeModel(f));
        WaveformEditor::drawZoomGauge(dl, ctx, WaveformEditor::kGaugeX, WaveformEditor::kGaugeY, 0);
    }

    bool dirty(double) const override { return hub() != nullptr; }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Rec && e.kind == Kind::Press)
        {
            toggleRec();
            return true;
        }
        return false; // the rotary walks the rail
    }

    // --- RailOwner ---------------------------------------------------------------------------------------------------
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        auto* h = m5::SamplingHost::current();
        switch (index)
        {
        case kRailRec:
            toggleRec();
            return true;
        case kRailUnload:
            if (h != nullptr && hasTake())
            {
                h->unloadLive(scene(), h->liveTrack());
                toast("Take unloaded");
            }
            return true;
        case kRailSave:
            if (h != nullptr && hasTake())
            {
                const int to = static_cast<int>(std::lround(get("live.saveTo")));
                h->saveLive(to, [this](bool ok, std::string m)
                            { toast(ok ? std::string("Saved to Recordings/") : "Save failed: " + m); });
            }
            return true;
        default:
            return false;
        }
    }
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        if (index == kRailLimiter)
            st.onColour = theme::palette(state_ ? state_->skin : theme::Skin::Dark).warn;
        else if (index == kRailUnload || index == kRailSave)
            st.disabled = ! hasTake();
    }
    bool railDraw(int index, const RailItemSpec&, const RailItemState& st, DrawList& dl,
                  const UiContext& ctx) const override
    {
        if (index != kRailRec)
            return false;
        const auto ls = liveState();
        m5::drawRecItem(dl, ctx, L::railItem(0),
                        ls == LiveState::Recording ? m5::RecLed::Recording
                        : ls == LiveState::Armed   ? m5::RecLed::Standby
                                                   : m5::RecLed::Off,
                        st.focused, ctx.nowMs);
        return true;
    }

    // --- StripOwner --------------------------------------------------------------------------------------------------
    void stripColumn(int column0, ParamColumnModel& c) const override
    {
        if (c.spec == nullptr)
            return;
        switch (column0)
        {
        case 0:
            c.subReadout = m1::trackLabel(*shell_, "sampling.source", 1); // SOURCE follows SAMPLING/EDIT's
            c.stepped = true;
            break;
        case 1:
        {
            const int k = std::clamp(static_cast<int>(std::lround(c.value)), 0, 5);
            static constexpr std::string_view kText[] = {"1/4 BAR", "1/2 BAR", "1 BAR", "2 BARS", "4 BARS", "8 BARS"};
            c.valueText = kText[k];
            break;
        }
        case 3:
        {
            const double sr = hub() != nullptr ? hub()->sampleRate : 44100.0;
            const double ms = static_cast<double>(c.value) / sr * 1000.0;
            nudge_.format("%+.0f ms", ms);
            c.valueText = nudge_.view();
            break;
        }
        default:
            break;
        }
    }
    long long stripSteps(int column0, const core::ParamSpec& spec, bool fine) const override
    {
        if (column0 == 0)
            return 0; // the target track, one by one
        return column0 == 3 ? EncoderAccel::stepsOf(spec, fine) : -1; // NUDGE
    }
    bool stripTurn(int column0, const core::ParamSpec& spec, int detents, bool fine) override
    {
        if (column0 == 0)
        {
            set("live.track", static_cast<float>(std::clamp(target() + detents, 0, 15)));
            return true;
        }
        if (column0 != 3)
            return false;
        const float v = spec.clamp(get("live.nudge") + static_cast<float>(detents) * (fine ? spec.fineStep : spec.coarseStep));
        set("live.nudge", v);
        if (auto* h = m5::SamplingHost::current())
            h->setNudge(static_cast<std::int32_t>(v)); // SMP-024: the window slides, the loop follows
        return true;
    }

private:
    float get(std::string_view id) const noexcept { return shell_ != nullptr ? m1::trackParam(*shell_, id, 1) : 0.0f; }
    void set(std::string_view id, float v) const noexcept
    {
        if (shell_ != nullptr)
            m1::setTrackParam(*shell_, id, 1, v);
    }
    void toggleRec()
    {
        auto* h = m5::SamplingHost::current();
        if (h == nullptr)
            return;
        const auto ls = liveState();
        if (ls == LiveState::Armed || ls == LiveState::Recording)
            h->liveDisarm(scene(), target());
        else if (! h->liveArm(scene(), target()))
            toast("REC refused: no memory for the capture ring");
    }
    TakeStripModel takeModel(const LiveSamplingFeed& f) const
    {
        TakeStripModel m;
        m.colour = f.colour;
        const auto* hb = hub();
        if (hb == nullptr || f.takeStart < 0 || f.takeLength <= 0)
            return m;
        const double window = 8.0 * 4.0 * 60.0 / f.bpm * f.sampleRate;
        const auto now = ring_.nowFrame();
        m.hasTake = true;
        m.take0 = 1.0 - static_cast<double>(now - f.takeStart) / window;
        m.take1 = 1.0 - static_cast<double>(now - f.takeStart - f.takeLength) / window;
        const auto nudge = hb->status.liveNudge.load(std::memory_order_relaxed);
        m.nudge = static_cast<double>(nudge) / window;
        caption_.format("NUDGE %+.0f ms", static_cast<double>(nudge) / f.sampleRate * 1000.0);
        m.caption = caption_.view();
        return m;
    }
    void toast(std::string text)
    {
        ToastRequest t;
        t.text = std::move(text);
        shell_->showToast(std::move(t));
    }

    ShellApi* shell_;
    UiState* state_;
    mutable RingView ring_;
    mutable m5::InputLevel input_ {};
    mutable FixedText<24> nudge_, caption_;
};

std::unique_ptr<Widget> makeLiveSampling(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<LiveSampling>(a);
}

constexpr RailItemSpec kRail[] = {{RailItemSpec::Kind::Toggle, "REC"},
                                  {RailItemSpec::Kind::Toggle, "SOFT LIMITER", {}, "mixer.softLimiter"},
                                  {RailItemSpec::Kind::Button, "UNLOAD"},
                                  {RailItemSpec::Kind::Button, "SAVE"}};
} // namespace

// Tests and benches: the LIVE SAMPLING body's ring view (null = not LIVE SAMPLING).
const RingView* liveRingOf(const Widget* body) noexcept
{
    const auto* b = dynamic_cast<const LiveSampling*>(body);
    return b != nullptr ? &b->ring() : nullptr;
}
} // namespace ui

APP_REGISTER_WIDGET("RingView", &ui::makeLiveSampling)
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "sampling.live",
                                     .title = "LIVE SAMPLING",
                                     .parent = "track.menu",
                                     .body = ui::BodyTemplate::RailMeterContent,
                                     .contextBar = ui::ContextBarKind::TrackContext,
                                     .rail = ui::kRail,
                                     .bodyComponent = "RingView",
                                     .strip = {"LIVE SAMPLING",
                                               {"live.track", "live.length", "live.loopAfterRec", "live.nudge",
                                                "live.saveTo", "routing.monitorLevel"}}}))
