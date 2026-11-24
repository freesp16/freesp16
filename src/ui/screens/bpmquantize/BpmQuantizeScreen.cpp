// BPM/QUANTIZE (07-ui-design/screens/bpm-quantize.md; UI-291, UID-625…634): a panel Modal (1104 × 640 at 88, 64) over
// the dimmed screen, opened by the BPM | QUANTIZE context item (the shell opens the id `bpmQuantize`). Three rows under
// hairline captions: TAP · BPM · SWING (BigValueFields); AUDIO INPUT BPM (drawn disabled, P2/P3) · NUDGE ◀ RESET ▶ ·
// metronome toggle with its LED; QUANTIZE as one Segmented row of the record-quantise list; the hint line.
//   - The BPM field has the focus on open (UID-626); the rotary edits the focused field (BPM ±1, SHIFT ±0.1; SWING
//     ±1 %; QUANTIZE one segment per detent), push moves the focus BPM → SWING → QUANTIZE (UID-631); SHIFT + push
//     toggles the metronome (the popup opens with SHIFT + ext.tap, nudge has keys). A tap focuses a field; a tap on
//     its stepper's + / − half steps it; a tap on a QUANTIZE segment selects it at once (UID-628, the
//     context item behind shows it at once).
//   - TAP flashes accent-weak for motion.fast per tap; from the second tap the BPM is the average of the last 4
//     intervals (UID-627, SYN-003; 2 s without a tap restarts).
//   - ◀ / ▶ act while held (transport.nudge −1 / +1, 0 on release, SYN-005); RESET stays disabled (the nudge is
//     momentary, no offset remains). An external clock (UiState::syncBadge) disables BPM (with the source badge), TAP
//     and NUDGE (UID-630; the MIDI clock input drives it).
// Values are the ParamSpecs transport.tempo, seq.swing, seq.recordQuantize, transport.metronome, transport.nudge; the
// panel also writes the context bar's BPM / QUANTIZE texts (UiState) until the transport feeds them; the tap average
// is core::musictime::TapTempo.
#include "ui/screens/bpmquantize/BpmQuantizeAccess.h"
#include "core/util/musictime/MusicTime.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/BigValueField.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/Segmented.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
using Kind = io::ControlEvent::Kind;

constexpr RectF kTap{120, 164, 336, 160}, kBpm{472, 164, 336, 160}, kSwing{824, 164, 336, 160},
    kAudio{120, 372, 336, 96}, kSet{328, 388, 112, 64}, kSlower{472, 372, 184, 96}, kReset{664, 372, 184, 96},
    kFaster{856, 372, 184, 96}, kMetronome{1056, 372, 104, 96}, kQuantize{120, 516, 1040, 88};
constexpr std::string_view kQuantizeLabels[] = {"OFF", "1/32", "1/16T", "1/16", "1/8T", "1/8"};

