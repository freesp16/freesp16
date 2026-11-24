// TRACK MENU (07-ui-design/screens/track-menu.md; UI-220…225, UI-258/259, UID-650…666). Template full, context bar
// TRACK TYPE + BYPASS. The TrackPanel: identity block (chip, type / stretch / loop badges, name — the HOME tile
// header; at its right end the small MUTE button: a tap, or the rotary on it and a push, toggles the track's MUTE
// MODE, track.muteMode, between SOUND = STOP SOUND, the default, and STEPS = STOP STEPS; focused, the knob strip
// binds it too), four header action slots, six fixed ModuleCard slots per
// track type with connectors only between chain neighbours, the wide SEQUENCE card and under it the MOTION bar (the
// track's recorded motion; a tap opens the MOTION page, ui/screens/motion). A SAMPLE track's first header action is MOTION RECORD (track.motionRec: knob turns
// while recording write motion; red LED = enabled). Nothing moves when a later package registers its screen (UID-665): every
// card is drawn at its fixed slot from the start; cards whose screen is not registered (FX1/FX2, LFO, SEQUENCE,
// NOTE/CC) are focusable and bind their strip tab, and the second tap does nothing until the screen exists.
//   - First tap / rotary focus binds the strip to the focused item (UI-223, UID-155), the second tap or a push opens
//     it; BACK from a module page returns with the focus on that module (UI-009).
//   - Opened: focus on BROWSE (SAMPLE, UI-222), SAMPLING/EDIT (THRU), SELECT CH (MIDI / AS-1).
//   - While a step is held, cards whose strip holds a lock on that step get the mod corner badge (UID-658).
//   - TRACK TYPE (context bar) opens the `track.type` ListPopup bound to track.mode (UI-225). The confirmation
//     before changing a track with content and the undo step come with the track command (the shell's
//     bound-list path writes the parameter directly).
//   - BYPASS mode (UI-221, the shell's UiState::bypassMode): while lit, bypassable cards (PLAYBACK playback.bypass,
//     AMP ENVELOPE env.bypass, FX1 / FX2 the slot's bypass) show the dashed warn outline and a tap or push toggles
//     their bypass instead of focusing or opening them; bypassed cards stay marked either way, and with BYPASS off a
//     bypassed card focuses but does not open its setting screen (the manual, p.36).
#include "ui/descriptors/ModuleSpec.h"
#include "ui/descriptors/PopupSpec.h"
#include "ui/screens/effects/FxCatalogue.h"
#include "ui/screens/effects/FxSlots.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/miditrack/As1Popups.h"
#include "ui/screens/motion/MotionLanes.h"
#include "ui/screens/playback/SampleViews.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/ModuleCard.h"
#include "ui/widgets/Toggle.h"
#include "ui/widgets/TrackTile.h"

#include "core/util/ampenv/EnvTime.h"
#include "core/util/midi/As1Program.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace env = ::core::ampenv;
using Kind = io::ControlEvent::Kind;
using Thumb = ModuleCardModel::Thumb;

constexpr float kActionX[] = {584, 752, 920, 1088};
constexpr RectF kSequence{48, 448, 1184, 88}, kMotion{48, 544, 1184, 48};
constexpr RectF kMuteMode{456, 170, 112, 28}; // the identity row's MUTE MODE button (≥ 44 lpx hit, kMuteModeHit)
constexpr RectF kMuteModeHit{448, 160, 128, 48};
constexpr RectF actionRect(int slot) noexcept
{
    return {kActionX[slot], 168, 160, 72};
}

// --- strips bound on focus (track-menu.md "Knob strip") ---
constexpr StripSpec kPlaybackStrip{"PLAYBACK",
                                   {"playback.pitch", "playback.start", "playback.loopStart", "playback.length",
                                    "playback.loop", "track.triggerMode"}};
constexpr StripSpec kAmpStrip{"AMP ENVELOPE",
                              {"env.velocity", "env.attack", "env.hold", "env.release", "track.triggerMode", ""}};
constexpr StripSpec kFx1Strip{"FX1", {}}, kFx2Strip{"FX2", {}},
    kLfoStrip{"LFO", {"lfo.speed", "lfo.depth", "lfo.phase", "", "", ""}}, kSeqStrip{"SEQUENCE", {}},
    kNoteStrip{"NOTE", {}}, kCc1Strip{"CC1", {}}, kCc2Strip{"CC2", {}}, kMixerStrip{"MIXER", {}};
constexpr StripSpec kBrowseStrip{"BROWSE", {}}, kLiveStrip{"LIVE SAMPLING", {}}, kEditStrip{"SAMPLING/EDIT", {}},
    kSelectChStrip{"SELECT CH", {}}, kAs1BankStrip{"BANK SELECT", {}}, kAs1ProgStrip{"PROG CHANGE", {}},
    // The MIDI track's BANK SELECT / PROG CHANGE values (midi.*; empty columns while those are not registered)
    kBankStrip{"BANK SELECT", {"midi.bankMsb", "midi.bankLsb", "", "", "", ""}},
    kProgStrip{"PROG CHANGE", {"midi.program", "", "", "", "", ""}},
    kScaleStrip{"SET SCALE", {"scaleMidi.area", "", "scaleMidi.root", "", "", ""}},
    kLimiterStrip{"SOFT LIMITER", {"mixer.softLimiter", "", "", "", "", ""}},
    kMotionRecStrip{"MOTION RECORD", {"track.motionRec", "", "", "", "", ""}}, kMotionStrip{"MOTION", {}},
    kMuteModeStrip{"MUTE MODE", {"track.muteMode", "", "", "", "", ""}};

