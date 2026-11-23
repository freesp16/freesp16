// SAMPLING/EDIT (07-ui-design/screens/sampling.md, UID-775…784; 03-ui/screens.md UI-320…324; SMP-001…010).
// Screen id `sampling.edit` (the id TRACK MENU's SAMPLING/EDIT action already opens), parent TRACK MENU, template
// rail + meter + content (the INPUT meter at x 224 between the rail and the editor), context bar trackContext with
// SCENE | PATTERN greyed (switching is blocked while the screen is open: sampling.screenOpen, SMP-008).
//   - Rail: REC (Rec Toggle: off / standby blinking / recording; the status-row REC mirrors it), SOFT LIMITER
//     (mixer.softLimiter, LED warn), EDIT (the EDIT list), SAVE CHANGES (SAVE SELECTION with the whole selection) —
//     EDIT and SAVE CHANGES are disabled with no sample and while recording (UID-782). The REC key does what the REC
//     item does while the screen is open (the transport leaves it to the screen).
//   - Editor: recording — the take grows left to right in the track colour (incremental: only the ring frames written
//     since the last frame are folded into the 280 column buckets), a 2 lpx `rec` write head, the elapsed chip
//     `● REC 0:02.140 / 2 BARS`, with REC LENGTH the editor spans that length and its end is marked `rec` 60 %; standby —
//     the chip `WAITING FOR THRESHOLD` / `WAITING FOR PLAY` / `STARTS IN 3 BEATS` (REC QUANTIZE, the engine's
//     countdown); afterwards the track's take in the PLAYBACK editor with the S / E flags (SELECTION START / END);
//     empty — `Press REC or hit a pad to record` (UID-783).
//   - Knobs SOURCE, REC THRESHOLD (a warn line on the INPUT meter; inactive while REC QUANTIZE is on, which replaces
//     it), REC LENGTH, REC QUANTIZE (ours: OFF, 1 / 2 / 4 / 8 / 16 beats), SELECTION START, SELECTION END (the take's
//     playback.start / playback.start + length: sampling.selStart / selEnd are focus-bound values, see
//     core/params/catalogue/samplingui.cpp). MONITOR LEVEL left the strip for REC QUANTIZE (it is on LIVE SAMPLING and
//     OUTPUT SETTING). Every column is disabled while recording (UI-320).
//   - Pads (ours; PLAY and TRACK pad mode, no SHIFT): a hit records into that pad's track like REC does — at the next
//     REC QUANTIZE boundary, at once when it is OFF. The same pad again during the take (standby or recording) drops
//     it: nothing lands on the track, the part file is deleted. During a take other pads keep the take's track
//     selected; REC still stops a take and keeps it. SHIFT + pad only selects (PAD-004).
//   - EDIT list: BAR, BPM (each follows the other, REN-021), SAVE SELECTION ›, NORMALIZE ›, FADE IN ›, FADE OUT ›.
//   - SAVE SELECTION dialog: `Save selection as:` with a numbered name, CANCEL · SAVE & REPLACE (primary) · SAVE.
//   - The track box reads `RECORDED TEMP.` until the take is saved (the model's display name, SMP-010); leaving with an
//     unsaved take asks `Discard the recording?` CANCEL / DISCARD / SAVE… (UID-784, LeaveGuard).
//   - Rotary: the rail (the editor keeps no rotary focus); SHIFT + rotary zooms about the last-touched flag.
// While the screen is open the input monitor is on (routing.monitor, MIX-050) and off again when it closes.
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/playback/SampleViews.h"
#include "ui/screens/project/ProjectSource.h"
#include "ui/screens/sampling/SamplingHost.h"
#include "ui/screens/sampling/SamplingParts.h"
#include "ui/shell/LeaveGuard.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/StripOwner.h"
#include "ui/shell/UiState.h"
#include "ui/widgets/Progress.h"
#include "ui/widgets/SettingPopup.h"
#include "ui/widgets/WaveformEditor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui
{
namespace m5
{
namespace
{
SamplingHost*& installedHost() noexcept
{
    static SamplingHost* host = nullptr;
    return host;
}
} // namespace
SamplingHost* SamplingHost::current() noexcept
{
    return installedHost();
}
void SamplingHost::install(SamplingHost* h) noexcept
{
    installedHost() = h;
}
} // namespace m5

namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
using core::sampling::RecState;

constexpr RectF kEditor {328, 152, 856, 360}, kOverview {328, 528, 856, 64};
constexpr int kRailRec = 0, kRailLimiter = 1, kRailEdit = 2, kRailSave = 3;
constexpr int kColThreshold = 1, kColQuantize = 3, kColSelStart = 4, kColSelEnd = 5; // the knob strip
constexpr int kColumns = 280; // 840 lpx at a 3 lpx pitch
constexpr float kPlotX = 336, kPlotW = 840;

int asInt(float v) noexcept
{
    return static_cast<int>(std::lround(v));
}

class SamplingEdit final : public Widget, public RailOwner, public StripOwner, public LeaveGuard
{
public:
    explicit SamplingEdit(const WidgetArgs& a) : shell_(a.shell), state_(a.state)
    {
        setParam("sampling.screenOpen", 1); // SMP-008 / SCN-051: scene switching blocked (engine, MIDI)
        setParam("routing.monitor", 1);     // the input monitor follows the screen
        setParam("midi.remoteSelectBlocked", 1); // SCN-051: remote CC 0/32 selection ignored
    }
    ~SamplingEdit() override
    {
        if (auto* h = m5::SamplingHost::current(); h != nullptr && recording())
            h->stop(scene(), track()); // leaving stops a take in progress
        setParam("sampling.screenOpen", 0);
        setParam("routing.monitor", 0);
        setParam("midi.remoteSelectBlocked", 0);
    }
    RectF bounds() const noexcept override { return {224, L::contentTop, 1040, L::contentH}; }

    // --- state ---------------------------------------------------------------------------------------------------------
    int scene() const noexcept { return state_ != nullptr ? (state_->scene - 1) & 15 : 0; }
    int track() const noexcept { return state_ != nullptr ? (state_->selectedTrack - 1) & 15 : 0; }
    const core::sampling::CaptureHub* hub() const noexcept
    {
        const auto* h = m5::SamplingHost::current();
        return h != nullptr ? h->hub() : nullptr;
    }
    RecState recState() const noexcept
    {
        const auto* hb = hub();
        if (hb == nullptr)
            return RecState::Idle;
        const auto& s = hb->status;
        if (s.takeTrack.load(std::memory_order_relaxed) != track() ||
            s.takeScene.load(std::memory_order_relaxed) != scene())
            return RecState::Idle;
        return static_cast<RecState>(s.state.load(std::memory_order_acquire));
    }
    bool recording() const noexcept
    {
        const auto s = recState();
        return s == RecState::Standby || s == RecState::Recording;
    }
    m1::SampleView sample() const { return m1::SampleViews::instance().of(track() + 1); }
    bool hasSample() const { return ! sample().empty(); }
    int filledColumns() const noexcept { return filled_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        state_->sceneLocked = true; // UID-776
        const auto rs = recState();
        state_->samplerRec = rs == RecState::Recording || (rs == RecState::Standby && std::fmod(ctx.nowMs, 1000.0) < 500.0);
        input_.update(hub(), ctx.nowMs);
        m5::drawInputMeter(dl, ctx, input_, quantized() ? -120.0f : m5::thresholdDb(param("sampling.threshold")));
        if (rs == RecState::Standby || rs == RecState::Recording)
        {
            grow(rs);
            drawRecording(dl, ctx, rs);
            WaveformEditor::drawOverview(dl, ctx, kOverview, WaveformModel {});
            WaveformEditor::drawZoomGauge(dl, ctx, WaveformEditor::kGaugeX, WaveformEditor::kGaugeY, 0);
            return;
        }
        const auto m = editorModel();
        WaveformEditor::drawEditor(dl, ctx, kEditor, m);
        WaveformEditor::drawOverview(dl, ctx, kOverview, m);
        WaveformEditor::drawZoomGauge(dl, ctx, WaveformEditor::kGaugeX, WaveformEditor::kGaugeY, zoomLevel(m));
        if (m.overview == nullptr)
            dl.text(kEditor.x + kEditor.w / 2, kEditor.y + kEditor.h / 2, Align::Centre, TextStyleId::Body,
                    ctx.palette.text2, "Press REC or hit a pad to record");
    }

    bool dirty(double) const override { return recording() || hub() != nullptr; }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (! state_->shift)
                return false; // the rail takes the rotary
            zoom(e.value);
            return true;
        }
        if (e.control == io::Control::Rec && e.kind == Kind::Press)
        {
            toggleRec();
            return true;
        }
        if (io::isPad(e.control) && e.kind == Kind::Press)
            return padHit(io::indexOf(e.control) - 1);
        if (touch::isTouch(e))
            return touchAt(e);
        return false;
    }

    // --- RailOwner ---------------------------------------------------------------------------------------------------
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (shell_ == nullptr)
            return true;
        switch (index)
        {
        case kRailRec:
            toggleRec();
            return true;
        case kRailEdit:
            if (! recording() && hasSample())
                editList();
            return true;
        case kRailSave:
            if (! recording() && hasSample())
                saveDialog();
            return true;
        default:
            return false; // SOFT LIMITER: the shell's toggle of mixer.softLimiter
        }
    }
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        if (index == kRailLimiter)
            st.onColour = theme::palette(state_ == nullptr || state_->dark).warn;
        else if (index == kRailEdit || index == kRailSave)
            st.disabled = recording() || ! hasSample();
    }
    bool railDraw(int index, const RailItemSpec&, const RailItemState& st, DrawList& dl,
                  const UiContext& ctx) const override
    {
        if (index != kRailRec)
            return false;
        const auto rs = recState();
        m5::drawRecItem(dl, ctx, L::railItem(0),
                        rs == RecState::Recording ? m5::RecLed::Recording
                        : rs == RecState::Standby ? m5::RecLed::Standby
                                                  : m5::RecLed::Off,
                        st.focused, ctx.nowMs);
        return true;
    }

    // --- StripOwner --------------------------------------------------------------------------------------------------
    void stripColumn(int column0, ParamColumnModel& c) const override
    {
        if (c.spec == nullptr)
            return;
        const bool rec = recording();
        if (rec)
            c.disabled = true; // UI-320
        if (column0 == kColThreshold && quantized())
        {
            c.inactive = true; // REC QUANTIZE starts the take, not the level
            c.subReadout = "QUANTIZE ON";
        }
        if (column0 == kColQuantize)
        {
            static constexpr std::string_view kBars[] = {"AT ONCE", "", "", "= 1 BAR", "= 2 BARS", "= 4 BARS"};
            c.subReadout = kBars[std::clamp(asInt(c.value), 0, 5)];
        }
        if (column0 == kColSelStart || column0 == kColSelEnd)
        {
            const bool first = column0 == kColSelStart;
            const auto s = sample();
            std::int64_t start = 0, end = 0;
            selection(start, end);
            auto& t = text_[first ? 0U : 1U];
            if (s.empty() || (rec && ! first))
                t.set(first ? "0" : "\xE2\x80\x94"); // —
            else
                t.format("%lld", static_cast<long long>(first ? start : end));
            c.valueText = t.view();
            c.value = s.empty() ? 0.0f
                                : static_cast<float>(static_cast<double>(first ? start : end) /
                                                     static_cast<double>(s.frames) * static_cast<double>(c.spec->maxValue));
        }
    }
    long long stripSteps(int column0, const core::ParamSpec&, bool fine) const override
    {
        if (column0 != 3 && column0 != 4)
            return -1;
        if (fine)
            return 0;
        // START / END walk one editor column per detent: the zoomed view's columns across the whole sample.
        return static_cast<long long>(zoomX_ * kColumns);
    }
    bool stripTurn(int column0, const core::ParamSpec&, int detents, bool fine) override
    {
        if (column0 != kColSelStart && column0 != kColSelEnd)
            return false;
        if (recording())
            return true;
        const auto s = sample();
        if (s.empty())
            return true;
        std::int64_t start = 0, end = 0;
        selection(start, end);
        // One editor column per detent (zoom-aware), one frame with SHIFT.
        const auto step = fine ? std::int64_t {1}
                               : std::max<std::int64_t>(1, static_cast<std::int64_t>(
                                                               static_cast<double>(s.frames) / zoomX_ / kColumns));
        if (column0 == kColSelStart)
            start = std::clamp<std::int64_t>(start + detents * step, 0, end - 1);
        else
            end = std::clamp<std::int64_t>(end + detents * step, start + 1, s.frames);
        active_ = column0 == kColSelStart ? 0 : 2;
        setSelection(start, end);
        return true;
    }

    // --- LeaveGuard (UID-784) ----------------------------------------------------------------------------------------
    bool mayLeave(std::string_view target) override
    {
        auto* h = m5::SamplingHost::current();
        if (leaving_ || h == nullptr || recording() || ! h->unsaved(scene(), track()))
            return true;
        std::string to(target);
        MessageRequest req;
        req.icon = glyph::Kind::Caution;
        req.title = "Unsaved recording";
        req.lines = {"Discard the recording?"};
        req.buttons = {{"CANCEL", ButtonStyle::Plain}, {"DISCARD", ButtonStyle::Destructive}, {"SAVE\xE2\x80\xA6", ButtonStyle::Primary}};
        req.defaultButton = 0;
        req.onResult = [this, to](int b)
        {
            if (b == 1)
            {
                if (auto* host = m5::SamplingHost::current())
                    host->discard(scene(), track());
                leaving_ = true;
                if (to.empty())
                    shell_->back();
                else if (to == "home")
                    shell_->home();
                else
                    shell_->open(to);
            }
            else if (b == 2)
                saveDialog();
        };
        shell_->showMessage(std::move(req));
        return false;
    }