class BpmQuantizePanel final : public Widget
{
public:
    explicit BpmQuantizePanel(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state)
    {
        if (a.params != nullptr)
        {
            tempo_ = a.params->find("transport.tempo");
            swing_ = a.params->find("seq.swing");
            quantize_ = a.params->find("seq.recordQuantize");
            metronome_ = a.params->find("transport.metronome");
            nudge_ = a.params->find("transport.nudge");
        }
    }
    ~BpmQuantizePanel() override { setNudge(0); }
    RectF bounds() const noexcept override { return layout::modalPanel; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        const bool external = state_ != nullptr && ! state_->syncBadge.empty();
        publish();
        // Centred title (bpm-quantize.md "Title bar"): the Modal draws it at the left; repaint the band's text area.
        const auto panel = layout::modalPanel;
        dl.fill({panel.x + 12, panel.y + 8, 360, 40}, p.surface2);
        dl.text(panel.x + panel.w / 2, panel.y + 28, Align::Centre, TextStyleId::Title, p.text1, "BPM/QUANTIZE");

        caption(dl, ctx, 120, 148, 688, "BPM");
        caption(dl, ctx, 824, 148, 336, "SWING");
        const bool flash = ctx.nowMs - tapAtMs_ < tk::motionFastMs;
        Button::draw(dl, ctx, kTap, {"TAP", ButtonStyle::Plain, external, flash});
        if (flash)
            dl.fill(kTap, p.accentWeak, tk::radiusS);
        if (flash)
            dl.text(kTap.x + kTap.w / 2, kTap.y + kTap.h / 2 + 1, Align::Centre, TextStyleId::Label, p.text1, "TAP");

        // Under an external clock the followed tempo (the context bar's text), not the project's stored one.
        const float bpm = external ? std::strtof(state_->bpm.text, nullptr) : value(tempo_, 120.0f);
        const long tenths = std::lround(static_cast<double>(bpm) * 10.0);
        whole_.format("%ld", tenths / 10);
        fraction_.format(".%ld", tenths % 10);
        BigValueModel b{whole_.view(), fraction_.view(), ! external,
                        focus_ == 0,   external,         state_ != nullptr ? state_->syncBadge : std::string_view{}};
        BigValueField::draw(dl, ctx, kBpm, b);
        swingText_.format("%ld", std::lround(value(swing_, 0)));
        BigValueModel sw{swingText_.view(), "%", true, focus_ == 1, false, {}, 176, 8, 6};
        BigValueField::draw(dl, ctx, kSwing, sw);

        caption(dl, ctx, 120, 356, 336, "AUDIO INPUT BPM");
        caption(dl, ctx, 472, 356, 568, "NUDGE");
        dl.fill(kAudio, p.surface2, tk::radiusM);
        dl.text(152, 420, Align::Left, TextStyleId::ValueL, p.text3, "\xE2\x80\x93 \xE2\x80\x93 \xE2\x80\x93");
        Button::draw(dl, ctx, kSet, {"SET", ButtonStyle::Plain, true}); // P2/P3: no audio-input tempo yet (UID-633)
        Button::draw(dl, ctx, kSlower, {"\xE2\x97\x80", ButtonStyle::Plain, external, held_ < 0});
        Button::draw(dl, ctx, kReset, {"RESET", ButtonStyle::Plain, true});
        Button::draw(dl, ctx, kFaster, {"\xE2\x96\xB6", ButtonStyle::Plain, external, held_ > 0});
        // Metronome: LED toggle with the metronome icon (UID-634).
        const bool on = value(metronome_, 0) >= 0.5f;
        dl.fill(kMetronome, p.surface2, tk::radiusS);
        dl.fill({kMetronome.x + 12, kMetronome.y + 8, kMetronome.w - 24, 4}, on ? p.accent : p.borderStrong, 2);
        const float mx = kMetronome.x + 40, my = kMetronome.y + 76;
        glyph::line(dl, mx, my, mx + 8, my - 40, 2, p.text1);
        glyph::line(dl, mx + 8, my - 40, mx + 16, my - 40, 2, p.text1);
        glyph::line(dl, mx + 16, my - 40, mx + 24, my, 2, p.text1);
        glyph::line(dl, mx + 24, my, mx, my, 2, p.text1);
        glyph::line(dl, mx + 12, my - 12, mx + 26, my - 32, 2, p.text1);

        caption(dl, ctx, 120, 500, 1040, "QUANTIZE");
        SegmentedModel q{kQuantizeLabels, static_cast<int>(std::lround(value(quantize_, 3))), 0, {}, p.surface1,
                         focus_ == 2};
        Segmented::draw(dl, ctx, kQuantize, q);
        dl.text(640, 664, Align::Centre, TextStyleId::Body, p.text3,
                "Turn the selector to change the focused value \xC2\xB7 SHIFT + turn for 0.1 BPM \xC2\xB7 BACK closes");
    }
    bool dirty(double nowMs) const override { return nowMs - tapAtMs_ < tk::motionFastMs + 20 || held_ != 0; }

    bool handle(const io::ControlEvent& e, UiContext& ctx) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        const bool external = ! state_->syncBadge.empty();
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            turn(e.value, external);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            if (state_->shift) // SHIFT + push toggles the metronome (its LED toggle is touch-only)
            {
                if (metronome_ != nullptr)
                    shell_->values().set(*metronome_, 0, value(metronome_, 0) >= 0.5f ? 0.0f : 1.0f);
                return true;
            }
            focus_ = (focus_ + 1) % 3; // BPM → SWING → QUANTIZE → BPM
            return true;
        }
        if (e.control == io::Control::ExtTap && e.kind == Kind::Press && ! external)
        {
            tap(e.timeMs);
            return true;
        }
        if ((e.control == io::Control::ExtNudgeDown || e.control == io::Control::ExtNudgeUp) && ! external)
        {
            const int dir = e.control == io::Control::ExtNudgeUp ? 1 : -1;
            setNudge(e.kind == Kind::Press ? dir : 0);
            return true;
        }
        if (! touch::isTouch(e))
            return false;
        const float x = touch::x(e), y = touch::y(e);
        if (e.value == touch::kDrag && dragField_ >= 0) // a vertical drag on BPM / SWING turns it, 8 px per detent
        {
            const int detents = static_cast<int>((dragY_ - y) / (8 * touch::kToCanvas));
            if (detents != 0)
            {
                focus_ = dragField_;
                turn(detents, external);
                dragY_ -= static_cast<float>(detents) * 8 * touch::kToCanvas;
            }
            return true;
        }
        if (e.value == touch::kUp)
        {
            setNudge(0);
            dragField_ = -1;
            return false; // the Modal's own handling (✕) still sees the release
        }
        if (e.value != touch::kDown)
            return false;
        if (contains(kTap, x, y))
        {
            if (! external)
                tap(e.timeMs);
            return true;
        }
        if (contains(kBpm, x, y))
        {
            const BigValueModel m{whole_.view(), fraction_.view(), ! external, focus_ == 0, external};
            if (const int d = BigValueField::stepAt(kBpm, m, x, y); d != 0)
                turn(d, external);
            focus_ = 0;
            dragField_ = 0;
            dragY_ = y;
            return true;
        }
        if (contains(kSwing, x, y))
        {
            const BigValueModel m{swingText_.view(), "%", true, focus_ == 1, false};
            if (const int d = BigValueField::stepAt(kSwing, m, x, y); d != 0)
                turn(d, external);
            focus_ = 1;
            dragField_ = 1;
            dragY_ = y;
            return true;
        }
        if (contains(kQuantize, x, y) && quantize_ != nullptr)
        {
            const int seg = std::clamp(static_cast<int>((x - kQuantize.x) / (kQuantize.w / 6)), 0, 5);
            shell_->values().set(*quantize_, 0, static_cast<float>(seg));
            focus_ = 2;
            return true;
        }
        if (contains(kSlower, x, y) || contains(kFaster, x, y))
        {
            if (! external)
                setNudge(contains(kFaster, x, y) ? 1 : -1);
            return true;
        }
        if (contains(kMetronome, x, y) && metronome_ != nullptr)
        {
            shell_->values().set(*metronome_, 0, value(metronome_, 0) >= 0.5f ? 0.0f : 1.0f);
            return true;
        }
        (void)ctx;
        return false;
    }

    int focus() const noexcept { return focus_; }