// --- the six slots and four header actions per track type (track-menu.md "The six slots per track type") ---
struct Card
{
    std::string_view id, title;
    Thumb thumb;
    std::string_view opens;
    const StripSpec* strip;
    bool disabled = false;
    int port = 0;
};
struct Action
{
    enum class Kind : std::uint8_t
    {
        None,
        Button,
        Value,
        Toggle
    } kind = Kind::None;
    std::string_view label{}, opens{}, value{};
    const StripSpec* strip = nullptr;
    std::string_view binds{};
};
struct Chain
{
    std::array<Card, 6> cards;
    std::uint8_t links; // bit n = connector after slot n
    bool sequence;
    std::array<Action, 4> actions;
    int openFocus; // item focused when the screen opens: action slot
};
using AK = Action::Kind;
const Card kNoCard{};
const Card kPlayback{"playback", "PLAYBACK", Thumb::Wave, "track.playback", &kPlaybackStrip};
const Card kAmp{"ampEnvelope", "AMP ENVELOPE", Thumb::Envelope, "track.ampEnvelope", &kAmpStrip};
const Card kFx1{"fx1", "FX1 : NONE", Thumb::Effect, "track.fx1", &kFx1Strip};
const Card kFx2{"fx2", "FX2 : NONE", Thumb::Effect, "track.fx2", &kFx2Strip};
const Card kMixer{"mixer", "MIXER", Thumb::None, {}, &kMixerStrip, true};
const Card kLfo{"lfo", "LFO", Thumb::Lfo, "track.lfo", &kLfoStrip};
const Card kNote{"note", "NOTE", Thumb::Keys, "track.note", &kNoteStrip};
const Card kCc1{"cc1", "CC1", Thumb::Port, "track.cc1", &kCc1Strip, false, 1};
const Card kCc2{"cc2", "CC2", Thumb::Port, "track.cc2", &kCc2Strip, false, 2};

const Chain kChains[4] = {
    // Sample
    {{kPlayback, kAmp, kFx1, kFx2, kMixer, kLfo},
     0b0111,
     true,
     {Action{AK::Toggle, "MOTION RECORD", {}, {}, &kMotionRecStrip, "track.motionRec"},
      Action{AK::Button, "LIVE SAMPLING", "sampling.live", {}, &kLiveStrip},
      Action{AK::Button, "BROWSE", "track.browse", {}, &kBrowseStrip},
      Action{AK::Button, "SAMPLING/EDIT", "sampling.edit", {}, &kEditStrip}},
     2},
    // Midi
    {{kNote, kCc1, kCc2, kNoCard, kMixer, kLfo},
     0b0011,
     true,
     {Action{AK::Button, "SELECT CH", "track.midiChannel", {}, &kSelectChStrip},
      Action{AK::Value, "BANK SELECT", "midi.bankSelect", "M000 L000", &kBankStrip},
      Action{AK::Value, "PROG CHANGE", "midi.progChange", "000", &kProgStrip},
      Action{AK::Button, "SET SCALE", "track.setScaleMidi", {}, &kScaleStrip}},
     0},
    // Thru
    {{kNoCard, kNoCard, kFx1, kFx2, kMixer, kLfo},
     0b0100,
     false,
     {Action{}, Action{}, Action{AK::Toggle, "SOFT LIMITER", {}, {}, &kLimiterStrip, "mixer.softLimiter"},
      Action{AK::Button, "SAMPLING/EDIT", "sampling.edit", {}, &kEditStrip}},
     3},
    // Instrument (AS-1 profile, MID-060)
    {{kNote, kCc1, kCc2, kNoCard, kMixer, kLfo},
     0b0011,
     true,
     {Action{AK::Button, "SELECT CH", "track.midiChannel", {}, &kSelectChStrip},
      Action{AK::Value, "BANK SELECT", "as1.bankSelect", "U1", &kAs1BankStrip},
      Action{AK::Value, "PROG CHANGE", "as1.progChange", "P01", &kAs1ProgStrip},
      Action{AK::Button, "SET SCALE", "track.setScaleMidi", {}, &kScaleStrip}},
     0},
};
constexpr std::string_view kTypeBadges[] = {"SMPL", "MIDI", "THRU", "AS-1"};

// Focus items: 0–3 header actions, 4–9 cards, 10 SEQUENCE, 11 MOTION, 12 the MUTE MODE button. The rotary visits them
// in screen order: the MUTE button (left of the header actions) first.
constexpr int kCard0 = 4, kSeqItem = 10, kMotionItem = 11, kMuteItem = 12, kLastItem = kMuteItem;
constexpr int kFocusOrder[] = {kMuteItem, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, kSeqItem, kMotionItem};
int s_returnFocus = -1; // the module page opened last (BACK lands on it)