private:
    float param(std::string_view id) const noexcept
    {
        return shell_ != nullptr ? m1::trackParam(*shell_, id, track() + 1) : 0.0f;
    }
    void setParam(std::string_view id, float v) const noexcept
    {
        if (shell_ != nullptr)
            m1::setTrackParam(*shell_, id, track() + 1, v);
    }
    void selection(std::int64_t& start, std::int64_t& end) const
    {
        const auto s = sample();
        start = std::clamp<std::int64_t>(static_cast<std::int64_t>(param("playback.start")), 0, std::max<std::int64_t>(0, s.frames - 1));
        const auto len = static_cast<std::int64_t>(param("playback.length"));
        end = len > 0 ? std::min<std::int64_t>(s.frames, start + len) : s.frames;
        if (end <= start)
            end = s.frames;
    }
    void setSelection(std::int64_t start, std::int64_t end)
    {
        setParam("playback.start", static_cast<float>(start));
        setParam("playback.loopStart", static_cast<float>(start));
        setParam("playback.length", static_cast<float>(end - start));
        setParam("sampling.selStart", static_cast<float>(start));
        setParam("sampling.selEnd", static_cast<float>(end));
    }

    WaveformModel editorModel() const
    {
        WaveformModel m;
        view_ = sample();
        m.colour = theme::trackColour(m1::padColour(*shell_, track() + 1));
        m.overview = view_.empty() ? nullptr : view_.overview;
        m.frames = view_.frames;
        m.gridCount = 0;
        std::int64_t start = 0, end = 0;
        if (! view_.empty())
            selection(start, end);
        m.start = m.loopStart = start;
        m.end = end;
        m.activeCursor = -1;
        m.zoomX = zoomX_;
        m.zoomCentre = centre_;
        return m;
    }
    float zoomLevel(const WaveformModel& m) const noexcept
    {
        const double maxZoom = std::max(2.0, static_cast<double>(m.frames) / 840.0);
        return static_cast<float>(std::log2(zoomX_) / std::log2(maxZoom));
    }
    void zoom(int detents)
    {
        const auto m = editorModel();
        if (m.frames <= 0)
            return;
        const double maxZoom = std::max(1.0, static_cast<double>(m.frames) / 840.0);
        zoomX_ = std::clamp(zoomX_ * std::pow(2.0, detents / 2.0), 1.0, maxZoom);
        centre_ = static_cast<double>(active_ == 2 ? m.end : m.start) / static_cast<double>(m.frames);
    }

    bool quantized() const noexcept { return asInt(param("sampling.quantize")) > 0; }
    // The take in standby or recording, whatever track is selected (−1 = none).
    int busyTrack() const noexcept
    {
        const auto* hb = hub();
        if (hb == nullptr)
            return -1;
        const auto& s = hb->status;
        const auto st = static_cast<RecState>(s.state.load(std::memory_order_acquire));
        if ((st != RecState::Standby && st != RecState::Recording) ||
            s.takeScene.load(std::memory_order_relaxed) != scene())
            return -1;
        return s.takeTrack.load(std::memory_order_relaxed) & 15;
    }
    // A pad hit (PLAY / TRACK pad mode; the pad controller already selected its track). SHIFT + pad only selects.
    bool padHit(int pad)
    {
        auto* h = m5::SamplingHost::current();
        if (h == nullptr || state_->shift)
            return false;
        const int busy = busyTrack();
        if (busy == pad)
        {
            h->drop(scene(), pad);
            toast("Take dropped");
            return true;
        }
        if (busy >= 0)
        {
            shell_->selectTrack(busy + 1); // a take keeps its track on screen; REC or its own pad ends it
            return true;
        }
        if (track() != pad)
            shell_->selectTrack(pad + 1);
        if (! h->record(scene(), pad))
            toast("REC refused: no memory for the capture ring");
        return true;
    }
    void toggleRec()
    {
        auto* h = m5::SamplingHost::current();
        if (h == nullptr)
            return;
        if (recording())
            h->stop(scene(), track());
        else if (! h->record(scene(), track()))
        {
            ToastRequest t;
            t.text = "REC refused: no memory for the capture ring";
            shell_->showToast(std::move(t));
        }
    }

    // The growing take (incremental): the ring frames written since the last frame are folded into the column buckets.
    double spanFrames(const core::sampling::CaptureHub& hb, std::int64_t elapsed) const noexcept
    {
        const auto& s = hb.status;
        const int bars = s.takeBars.load(std::memory_order_relaxed);
        const double bpm = std::max(20.0f, s.takeBpm.load(std::memory_order_relaxed));
        if (bars > 0)
            return bars * 4.0 * 60.0 / bpm * hb.sampleRate;
        // REC LENGTH OFF: the editor spans 4 s, then doubles as the take grows (the buckets merge pairwise).
        double span = 4.0 * hb.sampleRate;
        while (static_cast<double>(elapsed) > span)
            span *= 2;
        return span;
    }
    void grow(RecState rs) const
    {
        const auto* hb = hub();
        if (hb == nullptr)
            return;
        const auto& s = hb->status;
        const auto seq = s.takeSeq.load(std::memory_order_relaxed);
        const auto start = s.takeStart.load(std::memory_order_acquire);
        if (seq != seq_ || rs == RecState::Standby || start < 0)
        {
            seq_ = seq;
            filled_ = 0;
            folded_ = -1;
            lo_.fill(0);
            hi_.fill(0);
            span_ = 0;
            if (rs == RecState::Standby || start < 0)
                return;
        }
        const auto head = hb->ring.head();
        const auto endFrame = s.takeEnd.load(std::memory_order_relaxed);
        const auto to = endFrame >= 0 ? std::min(head, endFrame) : head;
        const double span = spanFrames(*hb, to - start);
        if (span_ > 0 && span > span_) // the editor doubled: merge the buckets pairwise
            for (int i = 0; i < kColumns; ++i)
            {
                const int a = 2 * i, b = 2 * i + 1;
                lo_[static_cast<std::size_t>(i)] = a < kColumns ? std::min(lo_[static_cast<std::size_t>(a)], b < kColumns ? lo_[static_cast<std::size_t>(b)] : 0.0f) : 0.0f;
                hi_[static_cast<std::size_t>(i)] = a < kColumns ? std::max(hi_[static_cast<std::size_t>(a)], b < kColumns ? hi_[static_cast<std::size_t>(b)] : 0.0f) : 0.0f;
            }
        span_ = span;
        const double fpc = span / kColumns;
        std::int64_t from = std::max<std::int64_t>({folded_ < 0 ? start : folded_, hb->ring.oldest(), start});
        while (from < to)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(l_.size()), to - from));
            if (! hb->ring.read(from, n, l_.data(), r_.data()))
                break;
            for (int k = 0; k < n; ++k)
            {
                const int col = static_cast<int>(static_cast<double>(from + k - start) / fpc);
                if (col < 0 || col >= kColumns)
                    continue;
                auto& lo = lo_[static_cast<std::size_t>(col)];
                auto& hi = hi_[static_cast<std::size_t>(col)];
                lo = std::min({lo, l_[static_cast<std::size_t>(k)], r_[static_cast<std::size_t>(k)]});
                hi = std::max({hi, l_[static_cast<std::size_t>(k)], r_[static_cast<std::size_t>(k)]});
            }
            from += n;
        }
        folded_ = from;
        filled_ = std::clamp(static_cast<int>(static_cast<double>(from - start) / fpc), 0, kColumns);
    }
    void drawRecording(DrawList& dl, const UiContext& ctx, RecState rs) const
    {
        const auto& p = ctx.palette;
        const auto x = kEditor.x, y = kEditor.y, w = kEditor.w, h = kEditor.h;
        dl.fill(kEditor, p.surface1, tk::radiusM);
        for (int b = 1; b < 8; ++b)
        {
            const float bx = kPlotX + kPlotW * static_cast<float>(b) / 8;
            dl.fill({bx - 0.5f, y + 8, 1, h - 16}, b == 4 ? p.borderStrong : p.border);
        }
        const auto* hb = hub();
        const auto colour = theme::trackColour(m1::padColour(*shell_, track() + 1));
        const int bars = hb != nullptr ? hb->status.takeBars.load(std::memory_order_relaxed) : 0;
        if (rs == RecState::Recording && hb != nullptr && span_ > 0)
        {
            const float cy = y + 32 + (h - 64) / 2, half = (h - 64) / 2 * 0.92f;
            for (int i = 0; i < filled_; ++i)
            {
                const float xx = kPlotX + 3.0f * static_cast<float>(i) + 1.5f;
                const float top = cy - std::min(1.0f, hi_[static_cast<std::size_t>(i)]) * half;
                const float bottom = cy - std::max(-1.0f, lo_[static_cast<std::size_t>(i)]) * half;
                dl.fill({xx - 0.9f, top, 1.8f, std::max(1.0f, bottom - top)}, colour);
            }
            draw::hline(dl, kPlotX, kPlotX + 3.0f * static_cast<float>(filled_), cy, p.border);
            const auto start = hb->status.takeStart.load(std::memory_order_relaxed);
            const auto elapsed = std::max<std::int64_t>(0, hb->ring.head() - start);
            const float hx = kPlotX + kPlotW * static_cast<float>(std::min(1.0, static_cast<double>(elapsed) / span_));
            dl.fill({hx - 1, y + 8, 2, h - 16}, p.rec); // the write head
            const double sec = static_cast<double>(elapsed) / hb->sampleRate;
            const int minutes = static_cast<int>(sec / 60);
            if (bars > 0)
                chip_.format("\xE2\x97\x8F REC  %d:%06.3f / %d BAR%s", minutes, sec - minutes * 60, bars, bars == 1 ? "" : "S");
            else
                chip_.format("\xE2\x97\x8F REC  %d:%06.3f", minutes, sec - minutes * 60);
        }
        else if (hb != nullptr && hb->status.takeQuantize.load(std::memory_order_relaxed) > 0)
        {
            // REC QUANTIZE: the engine's countdown to the boundary, in whole beats (stopped: the take starts at PLAY).
            const float left = hb->status.startInBeats.load(std::memory_order_relaxed);
            const int n = std::max(1, static_cast<int>(std::ceil(left - 1e-3f)));
            if (left < 0.0f)
                chip_.set("WAITING FOR PLAY");
            else
                chip_.format("STARTS IN %d BEAT%s", n, n == 1 ? "" : "S");
        }
        else
        {
            const int threshold = asInt(param("sampling.threshold"));
            chip_.set(threshold >= 42 ? "WAITING FOR PLAY" : "WAITING FOR THRESHOLD");
        }
        hint_.format("PAD %d AGAIN DROPS THE TAKE \xC2\xB7 REC STOPS IT", track() + 1);
        dl.text(x + 16, y + h - 20, Align::Left, TextStyleId::Caption, p.text2, hint_.view());
        if (bars > 0)
        {
            dl.fill({x + w - 12, y + 8, 4, h - 16}, theme::withOpacity(p.rec, 0.6f));
            end_.format("END \xC2\xB7 REC LENGTH %d BAR%s", bars, bars == 1 ? "" : "S");
            dl.text(x + w - 16, y + h - 20, Align::Right, TextStyleId::Caption, p.text2, end_.view());
        }
        dl.fill({x + 16, y + 16, 248, 32}, p.surface3, tk::radiusS);
        dl.text(x + 32, y + 32, Align::Left, TextStyleId::Label, p.rec, chip_.view());
    }

    // EDIT list (sampling.md "EDIT list popup"; UID-781).
    void editList()
    {
        auto* h = m5::SamplingHost::current();
        const auto s = sample();
        if (h == nullptr || s.empty())
            return;
        double bars = 0, bpm = 0;
        const double seconds = static_cast<double>(s.frames) / s.sampleRate;
        if (! h->tempo(scene(), track(), bars, bpm) || bars <= 0)
        {
            bpm = bpm > 0 ? bpm : 120.0;
            bars = std::max(1.0, std::round(bpm * seconds / 240.0));
        }
        const auto& params = shell_->params();
        SettingRequest req;
        req.title = "EDIT";
        SettingRow bar, tempo;
        bar.label = "BAR";
        bar.spec = params.find("sampling.editBars");
        bar.value = static_cast<float>(std::clamp(std::round(bars), 1.0, 64.0));
        tempo.label = "BPM";
        tempo.spec = params.find("sampling.editBpm");
        tempo.value = static_cast<float>(std::clamp(bpm, 20.0, 300.0));
        req.rows.push_back(bar);
        req.rows.push_back(tempo);
        for (const auto* label : {"SAVE SELECTION", "NORMALIZE", "FADE IN", "FADE OUT"})
        {
            SettingRow r;
            r.label = label;
            r.options = {"\xE2\x80\xBA"};
            r.action = true;
            req.rows.push_back(std::move(r));
        }
        req.onCommit = [this, seconds](int row, int, float value)
        {
            auto* host = m5::SamplingHost::current();
            if (host == nullptr)
                return;
            switch (row)
            {
            case 0: // BAR → BPM follows (REN-021)
            {
                const double b = std::max(1.0, static_cast<double>(value)), t = 240.0 * b / seconds;
                host->setTempo(scene(), track(), b, t);
                if (editPopup_ != nullptr)
                    editPopup_->setValue(1, static_cast<float>(std::clamp(t, 20.0, 300.0)));
                break;
            }
            case 1: // BPM → BAR follows
            {
                const double t = static_cast<double>(value), b = t * seconds / 240.0;
                host->setTempo(scene(), track(), b, t);
                if (editPopup_ != nullptr)
                    editPopup_->setValue(0, static_cast<float>(std::clamp(std::round(b), 1.0, 64.0)));
                break;
            }
            case 2:
                closeEdit();
                saveDialog();
                break;
            case 3:
                closeEdit();
                normalizeMessage();
                break;
            default:
                closeEdit();
                process(row == 4 ? m5::TakeProcess::FadeIn : m5::TakeProcess::FadeOut);
                break;
            }
        };
        editPopup_ = dynamic_cast<SettingPopup*>(shell_->showPopup(std::make_unique<SettingPopup>(std::move(req))));
        if (editPopup_ == nullptr) // no custom popups: the shell's own (no BAR ⇄ BPM follow-up)
            return;
    }
    void closeEdit()
    {
        if (editPopup_ != nullptr)
            editPopup_->closed = true;
        editPopup_ = nullptr;
    }
    void normalizeMessage()
    {
        MessageRequest req;
        req.icon = glyph::Kind::Info;
        req.title = "NORMALIZE";
        req.lines = {"Raise the selection's peak to \xE2\x88\x92" "1.0 dBFS.", "The take is saved as a new file."};
        req.buttons = {{"CANCEL", ButtonStyle::Plain}, {"APPLY", ButtonStyle::Primary}};
        req.onResult = [this](int b)
        {
            if (b == 1)
                process(m5::TakeProcess::Normalize);
        };
        shell_->showMessage(std::move(req));
    }
    void process(m5::TakeProcess kind)
    {
        auto* h = m5::SamplingHost::current();
        if (h == nullptr)
            return;
        std::int64_t start = 0, end = 0;
        selection(start, end);
        h->process(scene(), track(), kind, start, end,
                   [this](bool ok, std::string message) { toast(ok ? message : "Failed: " + message); });
    }

    // SAVE SELECTION (UID-780; SMP-009): CANCEL · SAVE & REPLACE (primary) · SAVE.
    void saveDialog()
    {
        auto* h = m5::SamplingHost::current();
        if (h == nullptr || ! hasSample())
            return;
        DialogRequest req;
        req.title = "SAVE SELECTION";
        req.prompt = "Save selection as:";
        req.text = h->proposedName(scene(), track());
        req.buttons = {{"CANCEL", ButtonStyle::Plain}, {"SAVE & REPLACE", ButtonStyle::Primary}, {"SAVE", ButtonStyle::Plain}};
        req.primary = 1;
        req.cancel = 0;
        req.validate = [](std::string_view name)
        {
            const auto why = m2::validateProjectName(name); // the same file-name rules (PRJ-004)
            return why ? std::string(*why) : std::string {};
        };
        req.onResult = [this](int button, std::string_view text)
        {
            if (button != 1 && button != 2)
                return;
            auto* host = m5::SamplingHost::current();
            if (host == nullptr)
                return;
            std::int64_t start = 0, end = 0;
            selection(start, end);
            ProgressRequest pr;
            pr.title = "SAVING";
            pr.detail = std::string(text);
            auto* progress = shell_->showProgress(std::move(pr));
            leaving_ = false;
            host->saveSelection(scene(), track(), start, end, std::string(text), button == 1,
                                [this, progress](bool ok, std::string message)
                                {
                                    if (progress != nullptr)
                                        progress->done();
                                    toast(ok ? std::string("Saved to Recordings/") : "Save failed: " + message);
                                });
        };
        shell_->showDialog(std::move(req));
    }
    void toast(std::string text)
    {
        ToastRequest t;
        t.text = std::move(text);
        shell_->showToast(std::move(t));
    }

    bool touchAt(const io::ControlEvent& e)
    {
        const float x = touch::x(e), y = touch::y(e);
        if (e.value == touch::kUp)
        {
            drag_ = -1;
            return true;
        }
        const auto m = editorModel();
        const RectF plot {kPlotX, kEditor.y, kPlotW, kEditor.h};
        if (e.value == touch::kDrag && drag_ >= 0 && m.frames > 0 && ! recording())
        {
            std::int64_t start = 0, end = 0;
            selection(start, end);
            const auto f = WaveformEditor::frameAt(m, plot, x);
            if (drag_ == 0)
                start = std::clamp<std::int64_t>(f, 0, end - 1);
            else
                end = std::clamp<std::int64_t>(f, start + 1, m.frames);
            setSelection(start, end);
            return true;
        }
        if (e.value != touch::kDown || ! contains(kEditor, x, y) || m.frames <= 0 || recording())
            return contains(kEditor, x, y);
        const float xs = WaveformEditor::xOf(m, plot, m.start), xe = WaveformEditor::xOf(m, plot, m.end);
        const bool top = y < kEditor.y + kEditor.h / 2;
        if (top && std::fabs(x - (xs + 12)) < 28)
            drag_ = active_ = 0;
        else if (! top && std::fabs(x - (xe - 12)) < 28)
        {
            drag_ = 1;
            active_ = 2;
        }
        return true;
    }

    ShellApi* shell_;
    UiState* state_;
    double zoomX_ = 1, centre_ = 0.5;
    int drag_ = -1, active_ = 0;
    bool leaving_ = false;
    SettingPopup* editPopup_ = nullptr;
    mutable m5::InputLevel input_ {};
    mutable m1::SampleView view_ {};
    mutable std::array<FixedText<24>, 2> text_ {};
    mutable FixedText<48> chip_, end_, hint_;
    // the growing take
    mutable std::array<float, kColumns> lo_ {}, hi_ {};
    mutable std::array<float, 1024> l_ {}, r_ {};
    mutable std::uint32_t seq_ = 0;
    mutable std::int64_t folded_ = -1;
    mutable double span_ = 0;
    mutable int filled_ = 0;
};