private:
    float value(const core::ParamSpec* s, float fallback) const
    {
        return s != nullptr && shell_ != nullptr ? shell_->values().get(*s, 0) : fallback;
    }
    void caption(DrawList& dl, const UiContext& ctx, float x, float y, float w, std::string_view text) const
    {
        const float cw = ctx.measure ? ctx.measure(TextStyleId::Caption, text) : 8.0f * static_cast<float>(text.size());
        draw::captionLine(dl, ctx.palette, x, y, w, text, cw, ctx.palette.surface1);
    }
    // The context bar's BPM | QUANTIZE readout follows the values at once (UID-628). Under an external clock the BPM
    // text is the followed tempo (TransportController::publish), never the project's.
    void publish() const
    {
        if (state_ == nullptr)
            return;
        if (tempo_ != nullptr && state_->syncBadge.empty())
            state_->bpm.format("%.1f", static_cast<double>(value(tempo_, 120)));
        if (quantize_ != nullptr)
            state_->quantize.set(kQuantizeLabels[std::clamp(static_cast<int>(std::lround(value(quantize_, 3))), 0, 5)]);
    }
    void turn(int detents, bool external)
    {
        switch (focus_)
        {
        case 0:
            if (tempo_ != nullptr && ! external) // turn = 1 BPM, SHIFT = 0.1 (SYN-002)
            {
                const float v = tempo_->step(value(tempo_, 120), detents, state_->shift);
                shell_->values().set(*tempo_, 0, std::round(v * 10.0f) / 10.0f);
            }
            break;
        case 1:
            if (swing_ != nullptr)
                shell_->values().set(*swing_, 0, swing_->step(value(swing_, 0), detents, false));
            break;
        default:
            if (quantize_ != nullptr)
                shell_->values().set(*quantize_, 0, quantize_->step(value(quantize_, 3), detents, false));
            break;
        }
        publish();
    }
    void tap(double nowMs)
    {
        tapAtMs_ = nowMs;
        if (const auto bpm = tapTempo_.tap(nowMs); bpm && tempo_ != nullptr)
            shell_->values().set(*tempo_, 0, tempo_->clamp(static_cast<float>(std::round(*bpm * 10.0) / 10.0)));
        publish();
    }
    void setNudge(int dir)
    {
        if (held_ == dir)
            return;
        held_ = dir;
        if (nudge_ != nullptr && shell_ != nullptr)
            shell_->values().set(*nudge_, 0, static_cast<float>(dir));
    }

    ShellApi* shell_;
    UiState* state_;
    const core::ParamSpec *tempo_ = nullptr, *swing_ = nullptr, *quantize_ = nullptr, *metronome_ = nullptr,
                          *nudge_ = nullptr;
    int focus_ = 0; // the BPM field has the focus on open (UID-626)
    int held_ = 0;
    int dragField_ = -1; // the field a running touch drag turns
    float dragY_ = 0;
    double tapAtMs_ = -1e9;
    core::musictime::TapTempo tapTempo_;
    mutable FixedText<8> whole_, fraction_, swingText_;
};

std::unique_ptr<Widget> makeBpmQuantize(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<BpmQuantizePanel>(a);
}
} // namespace

int bpmQuantizeFocus(Widget* body)
{
    auto* b = dynamic_cast<BpmQuantizePanel*>(body);
    return b != nullptr ? b->focus() : -1;
}
} // namespace ui

APP_REGISTER_WIDGET("BpmQuantizePanel", &ui::makeBpmQuantize);
APP_REGISTER_POPUP((ui::PopupSpec{.kind = ui::PopupSpec::Kind::Modal,
                                  .id = "bpmQuantize",
                                  .title = "BPM/QUANTIZE",
                                  .bodyComponent = "BpmQuantizePanel"}));