class TrackPanel final : public Widget
{
public:
    explicit TrackPanel(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state)
    {
        if (shell_ != nullptr && state_ != nullptr)
        {
            mode_ = m1::trackMode(*shell_, state_->selectedTrack);
            focus_ = s_returnFocus >= 0 && focusable(s_returnFocus) ? s_returnFocus : chain().openFocus;
            s_returnFocus = -1;
            boundFocus_ = -2;
        }
    }
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }
    int focus() const noexcept { return focus_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        auto& sh = *shell_;
        const auto& p = ctx.palette;
        const int t = state_->selectedTrack;
        const int mode = m1::trackMode(sh, t);
        if (mode != mode_)
        {
            mode_ = mode;
            focus_ = chain().openFocus;
        }
        bindFocused();
        const auto colour = theme::trackColour(m1::padColour(sh, t));
        dl.fill({16, 152, 1248, 456}, p.surface1, tk::radiusM);
        dl.stroke({16, 152, 1248, 456}, colour, tk::borderTrackFrame, tk::radiusM);
        draw::hline(dl, 32, 1248, 256, p.border);
        identity(dl, ctx, t, colour);
        const auto& c = chain();
        for (int s = 0; s < 4; ++s)
            action(dl, ctx, s, c.actions[static_cast<std::size_t>(s)], focus_ == s);
        const auto lockMask = lockedCards();
        for (int s = 0; s < 6; ++s)
            ModuleCard::draw(dl, ctx, card(s, colour, lockMask));
        for (int s = 0; s < 5; ++s)
            if (c.links & (1u << s))
                ModuleCard::connector(dl, ctx, s);
        sequence(dl, ctx, c.sequence);
        motionBar(dl, ctx, c.sequence, colour);
    }

    bool dirty(double) const override
    {
        return state_ != nullptr && (m1::feedsOf(shell_).position(state_->selectedTrack) >= 0 ||
                                     m1::loadProgress()[state_->selectedTrack - 1] >= 0 || state_->stepHeld > 0);
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            moveFocus(e.value);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            activate(focus_);
            return true;
        }
        if (! touch::isTouch(e) || e.value != touch::kDown)
            return false;
        const float x = touch::x(e), y = touch::y(e);
        for (int i = 0; i <= kLastItem; ++i)
            if (focusable(i) && contains(itemRect(i), x, y))
            {
                const bool card = i >= kCard0 && i != kMotionItem; // the MOTION bar opens on the first tap
                if (toggleBypass(i))
                    return true;         // BYPASS mode: the tap toggles the module's bypass (UI-221)
                if (card && focus_ != i) // first tap focuses and binds the strip (UI-223)
                    focus_ = i;
                else
                    activate(i); // buttons act on the first tap, cards on the second (UID-155)
                return true;
            }
        return contains(bounds(), x, y);
    }