std::unique_ptr<Widget> makeSamplingEdit(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<SamplingEdit>(a);
}

constexpr RailItemSpec kRail[] = {{RailItemSpec::Kind::Toggle, "REC"},
                                  {RailItemSpec::Kind::Toggle, "SOFT LIMITER", {}, "mixer.softLimiter"},
                                  {RailItemSpec::Kind::Button, "EDIT"},
                                  {RailItemSpec::Kind::Button, "SAVE CHANGES"}};
} // namespace

// Tests: the columns the growing take has filled (−1 = not SAMPLING/EDIT).
int samplingFilledColumns(const Widget* body) noexcept
{
    const auto* b = dynamic_cast<const SamplingEdit*>(body);
    return b != nullptr ? b->filledColumns() : -1;
}
} // namespace ui

APP_REGISTER_WIDGET("SamplingEditor", &ui::makeSamplingEdit)
APP_REGISTER_SCREEN((ui::ScreenSpec {.id = "sampling.edit",
                                     .title = "SAMPLING/EDIT",
                                     .parent = "track.menu",
                                     .body = ui::BodyTemplate::RailMeterContent,
                                     .contextBar = ui::ContextBarKind::TrackContext,
                                     .rail = ui::kRail,
                                     .bodyComponent = "SamplingEditor",
                                     .strip = {"SAMPLING/EDIT",
                                               {"sampling.source", "sampling.threshold", "sampling.length",
                                                "sampling.quantize", "sampling.selStart", "sampling.selEnd"}}}))