private:
    const Chain& chain() const noexcept { return kChains[mode_ & 3]; }
    static RectF itemRect(int i) noexcept
    {
        return i < kCard0       ? actionRect(i)
               : i == kSeqItem    ? kSequence
               : i == kMotionItem ? kMotion
               : i == kMuteItem   ? kMuteModeHit
                                  : ModuleCard::rect(i - kCard0);
    }
    bool focusable(int i) const noexcept
    {
        const auto& c = chain();
        if (i < 0 || i > kLastItem)
            return false;
        if (i == kMuteItem)
            return true;
        if (i < kCard0)
            return c.actions[static_cast<std::size_t>(i)].kind != AK::None;
        if (i == kSeqItem || i == kMotionItem)
            return c.sequence;
        const auto& cd = c.cards[static_cast<std::size_t>(i - kCard0)];
        return ! cd.id.empty() && ! cd.disabled;
    }
    void moveFocus(int d)
    {
        constexpr int n = static_cast<int>(std::size(kFocusOrder));
        int at = 0; // focus_'s place in kFocusOrder
        while (at < n && kFocusOrder[at] != focus_)
            ++at;
        if (at == n)
            at = 0;
        for (int k = 0; k < std::abs(d); ++k)
        {
            int next = at;
            do
                next += d > 0 ? 1 : -1;
            while (next >= 0 && next < n && ! focusable(kFocusOrder[next]));
            if (next < 0 || next >= n)
                break;
            at = next;
        }
        focus_ = kFocusOrder[at];
    }
    const StripSpec* stripOf(int i) const noexcept
    {
        const auto& c = chain();
        if (i < 0)
            return nullptr;
        if (i < kCard0)
            return c.actions[static_cast<std::size_t>(i)].strip;
        if (i == kSeqItem)
            return &kSeqStrip;
        if (i == kMotionItem)
            return &kMotionStrip;
        if (i == kMuteItem)
            return &kMuteModeStrip;
        const auto& cd = c.cards[static_cast<std::size_t>(i - kCard0)];
        if (cd.id == "fx1" || cd.id == "fx2")
            return fxStrip(cd.id == "fx1" ? fx::Slot::Fx1 : fx::Slot::Fx2);
        return cd.strip;
    }
    // The effect a FX card's slot holds now, and whether its SYNC option is on (the strip shows the synced forms).
    struct FxKey
    {
        std::string_view effect;
        bool sync = false;
        bool operator==(const FxKey&) const = default;
    };
    FxKey fxKey(fx::Slot slot) const
    {
        const auto& env = fx::environment();
        const auto& params = shell_->params();
        FxKey k {env.slotAccess().effect(slot, state_->scene, state_->selectedTrack), false};
        if (const auto* d = fx::findEffect(env.effectRegistry(), k.effect))
            if (const auto* sync = fx::syncOption(params, slot, *d))
                k.sync = shell_->values().get(*sync, fx::scopeOf(*sync, state_->scene, state_->selectedTrack)) >= 0.5f;
        return k;
    }
    // Hardware feedback: a FX card bound an empty strip whatever the slot held. Its strip is now the slot's effect's
    // six knobs (as on the FX page, UID-125: the synced forms while SYNC is on), double-buffered so a rebind is a new
    // source for the shell.
    const StripSpec* fxStrip(fx::Slot slot) const
    {
        const auto& env = fx::environment();
        const auto& params = shell_->params();
        const auto k = fxKey(slot);
        const auto* d = fx::findEffect(env.effectRegistry(), k.effect);
        fxStripIndex_ ^= 1;
        auto& strip = fxStrips_[fxStripIndex_];
        strip = {fx::tab(slot), {}, 0};
        if (d != nullptr && ! fx::isNone(k.effect))
            for (std::size_t n = 0; n < 6; ++n)
            {
                const auto* s = fx::resolveParam(params, slot, *d, d->params[n]);
                if (s != nullptr && k.sync)
                    if (const auto* div = fx::syncForm(params, *s))
                        s = div;
                strip.columns[n] = s != nullptr ? s->id : std::string_view {};
            }
        return &strip;
    }
    void bindFocused() const
    {
        // A FX card follows its slot: a new effect or a SYNC change rebinds the strip while the card stays focused.
        FxKey key {};
        if (focus_ >= kCard0 && focus_ < kSeqItem)
        {
            const auto id = chain().cards[static_cast<std::size_t>(focus_ - kCard0)].id;
            if (id == "fx1" || id == "fx2")
                key = fxKey(id == "fx1" ? fx::Slot::Fx1 : fx::Slot::Fx2);
        }
        if (focus_ == boundFocus_ && key == boundFx_)
            return;
        boundFocus_ = focus_;
        boundFx_ = key;
        shell_->bindStrip(stripOf(focus_));
    }
    // UI-221: the bypass of a card (a parameter or an FX slot); null id = not bypassable. A MIDI / AS-1 track bypasses
    // CC1, CC2 and LFO alone (the manual's BYPASS button, p.36): NOTE is not a bypassable module.
    std::string_view bypassId(int i) const noexcept
    {
        if (i < kCard0 || i >= kSeqItem)
            return {};
        const auto id = chain().cards[static_cast<std::size_t>(i - kCard0)].id;
        if (id == "playback")
            return "playback.bypass";
        if (id == "ampEnvelope")
            return "env.bypass";
        if (id == "fx1" || id == "fx2")
            return id;
        if (id == "lfo")
            return "lfo.bypass";
        if (id == "cc1")
            return "midi.cc1Bypass";
        if (id == "cc2")
            return "midi.cc2Bypass";
        return {};
    }
    bool bypassedCard(int i) const
    {
        const auto id = bypassId(i);
        const int t = state_->selectedTrack;
        if (id == "fx1" || id == "fx2")
            return fx::environment().slotAccess().bypass(id == "fx1" ? fx::Slot::Fx1 : fx::Slot::Fx2, state_->scene, t);
        return ! id.empty() && m1::trackFlag(*shell_, id, t);
    }
    bool bypassable(int i) const
    {
        const auto id = bypassId(i);
        return id == "fx1" || id == "fx2" || (! id.empty() && shell_->params().find(id) != nullptr);
    }
    bool toggleBypass(int i)
    {
        if (! state_->bypassMode || ! bypassable(i))
            return false;
        const auto id = bypassId(i);
        const bool on = ! bypassedCard(i);
        if (id == "fx1" || id == "fx2")
            fx::environment().slotAccess().setBypass(id == "fx1" ? fx::Slot::Fx1 : fx::Slot::Fx2, state_->scene,
                                                     state_->selectedTrack, on);
        else
            m1::setTrackParam(*shell_, id, state_->selectedTrack, on ? 1.0f : 0.0f);
        return true;
    }

    void activate(int i)
    {
        if (! focusable(i))
            return;
        if (toggleBypass(i))
            return; // BYPASS mode: the push toggles the module's bypass (UI-221)
        focus_ = i;
        const auto& c = chain();
        if (i == kMuteItem) // MUTE MODE: STOP SOUND ↔ STOP STEPS
        {
            const int t = state_->selectedTrack;
            m1::setTrackParam(*shell_, "track.muteMode", t, m1::trackFlag(*shell_, "track.muteMode", t) ? 0.0f : 1.0f);
            return;
        }
        if (i < kCard0)
        {
            const auto& a = c.actions[static_cast<std::size_t>(i)];
            if (a.kind == AK::Toggle && ! a.binds.empty())
                m1::setTrackParam(*shell_, a.binds, state_->selectedTrack,
                                  m1::trackFlag(*shell_, a.binds, state_->selectedTrack) ? 0.0f : 1.0f);
            else if (! a.opens.empty())
                shell_->open(a.opens);
            return;
        }
        if (bypassedCard(i))
            return; // BYPASS OFF: a bypassed module's setting screen does not open (the manual's BYPASS button, p.36)
        const auto opens = i == kSeqItem      ? std::string_view("track.sequence")
                           : i == kMotionItem ? std::string_view("track.motion")
                                              : c.cards[static_cast<std::size_t>(i - kCard0)].opens;
        if (! opens.empty() && shell_->open(opens))
            s_returnFocus = i; // BACK returns with the focus on this module
    }

    std::uint8_t lockedCards() const
    {
        if (state_->stepHeld <= 0)
            return 0;
        std::uint8_t mask = 0;
        const auto& c = chain();
        for (int s = 0; s < 6; ++s)
        {
            const auto* strip = c.cards[static_cast<std::size_t>(s)].strip;
            if (strip == nullptr)
                continue;
            for (const auto id : strip->columns)
                if (const auto* spec = id.empty() ? nullptr : shell_->params().find(id))
                {
                    float v = 0;
                    if (spec->lockable && shell_->values().lockOf(*spec, state_->trackScope(), state_->stepHeld, v))
                        mask |= static_cast<std::uint8_t>(1u << s);
                }
        }
        return mask;
    }

    ModuleCardModel card(int s, std::uint32_t colour, std::uint8_t lockMask) const
    {
        const auto& cd = chain().cards[static_cast<std::size_t>(s)];
        ModuleCardModel m;
        m.slot = s;
        m.colour = colour;
        if (cd.id.empty())
        {
            m.state = ModuleCardModel::State::Empty;
            return m;
        }
        m.title = cd.title;
        if (cd.id == "fx1" || cd.id == "fx2") // "FX1 : DELAY" — the effect the slot holds (hardware feedback)
        {
            const auto slot = cd.id == "fx1" ? fx::Slot::Fx1 : fx::Slot::Fx2;
            const auto effect = fx::environment().slotAccess().effect(slot, state_->scene, state_->selectedTrack);
            const auto* d = fx::isNone(effect) ? nullptr : fx::findEffect(fx::environment().effectRegistry(), effect);
            auto& buf = fxTitle_[slot == fx::Slot::Fx1 ? 0 : 1];
            const auto name = d != nullptr ? d->name : std::string_view("NONE");
            std::snprintf(buf.data(), buf.size(), "%.*s : %.*s", static_cast<int>(fx::tab(slot).size()),
                          fx::tab(slot).data(), static_cast<int>(name.size()), name.data());
            m.title = buf.data();
            m.effect = d != nullptr ? d->id : std::string_view {};
        }
        m.thumb = cd.thumb;
        m.port = cd.port;
        m.state = cd.disabled                                     ? ModuleCardModel::State::Disabled
                  : focus_ == kCard0 + s && state_->stepHeld == 0 ? ModuleCardModel::State::Focused
                                                                  : ModuleCardModel::State::Rest;
        m.locked = (lockMask >> s & 1u) != 0;
        const int t = state_->selectedTrack;
        if (cd.thumb == Thumb::Wave)
        {
            view_ = m1::SampleViews::instance().of(t);
            if (! view_.empty())
            {
                m.wave = view_.overview;
                const double frames = static_cast<double>(view_.frames);
                const double start =
                    std::clamp(static_cast<double>(m1::trackParam(*shell_, "playback.start", t)), 0.0, frames);
                const double len =
                    static_cast<double>(m1::trackParam(*shell_, "playback.length", t, static_cast<float>(frames)));
                m.waveFrom = start; // START is in frames (GRID SNAP only snaps the PLAYBACK flags)
                m.waveTo = std::min(frames, m.waveFrom + (len > 0 ? len : frames));
                const float pos = m1::feedsOf(shell_).position(t);
                if (pos >= 0)
                    m.playhead = static_cast<float>((static_cast<double>(pos) * frames - m.waveFrom) /
                                                    std::max(1.0, m.waveTo - m.waveFrom));
            }
            m.bypassed = bypassedCard(kCard0 + s);
        }
        else if (cd.thumb == Thumb::Envelope)
        {
            const auto ia = env::clampIndex(env::EnvStage::Attack, m1::trackParam(*shell_, "env.attack", t));
            const auto ih = env::clampIndex(env::EnvStage::Hold, m1::trackParam(*shell_, "env.hold", t, 128));
            const auto ir = env::clampIndex(env::EnvStage::Release, m1::trackParam(*shell_, "env.release", t));
            auto& e = m.envelope;
            e.attackS = env::envSeconds(env::EnvStage::Attack, ia, false, 120);
            e.holdInf = env::isInfinite(env::EnvStage::Hold, ih);
            e.releaseInf = env::isInfinite(env::EnvStage::Release, ir);
            e.holdS = e.holdInf ? 0 : env::envSeconds(env::EnvStage::Hold, ih, false, 120);
            e.releaseS = e.releaseInf ? 0 : env::envSeconds(env::EnvStage::Release, ir, false, 120);
            e.windowS = EnvelopeEditor::fitWindow(e);
            m.bypassed = bypassedCard(kCard0 + s);
        }
        else
            m.bypassed = bypassedCard(kCard0 + s); // FX, LFO, NOTE, CC1, CC2
        m.bypassable = state_->bypassMode && bypassable(kCard0 + s);
        return m;
    }

    void identity(DrawList& dl, const UiContext& ctx, int t, std::uint32_t colour) const
    {
        const auto& p = ctx.palette;
        const auto& s = state_->tracks[static_cast<std::size_t>(t - 1)];
        auto textW = [&](std::string_view x)
        { return ctx.measure ? ctx.measure(TextStyleId::Caption, x) : 8.0f * static_cast<float>(x.size()); };
        auto badge = [&](float x, std::string_view text, std::uint32_t fill, std::uint32_t ink, float w)
        {
            if (w <= 0)
                w = textW(text) + 16;
            dl.fill({x, 173, w, 22}, fill, tk::radiusXs);
            dl.text(x + w / 2, 185, Align::Centre, TextStyleId::Caption, ink, text);
            return x + w + 6;
        };
        dl.fill({32, 172, 32, 24}, s.hasContent ? colour : theme::trackColour(m1::padColour(*shell_, t), true),
                tk::radiusXs);
        char number[4];
        std::snprintf(number, sizeof number, "%d", t);
        dl.text(48, 185, Align::Centre, TextStyleId::Label, s.hasContent ? p.onAccent : p.text1, number);
        float x = badge(72, kTypeBadges[mode_ & 3], colour, p.onAccent, 0);
        if (mode_ == 0 && s.hasContent)
        {
            x = badge(x, s.stretchBadge, p.surface3, p.text2, 0);
            x = badge(x, s.loopGlyph, p.surface3, colour, 32);
        }
        if (mode_ == 2) // THRU input meters (horizontal, L / R), up to the MUTE MODE button
        {
            const float w = std::min(360.0f, kMuteMode.x - 40 - x);
            for (int k = 0; k < 2; ++k)
                dl.fill({x, 174 + 12.0f * static_cast<float>(k), w, 6}, p.surface3, 3);
            dl.text(x + w + 8, 183, Align::Left, TextStyleId::Caption, p.text3, "IN");
        }
        muteModeButton(dl, ctx, t);
        const float loading = m1::loadProgress()[t - 1];
        if (loading >= 0) // UI-410: a 4 lpx accent bar 536 wide instead of the name
        {
            dl.fill({32, 222, 536, 4}, p.surface3, 2);
            dl.fill({32, 222, std::max(4.0f, 536 * std::min(1.0f, loading)), 4}, p.accent, 2);
            return;
        }
        if (s.hasContent && ! s.name.empty())
        {
            char buf[96];
            const auto name = TrackTile::middleTruncate(s.name, 536, TextStyleId::Title, ctx.measure, buf, sizeof buf);
            dl.text(32, 224, Align::Left, TextStyleId::Title, s.missing ? p.text3 : p.text1, name);
        }
        else
            dl.text(32, 224, Align::Left, TextStyleId::Title, p.text3, "----");
    }

    // MUTE MODE: "MUTE" and the mode, SOUND (default) in text-1, STEPS in accent (the MIXER's MUTE colour); the focus
    // ring while the rotary is on it.
    void muteModeButton(DrawList& dl, const UiContext& ctx, int t) const
    {
        const auto& p = ctx.palette;
        const bool steps = m1::trackFlag(*shell_, "track.muteMode", t);
        const auto& r = kMuteMode;
        dl.fill(r, p.surface2, tk::radiusS);
        if (steps)
            dl.stroke(r, p.accent, 1, tk::radiusS);
        const float cy = r.y + r.h / 2;
        dl.text(r.x + 10, cy, Align::Left, TextStyleId::Caption, p.text3, "MUTE");
        dl.text(r.x + r.w - 10, cy, Align::Right, TextStyleId::Caption, steps ? p.accent : p.text1,
                steps ? "STEPS" : "SOUND");
        if (focus_ == kMuteItem)
            draw::focusRing(dl, p, r, tk::radiusS);
    }

    void action(DrawList& dl, const UiContext& ctx, int slot, const Action& a, bool focused) const
    {
        const auto& p = ctx.palette;
        const auto r = actionRect(slot);
        switch (a.kind)
        {
        case AK::None:
            return;
        case AK::Button:
            Button::draw(dl, ctx, r, {a.label, ButtonStyle::Plain, false, false, focused});
            if (focused)
                ModuleCard::cornerTriangle(dl, r, p.accent);
            return;
        case AK::Value:
            dl.fill(r, p.surface2, tk::radiusM);
            dl.text(r.x + 80, 184, Align::Centre, TextStyleId::Caption, p.text2, a.label);
            dl.fill({r.x + 12, 198, 136, 32}, p.surface1, tk::radiusS);
            dl.text(r.x + 80, 214, Align::Centre, TextStyleId::ValueS, p.text1, valueOf(slot, a));
            if (focused)
                draw::focusRing(dl, p, r, tk::radiusM);
            return;
        case AK::Toggle:
        {
            const bool on = m1::trackFlag(*shell_, a.binds, state_->selectedTrack);
            if (a.binds == "track.motionRec") // MOTION RECORD: the label raised, ENABLED (rec) / DISABLED under it
            {
                Toggle::draw(dl, ctx, r, {{}, on, false, false, focused, p.rec});
                dl.text(r.x + r.w / 2, r.y + 36, Align::Centre, TextStyleId::Label, p.text1, a.label);
                dl.text(r.x + r.w / 2, r.y + 56, Align::Centre, TextStyleId::Caption, on ? p.rec : p.text3,
                        on ? "ENABLED" : "DISABLED");
                return;
            }
            Toggle::draw(dl, ctx, r, {a.label, on, false, false, focused, p.warn});
            return;
        }
        }
    }

    // BANK SELECT (`M000 L000`) and PROG CHANGE (`000`) of a MIDI track from the midi.* values
    // (UI-293/294; `---` = OFF, not sent); an AS-1 track's as its pickers name them (`U1`, `P01`); the action's own
    // text while those are not registered.
    std::string_view valueOf(int slot, const Action& a) const
    {
        const auto* msb = shell_->params().find("midi.bankMsb");
        const bool as1 = a.opens == "as1.bankSelect" || a.opens == "as1.progChange";
        if (msb == nullptr || (! as1 && a.opens != "midi.bankSelect" && a.opens != "midi.progChange"))
            return a.value;
        const int t = state_->selectedTrack;
        auto stored = [&](std::string_view id)
        { return static_cast<int>(std::lround(m1::trackParam(*shell_, id, t, -1))); };
        if (a.opens == "as1.bankSelect")
            return as1BankText(stored("midi.bankMsb"), stored("midi.bankLsb"));
        if (a.opens == "as1.progChange")
            return as1ProgramText(core::midi::as1::programShown(stored("midi.program")));
        auto three = [](char* out, int v) { v < 0 ? std::snprintf(out, 4, "---") : std::snprintf(out, 4, "%03d", v & 127); };
        char m[4], l[4], pg[4];
        three(m, static_cast<int>(std::lround(m1::trackParam(*shell_, "midi.bankMsb", t, -1))));
        three(l, static_cast<int>(std::lround(m1::trackParam(*shell_, "midi.bankLsb", t, -1))));
        three(pg, static_cast<int>(std::lround(m1::trackParam(*shell_, "midi.program", t, -1))));
        auto& out = values_[static_cast<std::size_t>(slot & 3)];
        if (a.opens == "midi.bankSelect")
            out.format("M%s L%s", m, l);
        else
            out.set(pg);
        return out.view();
    }

    void sequence(DrawList& dl, const UiContext& ctx, bool present) const
    {
        const auto& p = ctx.palette;
        const auto r = kSequence;
        if (! present)
        {
            ModuleCard::dashedRect(dl, r, p.border);
            return;
        }
        dl.fill(r, p.surface2, tk::radiusM);
        dl.text(r.x + r.w / 2, r.y + 14, Align::Centre, TextStyleId::Label, p.text1, "SEQUENCE");
        dl.fill({r.x + 16, r.y + 26, r.w - 32, 54}, p.surface1, tk::radiusS);
        const float ix = r.x + 32, iw = r.w - 64;
        static constexpr std::string_view steps[] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                                     "9", "10", "11", "12", "13", "14", "15", "16"};
        for (int i = 0; i < 16; ++i)
            dl.text(ix + iw * (static_cast<float>(i) + 0.5f) / 16, r.y + 40, Align::Centre, TextStyleId::Caption,
                    p.text2, steps[i]);
        for (int k = 0; k <= 96; ++k)
        {
            const bool tall = k % 6 == 0;
            draw::vline(dl, ix + iw * static_cast<float>(k) / 96, r.y + (tall ? 56 : 62), r.y + 72,
                        tall ? p.borderStrong : p.border);
        }
        // The triggers of the shown bar come from the pattern; none are drawn here.
        if (focus_ == kSeqItem)
        {
            draw::focusRing(dl, p, r, tk::radiusM);
            ModuleCard::cornerTriangle(dl, r, p.accent);
        }
    }

    // MOTION bar: the parameters and steps holding motion in the current pattern, one tick per step that holds
    // some (the pattern's length across the bar), MOTION RECORD's state; a tap opens the MOTION page.
    void motionBar(DrawList& dl, const UiContext& ctx, bool present, std::uint32_t colour) const
    {
        const auto& p = ctx.palette;
        const auto r = kMotion;
        if (! present)
        {
            ModuleCard::dashedRect(dl, r, p.border);
            return;
        }
        dl.fill(r, p.surface2, tk::radiusM);
        const float cy = r.y + r.h / 2;
        dl.text(r.x + 24, cy, Align::Left, TextStyleId::Label, p.text1, "MOTION");
        const auto* pat = motion::pattern(*shell_);
        const int t0 = state_->selectedTrack - 1;
        const int length = motion::patternLength(*shell_);
        const std::uint64_t lanes = motion::lanes(*shell_);
        const int params = __builtin_popcountll(lanes);
        const int steps = pat != nullptr ? motion::stepsWithMotion(*pat, t0, length) : 0;
        if (params == 0)
            motionText_.set("NO MOTION");
        else
            motionText_.format("%d PARAM%s \xC2\xB7 %d STEP%s", params, params == 1 ? "" : "S", steps, steps == 1 ? "" : "S");
        dl.text(r.x + 128, cy, Align::Left, TextStyleId::Caption, params == 0 ? p.text3 : p.text2, motionText_.view());
        // The steps holding motion, across the pattern.
        const float x0 = r.x + 360, w = r.w - 360 - 200;
        dl.fill({x0, cy - 1, w, 2}, p.border);
        if (pat != nullptr)
            for (int s = 0; s < length; ++s)
            {
                const auto& c = pat->steps[static_cast<std::size_t>(s)][static_cast<std::size_t>(t0 & 15)];
                if (c.lockIndex == core::model::Step::kNoLocks || c.lockIndex >= pat->locks.size() ||
                    pat->locks[c.lockIndex].mask == 0)
                    continue;
                const float sx = x0 + w * static_cast<float>(s) / static_cast<float>(length);
                dl.fill({sx, cy - 10, std::max(2.0f, w / static_cast<float>(length) - 2), 20}, colour, 2);
            }
        const bool rec = m1::trackFlag(*shell_, "track.motionRec", state_->selectedTrack);
        dl.fill({r.right() - 176, cy - 4, 8, 8}, rec ? p.rec : p.borderStrong, 4);
        dl.text(r.right() - 160, cy, Align::Left, TextStyleId::Caption, rec ? p.rec : p.text3,
                rec ? "REC ENABLED" : "REC DISABLED");
        dl.text(r.right() - 24, cy, Align::Right, TextStyleId::Label, p.text2, "\xE2\x80\xBA"); // ›
        if (focus_ == kMotionItem)
        {
            draw::focusRing(dl, p, r, tk::radiusM);
            ModuleCard::cornerTriangle(dl, r, p.accent);
        }
    }

    ShellApi* shell_;
    UiState* state_;
    mutable int mode_ = 0, focus_ = 2, boundFocus_ = -2;
    mutable FxKey boundFx_ {};
    mutable std::array<StripSpec, 2> fxStrips_ {};
    mutable int fxStripIndex_ = 0;
    mutable std::array<std::array<char, 40>, 2> fxTitle_ {};
    mutable m1::SampleView view_{};
    mutable std::array<FixedText<16>, 4> values_{}; // BANK SELECT / PROG CHANGE value texts
    mutable FixedText<40> motionText_{};
};

std::unique_ptr<Widget> makeTrackPanel(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<TrackPanel>(a);
}

// TRACK TYPE ListPopup items in the mockup's order, valued by core::model::TrackMode; the AS-1 item is named as in the
// manual ("Turn the rotary selector to select [TORAIZ AS-1]", p.40).
void trackTypes(std::string_view, std::vector<ListItem>& out)
{
    out.push_back({"SAMPLE", {}, {}, 0});
    out.push_back({"THRU", {}, {}, 2});
    out.push_back({"MIDI", {}, {}, 1});
    out.push_back({"TORAIZ AS-1", {}, {}, 3});
}
constexpr ListSpec kTrackTypeList{
    .id = "track.type", .title = "TRACK TYPE", .source = "trackTypes", .binds = "track.mode"};

// Reference chains (05-extensibility.md §3, UID-210) as data for the module registry.
constexpr ModuleSpec kModules[] = {
    {"playback", "PLAYBACK", "WaveformThumb", "playback.strip", "track.playback", 0b0001},
    {"ampEnvelope", "AMP ENVELOPE", "EnvelopeThumb", "ampEnvelope.strip", "track.ampEnvelope", 0b0001},
};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("ModuleCard", &ui::makeTrackPanel)
APP_REGISTER_LIST_SOURCE("trackTypes", &ui::trackTypes)
APP_REGISTER_POPUP((ui::PopupSpec{
    .kind = ui::PopupSpec::Kind::List, .id = "track.type", .title = "TRACK TYPE", .list = &ui::kTrackTypeList}))
APP_REGISTER_MODULE((ui::kModules[0]))
APP_REGISTER_MODULE((ui::kModules[1]))
APP_REGISTER_MODULE((ui::ModuleChainSpec{
    core::model::TrackMode::Sample, 0, {"playback", "ampEnvelope", "fx1", "fx2", "mixer", "lfo"}}))
APP_REGISTER_MODULE((ui::ModuleChainSpec{core::model::TrackMode::Thru, 0, {"", "", "fx1", "fx2", "mixer", "lfo"}}))
APP_REGISTER_MODULE((ui::ModuleChainSpec{core::model::TrackMode::Midi, 0, {"note", "cc1", "cc2", "", "mixer", "lfo"}}))
APP_REGISTER_MODULE((ui::ModuleChainSpec{
    core::model::TrackMode::Instrument, 0, {"note", "cc1", "cc2", "", "mixer", "lfo"}}))
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.menu",
                                    .title = "TRACK MENU",
                                    .parent = "home",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::TrackTypeBypass,
                                    .rail = {},
                                    .bodyComponent = "ModuleCard",
                                    .strip = {"TRACK MENU", {}}}))
