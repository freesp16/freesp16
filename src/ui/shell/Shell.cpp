#include "ui/shell/Shell.h"

#include "ui/shell/ColumnRules.h"
#include "ui/shell/Layout.h"
#include "ui/shell/PopupFactory.h"
#include "ui/shell/Timing.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Dialog.h"
#include "ui/widgets/GridPopup.h"
#include "ui/widgets/ListPopup.h"
#include "ui/widgets/Message.h"
#include "ui/widgets/Modal.h"
#include "ui/widgets/Progress.h"
#include "ui/widgets/SettingPopup.h"
#include "ui/widgets/Toast.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;

ScreenRegistry& prepare(const ShellConfig& cfg)
{
    auto& r = cfg.screens != nullptr ? *cfg.screens : ScreenRegistry::builtin();
    if (cfg.installFallbacks)
        installFallbackScreens(r);
    return r;
}

// Fixed-area targets (UID-305, UI-206/207): the screen ids the screens register (our choice).
constexpr std::string_view kMenuTargets[] = {"trackSetting", "mixer", "arranger", "utility"};
constexpr std::string_view kProject = "project", kSceneManager = "sceneManager", kBpmQuantize = "bpmQuantize",
                           kTrackType = "track.type", kUtility = "utility";
constexpr int kMaxPopupDepth = 2; // UID-914 (a Message may stack above, errors only)
constexpr double kCpuIntervalMs = 1000; // CPU USAGE refresh: one kernel accounting interval per readout
constexpr float kCpuHighPercent = 85;   // the readout turns `warn` at or above this

// UI-102, UI-403: the screen an extension jump key opens; SHIFT gives the neighbouring fixed-area screen
// that has no key of its own (SHIFT + ext.mixer = ARRANGER, SHIFT + ext.utility = TRACK SETTING). Empty = not
// a jump key (ext.browse is resolved per track).
std::string_view jumpTarget(io::Control c, bool shift) noexcept
{
    switch (c)
    {
    case io::Control::ExtMixer:
        return shift ? kMenuTargets[2] : kMenuTargets[1];
    case io::Control::ExtUtility:
        return shift ? kMenuTargets[0] : kUtility;
    case io::Control::ExtProject:
        return kProject;
    default:
        return {};
    }
}
} // namespace

Shell::Shell(ShellConfig cfg)
    : params_(cfg.params), measure_(std::move(cfg.measure)), bridge_(cfg.bridge), log_(std::move(cfg.log)),
      ownValues_(cfg.values == nullptr ? std::make_unique<LocalParamValues>(cfg.params, cfg.bridge) : nullptr),
      values_(cfg.values != nullptr ? cfg.values : ownValues_.get()),
      factory_(cfg.factory ? std::move(cfg.factory) : WidgetRegistry::builtin().dispatcher()), flags_(cfg.flags),
      router_(prepare(cfg), &flags_)
{
    router_.setPresenter(this);
    feeds_.attach(cfg.bridge);
    padColourSpec_ = params_.find("track.padColor");
    showCpuSpec_ = params_.find("ui.showCpu");
    muteModeSpec_ = params_.find("track.muteMode");
    rail_.reserve(L::railCompactMax);
    slideList_.reserve(2048, 16384);
    popups_.reserve(4);
    closing_.reserve(4);
    state_.tracks[0].hasContent = false;
    enterScreen(0);
}

Shell::~Shell()
{
    // A body or meter may call back into the shell from its destructor (a body unbinds its strip override, which
    // walks popups_): destroy them first, while every member they reach is still alive.
    body_.reset();
    meter_.reset();
}

void Shell::log(const char* fmt, ...) const
{
    if (! log_)
        return;
    char line[200];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    log_(line);
}

UiContext Shell::context(const theme::tk::Palette& p, double nowMs)
{
    return UiContext{p, measure_, nowMs, focus_};
}

// --- navigation ---------------------------------------------------------------------------------------------------
void Shell::enterScreen(double nowMs)
{
    seenChanges_ = router_.changes();
    const auto& s = router_.current();
    screenSinceMs_ = nowMs;
    screenDirection_ = router_.lastDirection();
    stripOverride_ = nullptr;
    focus_.screen = s.id;
    focus_.stripBound = false;
    const int remembered = router_.focusOf(s.id);
    const bool compact = s.rail.size() > L::railMax;
    rail_.clear();
    const auto rects = L::bodyRects(s.body);
    const float railWidth = rects.rail.w > 0 ? rects.rail.w : L::railW;
    for (std::size_t i = 0; i < s.rail.size() && i < L::railCompactMax; ++i)
        rail_.emplace_back(&s.rail[i], static_cast<int>(i), compact, railWidth);
    WidgetArgs args{rects.content, {}, &params_, this, &state_, &feeds_};
    railOwner_ = nullptr;
    stripOwner_ = nullptr;
    lampSource_ = nullptr;
    leaveGuard_ = nullptr;
    body_ = s.bodyComponent.empty() || ! factory_ ? nullptr : factory_(s, args);
    railOwner_ = dynamic_cast<RailOwner*>(body_.get());
    stripOwner_ = dynamic_cast<StripOwner*>(body_.get());
    lampSource_ = dynamic_cast<const LampSource*>(body_.get());
    leaveGuard_ = dynamic_cast<LeaveGuard*>(body_.get());
    meter_.reset();
    if (! s.meter.empty())
        if (const auto make = WidgetRegistry::builtin().find("Meter"))
            meter_ = make(s, WidgetArgs{rects.meter, s.meter, &params_, this, &state_, &feeds_});
    touchOwner_ = TouchOwner::None;
    dragColumn_ = -1;
    closing_.clear();          // a fading popup may draw state of the screen it belonged to
    state_.bypassMode = false; // the BYPASS mode ends with its screen (our choice, UI-221)
    state_.sceneLocked = false; // the next body says whether SCENE | PATTERN stays greyed
    state_.samplerRec = false;  // … and whether the status-row REC shows the sampler
    // Focus: the remembered one (UI-009), else the body when it has one, else the first rail item (holes skipped).
    int firstItem = -1;
    for (int i = 0; i < static_cast<int>(rail_.size()) && firstItem < 0; ++i)
        firstItem = railFocusable(i) ? i : -1;
    focus_.focused = railFocusable(remembered) ? remembered : (body_ != nullptr ? -1 : firstItem);
    resolveStrip();
    changedSinceFrame_ = true;
    char crumb[160];
    int n = 0;
    for (const auto t : router_.breadcrumb())
        n += std::snprintf(crumb + n, sizeof crumb - static_cast<std::size_t>(n), "%s%.*s", n ? " > " : "",
                           static_cast<int>(t.size()), t.data());
    log("NAV %.*s (%s)", static_cast<int>(s.id.size()), s.id.data(), crumb);
}

bool Shell::open(std::string_view id)
{
    // A guarded body is asked before a screen change (LeaveGuard.h); popups and the current screen pass.
    if (leaveGuard_ != nullptr && id != router_.current().id && router_.resolve(id) != nullptr &&
        ! leaveGuard_->mayLeave(id))
        return true;
    router_.rememberFocus(focus_.focused);
    if (! router_.open(id))
    {
        log("NAV %.*s not registered", static_cast<int>(id.size()), id.data());
        return false;
    }
    return true;
}

void Shell::back()
{
    if (! popupOpen() && leaveGuard_ != nullptr && ! leaveGuard_->mayLeave({}))
        return; // LeaveGuard.h
    if (! popupOpen())
        router_.rememberFocus(focus_.focused);
    router_.back();
}

void Shell::home()
{
    if (const auto* top = topPopup(); top != nullptr && top->blocksHome())
        return; // a blocking progress ignores HOME until done
    if (! popupOpen() && leaveGuard_ != nullptr && router_.current().id != "home" && ! leaveGuard_->mayLeave("home"))
        return; // LeaveGuard.h
    router_.rememberFocus(focus_.focused);
    router_.home();
}

void Shell::selectTrack(int t)
{
    if (t < 1 || t > 16 || t == state_.selectedTrack)
        return;
    state_.selectedTrack = t; // screens follow the selection (UI-205, UI-405)
    changedSinceFrame_ = true;
}

void Shell::bindStrip(const StripSpec* strip)
{
    stripOverride_ = strip;
    focus_.stripBound = strip != nullptr;
    resolveStrip();
}

// --- strip binding --------------------------------------------------------------------------------------------------
void Shell::resolveStrip()
{
    const PopupWidget* top = nullptr;
    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
        if (! (*it)->closed && ! (*it)->bindings().empty())
        {
            top = it->get();
            break;
        }
    popupStrip_ = top != nullptr;
    const void* source = top != nullptr              ? static_cast<const void*>(top)
                         : stripOverride_ != nullptr ? static_cast<const void*>(stripOverride_)
                                                     : static_cast<const void*>(&router_.current());
    if (source == boundSource_)
        return;
    boundSource_ = source;
    bound_.fill(nullptr);
    touchedMs_.fill(-1e9);
    if (top != nullptr)
    {
        for (const auto& b : top->bindings())
            if (b.column < 6)
                bound_[b.column] = params_.find(b.param);
        groupMask_ = 0;
        tab_.set(top->kind() == PopupSpec::Kind::Modal ? std::string_view("") : std::string_view(""));
        return;
    }
    const auto& strip = stripOverride_ != nullptr ? *stripOverride_ : router_.current().strip;
    for (std::size_t n = 0; n < 6; ++n)
        bound_[n] = strip.columns[n].empty() ? nullptr : params_.find(strip.columns[n]);
    groupMask_ = strip.groupMask;
    tab_.set(strip.tab);
}

std::uint16_t Shell::scopeOf(const core::ParamSpec& s) const noexcept
{
    switch (s.scope)
    {
    case core::ParamScope::Track:
        return state_.trackScope();
    case core::ParamScope::Scene:
    case core::ParamScope::SceneOverrideElseProject:
        return static_cast<std::uint16_t>(state_.scene - 1);
    default:
        return 0;
    }
}

void Shell::turnEncoder(int n0, int detents, double nowMs)
{
    const auto* spec = bound_[static_cast<std::size_t>(n0)];
    const auto& col = strip_.model().columns[static_cast<std::size_t>(n0)];
    if (spec == nullptr || col.disabled || col.readOnly || detents == 0)
        return; // an empty or disabled column ignores its encoder (UI-002/003)
    const auto scope = scopeOf(*spec);
    float v = values_->get(*spec, scope);
    if (stripOwner_ != nullptr && ! popupStrip_ &&
        stripOwner_->stripTurn(n0, *spec, detents, state_.shift))
    {
        touchedMs_[static_cast<std::size_t>(n0)] = nowMs;
        strip_.lastTouchMs = nowMs;
        changedSinceFrame_ = true;
        log("INPUT knob%d %+d (%.*s, screen)", n0 + 1, detents, static_cast<int>(spec->id.size()), spec->id.data());
        return; // the screen applied it (StripOwner)
    }
    if (state_.stepHeld > 0)
    {
        if (! spec->lockable)
            return; // greyed while a step is held (UI-204)
        values_->lockOf(*spec, scope, state_.stepHeld, v);
        values_->setLock(*spec, scope, state_.stepHeld, spec->step(v, detents, state_.shift));
    }
    else
        values_->set(*spec, scope, spec->step(v, detents, state_.shift));
    touchedMs_[static_cast<std::size_t>(n0)] = nowMs;
    strip_.lastTouchMs = nowMs;
    changedSinceFrame_ = true;
    char buf[32];
    const float now =
        state_.stepHeld > 0 ? (values_->lockOf(*spec, scope, state_.stepHeld, v), v) : values_->get(*spec, scope);
    const int len = std::max(0, spec->formatValue(now, buf, sizeof buf));
    log("INPUT knob%d %+d (%.*s = %.*s%s)", n0 + 1, detents, static_cast<int>(spec->id.size()), spec->id.data(), len,
        buf, state_.stepHeld > 0 ? " lock" : "");
}

void Shell::updateStripModel(double nowMs)
{
    resolveStrip();
    auto& m = strip_.model();
    m.stepHeld = state_.stepHeld;
    m.groupMask = groupMask_;
    // "{track}" in a tab is the selected track's number (HOME: "TRACK {track}" → "TRACK 1").
    const auto t = tab_.view();
    if (const auto at = t.find("{track}"); at != std::string_view::npos)
    {
        tabShown_.format("%.*s%d%.*s", static_cast<int>(at), t.data(), state_.selectedTrack,
                         static_cast<int>(t.size() - at - 7), t.data() + at + 7);
        m.tab = tabShown_.view();
    }
    else
        m.tab = t;
    for (std::size_t n = 0; n < 6; ++n)
    {
        auto& c = m.columns[n];
        c = {};
        const auto* spec = bound_[n];
        if (spec == nullptr || ! flags_.enabled(spec->featureFlag))
            continue;
        const auto scope = scopeOf(*spec);
        c.spec = spec;
        c.value = values_->get(*spec, scope);
        if (state_.stepHeld > 0)
        {
            float lock = 0;
            if (values_->lockOf(*spec, scope, state_.stepHeld, lock))
            {
                c.locked = true;
                c.value = lock;
            }
            c.disabled = ! spec->lockable;
        }
        if (const auto rule = ColumnRuleRegistry::builtin().find(spec->id);
            rule != nullptr && rule(*values_, params_, scope))
            c.disabled = true;
        c.touched = nowMs - touchedMs_[n] < timing::recentlyTouchedMs;
        c.fine = state_.shift;
        if (stripOwner_ != nullptr && ! popupStrip_)
            stripOwner_->stripColumn(static_cast<int>(n), c);
    }
}

// --- frame ----------------------------------------------------------------------------------------------------------
void Shell::pollFeeds(double nowMs)
{
    if (bridge_ == nullptr)
    {
        feeds_.poll(nowMs, &state_);
        return;
    }
    const auto& feed = bridge_->meters();
    for (int ch = 0; ch < 2; ++ch)
    {
        // Meters decay in the UI (threading.md): 20 dB per motion.meter-release, evaluated per frame.
        const float in = toDb(feed.masterPeak[ch]);
        const float fall = 20.0f * (1000.0f / 60.0f) / tk::motionMeterReleaseMs;
        state_.masterDb[ch] = std::max(in, state_.masterDb[ch] - fall);
        if (in >= state_.masterPeakDb[ch] || nowMs - state_.masterPeakAtMs[ch] > tk::motionPeakHoldMs)
        {
            state_.masterPeakDb[ch] = in;
            state_.masterPeakAtMs[ch] = nowMs;
        }
    }
    state_.tracks[0].levelDb = state_.masterDb[0];
    feeds_.poll(nowMs, &state_); // levels, playhead and this frame's engine events for the widgets
}

// The status row's MUTE badge detail: the current scene's tracks' MUTE MODE — one name when they agree, else the split.
std::string_view Shell::muteModeSummary()
{
    if (state_.padMuteView != UiState::PadMuteView::Mute || muteModeSpec_ == nullptr)
        return {};
    int steps = 0;
    for (int t = 1; t <= 16; ++t)
        steps += values_->get(*muteModeSpec_, state_.trackScope(t)) >= 0.5f ? 1 : 0;
    if (steps == 0)
        return "STOP SOUND";
    if (steps == 16)
        return "STOP STEPS";
    muteModeText_.format("SOUND %d \xC2\xB7 STEPS %d", 16 - steps, steps); // ·
    return muteModeText_.view();
}

void Shell::updateFrameModels(double nowMs)
{
    const auto& s = router_.current();
    // CPU USAGE: the machine's busy share over the last second (core::CpuUsage), "--" until the first interval ends.
    const bool showCpu = showCpuSpec_ != nullptr && values_->get(*showCpuSpec_, 0) >= 0.5f;
    if (! showCpu)
    {
        cpu_.reset();
        cpuSampledMs_ = -1e18;
    }
    else if (nowMs - cpuSampledMs_ >= kCpuIntervalMs)
    {
        cpuSampledMs_ = nowMs;
        if (cpu_.sample() && cpu_.percent() >= 0)
            cpuText_.format("%d%%", static_cast<int>(std::lround(cpu_.percent())));
        else
            cpuText_.set("--");
    }
    statusRow_.set({router_.breadcrumb(), state_.playing, state_.recArmed || state_.samplerRec, state_.midiInAtMs,
                    state_.midiOutAtMs, state_.syncBadge, state_.syncLost,
                    showCpu ? cpuText_.view() : std::string_view{}, showCpu && cpu_.percent() >= kCpuHighPercent,
                    state_.padMuteView, muteModeSummary()});
    drawnPadMuteView_ = state_.padMuteView;
    ContextBarModel cb = contextBar_.model();
    cb.kind = s.contextBar;
    bool projectPage = false;
    for (const auto id : router_.path())
        projectPage |= id == kProject;
    cb.back =
        s.contextBar == ContextBarKind::TrackContext || s.contextBar == ContextBarKind::TrackTypeBypass || projectPage;
    cb.bpm = state_.bpm.view();
    cb.quantize = state_.quantize.view();
    sceneText_.format("%02d", state_.scene);
    patternText_.format("%02d", state_.pattern);
    cb.scene = sceneText_.view();
    cb.pattern = patternText_.view();
    cb.position = state_.position.view();
    cb.playing = state_.playing;
    cb.sceneSelected = s.id == kSceneManager;
    cb.sceneDimmed = state_.sceneLocked; // UI-206
    cb.menuSelected = -1;
    for (const auto id : router_.path())
        for (int i = 0; i < 4; ++i)
            if (id == kMenuTargets[i])
                cb.menuSelected = i;
    const auto& tr = state_.selected();
    cb.track = state_.selectedTrack;
    cb.trackName = tr.name;
    cb.typeBadge = tr.typeBadge;
    cb.stretchBadge = tr.hasContent ? tr.stretchBadge : std::string_view{};
    cb.loopGlyph = tr.hasContent ? tr.loopGlyph : std::string_view{};
    cb.missing = tr.missing;
    cb.bypass = state_.bypassMode;
    if (nowMs - pressedAtMs_ > tk::motionFastMs)
        cb.pressed = -1; // pressed state for motion.fast (UID-156)
    contextBar_.set(cb);

    // Rail items: toggles and options show their bound parameter; a RailOwner body refines the state.
    for (std::size_t i = 0; i < rail_.size(); ++i)
    {
        auto& item = rail_[i];
        auto& st = item.state();
        if (isRailHole(*item.spec()))
            continue;
        st.focused = focus_.focused == static_cast<int>(i) && ! popupOpen();
        const auto* spec = item.spec()->binds.empty() ? nullptr : params_.find(item.spec()->binds);
        st.disabled = ! item.spec()->opens.empty() && router_.resolve(item.spec()->opens) == nullptr &&
                      PopupRegistry::builtin().find(item.spec()->opens) == nullptr;
        if (spec != nullptr)
        {
            const float v = values_->get(*spec, scopeOf(*spec));
            st.on = v >= 0.5f;
            if (item.spec()->kind == RailItemSpec::Kind::Option && spec->type == core::ParamType::Enum)
            {
                const int k = static_cast<int>(std::lround(v - spec->minValue));
                st.value = k >= 0 && k < static_cast<int>(spec->enumLabels.size())
                               ? spec->enumLabels[static_cast<std::size_t>(k)]
                               : std::string_view{};
            }
        }
        if (railOwner_ != nullptr)
            railOwner_->railState(static_cast<int>(i), *item.spec(), st);
    }
    updateStripModel(nowMs);
}

void Shell::eraseClosedPopups()
{
    const auto before = popups_.size();
    // Closed popups fade out (motion.base, popups-and-feedback.md "open/close in base"); input ends the fade.
    for (auto& p : popups_)
        if (p->closed && p->modal() && p->visibleAt(nowMs_) && ! state_.reduceMotion && closing_.size() < 4)
        {
            p->closedMs = nowMs_;
            closing_.push_back(std::move(p));
        }
    popups_.erase(
        std::remove_if(popups_.begin(), popups_.end(), [](const auto& p) { return p == nullptr || p->closed; }),
        popups_.end());
    if (popups_.size() != before)
    {
        resolveStrip();
        changedSinceFrame_ = true;
    }
    if (toast_ && toast_->closed)
        toast_.reset();
}

void Shell::frame(DrawList& dl, double nowMs)
{
    nowMs_ = nowMs;
    eraseClosedPopups();
    if (router_.changes() != seenChanges_)
        enterScreen(nowMs);
    pollFeeds(nowMs);
    updateFrameModels(nowMs);

    const auto& base = theme::palette(state_.skin);
    auto ctx = context(base, nowMs);
    dl.fill(L::canvas, base.bg);

    // Body, with the screen push/pop transition: cross-fade + 16 lpx slide in the direction of travel (motion.slow).
    const double slowMs = state_.reduceMotion ? 0 : tk::motionSlowMs;
    const float t = theme::ease(theme::progress(nowMs, screenSinceMs_, slowMs));
    const auto bodyPalette = t < 1 ? theme::faded(base, t) : base;
    UiContext bodyCtx{bodyPalette, measure_, nowMs, focus_};
    // During the transition the body is built into a scratch list and re-appended shifted by the slide offset (the
    // DrawList has no transform); otherwise it is built in place.
    const float slide =
        t < 1 && screenDirection_ != 0 ? L::screenSlide * (1 - t) * static_cast<float>(screenDirection_) : 0;
    auto& target = slide != 0 ? slideList_ : dl;
    if (slide != 0)
        slideList_.clear();
    for (std::size_t i = 0; i < rail_.size(); ++i)
    {
        const auto& item = rail_[i];
        if (isRailHole(*item.spec()))
            continue;
        if (railOwner_ == nullptr ||
            ! railOwner_->railDraw(static_cast<int>(i), *item.spec(), item.state(), target, bodyCtx))
            item.build(target, bodyCtx);
    }
    if (meter_)
        meter_->build(target, bodyCtx);
    if (body_)
        body_->build(target, bodyCtx);
    if (slide != 0)
        for (const auto& c : slideList_.commands())
        {
            const ui::RectF r{c.rect.x + slide, c.rect.y, c.rect.w, c.rect.h};
            if (c.op == DrawCmd::Op::Fill)
                dl.fill(r, c.argb, c.radius);
            else if (c.op == DrawCmd::Op::Stroke)
                dl.stroke(r, c.argb, c.strokeWidth, c.radius);
            else
                dl.text(r.x, r.y, c.align, c.style, c.argb, slideList_.textOf(c));
        }

    // The frame regions never animate (UID-015: static chrome).
    statusRow_.build(dl, ctx);
    contextBar_.build(dl, ctx);
    const bool covered = std::any_of(popups_.begin(), popups_.end(), [](const auto& p) { return p->coversStrip(); });
    if (! covered && ! (base.classic && current().id == "utility"))
        strip_.build(dl, ctx);

    // Popups: a scrim under each modal level (a nested popup dims its parent with a second scrim, UID-914).
    const double baseMs = state_.reduceMotion ? 0 : tk::motionBaseMs;
    int level = 0;
    for (const auto& p : popups_)
    {
        if (! p->modal() || ! p->visibleAt(nowMs))
            continue;
        const float e = theme::ease(theme::progress(nowMs, p->openedMs, baseMs));
        draw::scrim(dl, theme::faded(base, e), L::canvas);
        p->dx = level > 0 && p->kind() == PopupSpec::Kind::List ? L::nestedOffset : 0;
        p->dy = L::popupRise * (1 - e) + p->dx;
        const auto pal = e < 1 ? theme::faded(base, e) : base;
        UiContext pctx{pal, measure_, nowMs, focus_};
        p->build(dl, pctx);
        ++level;
    }
    // Closing popups: the reverse of the open transition (fade + 8 lpx sink), scrim included.
    for (auto& p : closing_)
    {
        const float e = 1 - theme::ease(theme::progress(nowMs, p->closedMs, baseMs));
        if (e <= 0)
            continue;
        draw::scrim(dl, theme::faded(base, e), L::canvas);
        p->dy = L::popupRise * (1 - e) + p->dx;
        UiContext pctx{theme::faded(base, e), measure_, nowMs, focus_};
        p->build(dl, pctx);
    }
    closing_.erase(
        std::remove_if(closing_.begin(), closing_.end(), [&](const auto& p) { return nowMs - p->closedMs >= baseMs; }),
        closing_.end());
    // Caution chips and toasts above the scrim (UID-916): at most two chips, then the newest plus "+n" (UID-909).
    if (cautionCount_ > 0)
    {
        float right = L::notifyX1;
        if (cautionCount_ > 2)
        {
            moreText_.format("+%d", static_cast<int>(cautionCount_ - 1));
            right = Toast::drawCautionChip(dl, ctx, right, moreText_.view()) - tk::space[2];
            Toast::drawCautionChip(dl, ctx, right, cautions_[cautionCount_ - 1].text);
        }
        else
            for (std::size_t i = cautionCount_; i-- > 0;)
                right = Toast::drawCautionChip(dl, ctx, right, cautions_[i].text) - tk::space[2];
    }
    if (toast_)
    {
        toast_->tick(nowMs, measure_);
        if (! toast_->closed)
            toast_->build(dl, ctx);
    }
    frameMs_ = nowMs;
    changedSinceFrame_ = false;
}

const io::LampFrame& Shell::lamps() const noexcept
{
    if (lampsAtMs_ != frameMs_) // UI-150: the lamps follow the last frame's state, computed once per frame
    {
        lamps_.update(state_, frameMs_, &Shell::padColourOf, this, lampSource_);
        lampsAtMs_ = frameMs_;
    }
    return lamps_.frame();
}

int Shell::padColourOf(const void* self, int t)
{
    // track.padColor (UI-228, TRK-031): palette index 0–15 (default per track = its own index, set by the model).
    const auto& sh = *static_cast<const Shell*>(self);
    const auto* spec = sh.padColourSpec_;
    if (spec == nullptr)
        return t;
    const int v = static_cast<int>(std::lround(sh.values_->get(*spec, sh.state_.trackScope(t))));
    return (v & 15) + 1;
}

bool Shell::dirty(double nowMs) const
{
    if (changedSinceFrame_ || nowMs - screenSinceMs_ < tk::motionSlowMs + 20 || strip_.dirty(nowMs) ||
        statusRow_.dirty(nowMs) || state_.padMuteView != drawnPadMuteView_)
        return true;
    if (showCpuSpec_ != nullptr && values_->get(*showCpuSpec_, 0) >= 0.5f && nowMs - cpuSampledMs_ >= kCpuIntervalMs)
        return true; // the CPU readout is due
    if ((body_ && body_->dirty(nowMs)) || (meter_ && meter_->dirty(nowMs)) || (toast_ && ! toast_->closed))
        return true;
    for (const auto& p : popups_)
        if (nowMs - p->openedMs < tk::motionBaseMs + timing::progressDelayMs + 20 || p->dirty(nowMs))
            return true;
    return ! closing_.empty();
}

// --- input ----------------------------------------------------------------------------------------------------------
void Shell::interruptAnimations() noexcept
{
    // UID-065: input applies immediately and every running transition jumps to its end.
    screenSinceMs_ = -1e9;
    closing_.clear(); // a close fade ends at once
    for (auto& p : popups_)
        if (p->kind() != PopupSpec::Kind::Message || p->visibleAt(nowMs_))
            p->openedMs = std::min(p->openedMs, nowMs_ - tk::motionBaseMs);
}

bool Shell::railFocusable(int i) const noexcept
{
    return i >= 0 && i < static_cast<int>(rail_.size()) && ! isRailHole(*rail_[static_cast<std::size_t>(i)].spec());
}

RectF Shell::railRect(int i) const noexcept
{
    const auto& item = rail_[static_cast<std::size_t>(i)];
    RectF r = item.bounds();
    if (railOwner_ != nullptr)
        railOwner_->railRect(i, *item.spec(), r);
    return r;
}

void Shell::moveRailFocus(int d)
{
    const int n = static_cast<int>(rail_.size());
    int first = -1, last = -1;
    for (int i = 0; i < n; ++i)
        if (railFocusable(i))
        {
            first = first < 0 ? i : first;
            last = i;
        }
    if (first < 0 || d == 0)
        return;
    if (focus_.focused < 0)
    {
        focus_.focused = d > 0 ? first : last;
        return;
    }
    // Holes are skipped; past the last item a RailOwner body may take the focus (its first element).
    const int step = d > 0 ? 1 : -1;
    int i = focus_.focused;
    for (int k = 0; k < std::abs(d); ++k)
    {
        int next = i + step;
        while (next >= 0 && next < n && ! railFocusable(next))
            next += step;
        if (next < 0 || next >= n)
        {
            if (step > 0 && railOwner_ != nullptr && railOwner_->railFocusEnter(std::abs(d) - k))
            {
                focus_.focused = -1;
                return;
            }
            break;
        }
        i = next;
    }
    focus_.focused = i;
}

void Shell::activateRail(int i, float x, float y)
{
    if (! railFocusable(i))
        return;
    const auto& spec = *rail_[static_cast<std::size_t>(i)].spec();
    focus_.focused = i;
    const auto* p = spec.binds.empty() ? nullptr : params_.find(spec.binds);
    log("INPUT rail %.*s", static_cast<int>(spec.label.size()), spec.label.data());
    if (railOwner_ != nullptr && railOwner_->railActivate(i, spec, x, y))
        return; // the screen's own action (RailOwner)
    switch (spec.kind)
    {
    case RailItemSpec::Kind::Button:
        if (! spec.opens.empty())
            open(spec.opens);
        break;
    case RailItemSpec::Kind::Toggle:
        if (p != nullptr)
        {
            const auto scope = scopeOf(*p);
            values_->set(*p, scope, values_->get(*p, scope) >= 0.5f ? p->minValue : p->maxValue);
        }
        else if (! spec.opens.empty())
            open(spec.opens);
        break;
    case RailItemSpec::Kind::Option:
        if (! spec.opens.empty())
            open(spec.opens);
        else if (p != nullptr && p->type == core::ParamType::Enum)
        {
            ListRequest req;
            req.title = p->label;
            for (std::size_t k = 0; k < p->enumLabels.size(); ++k)
                req.items.push_back({p->enumLabels[k], {}, {}, static_cast<int>(k)});
            const auto scope = scopeOf(*p);
            req.current = static_cast<int>(std::lround(values_->get(*p, scope) - p->minValue));
            req.onChoose = [this, p, scope](int, const ListItem& it)
            { values_->set(*p, scope, p->minValue + static_cast<float>(it.value)); };
            showList(std::move(req));
        }
        break;
    }
}

void Shell::contextAction(ContextBar::Item item)
{
    auto cb = contextBar_.model();
    cb.pressed = item;
    contextBar_.set(cb);
    pressedAtMs_ = nowMs_;
    switch (item)
    {
    case ContextBar::SlotA:
        if (cb.back)
            back();
        else
            open(kProject);
        break;
    case ContextBar::Bpm:
    case ContextBar::Quantize:
        open(kBpmQuantize);
        break;
    case ContextBar::Scene:
    case ContextBar::Pattern:
        if (router_.current().id == kSceneManager)
            back(); // a tap on the lit SCENE | PATTERN closes SCENE MANAGER (UI-408)
        else
            open(kSceneManager);
        break;
    case ContextBar::Menu0:
    case ContextBar::Menu1:
    case ContextBar::Menu2:
    case ContextBar::Menu3:
    {
        const auto target = kMenuTargets[item - ContextBar::Menu0];
        if (router_.current().id != target)
            open(target);
        break;
    }
    case ContextBar::TrackType:
        open(kTrackType);
        break;
    case ContextBar::Bypass: // UI-221: a mode toggle, not a parameter
        state_.bypassMode = ! state_.bypassMode;
        log("INPUT bypass mode %s", state_.bypassMode ? "on" : "off");
        break;
    default:
        break;
    }
}

void Shell::handleTouch(const io::ControlEvent& e, UiContext& ctx)
{
    const float x = touch::x(e), y = touch::y(e);
    if (e.value == touch::kDrag)
    {
        ++touchDrags_;
        if (dragColumn_ >= 0) // vertical drag on a column (or a body's knob glyph) = turning, 8 px per detent (UI-123)
        {
            const float pixels = (dragY_ - y) / touch::kToCanvas; // logical px; ±1e-3 absorbs the ×1.6 rounding
            const int detents = static_cast<int>(pixels / 8 + (pixels > 0 ? 1e-3f : -1e-3f));
            if (detents != 0)
            {
                turnEncoder(dragColumn_, detents, e.timeMs);
                dragY_ -= static_cast<float>(detents) * 8 * touch::kToCanvas;
            }
            return;
        }
        if (touchOwner_ == TouchOwner::Body && body_) // the body that took the down follows the finger (UI-209)
            body_->handle(e, ctx);
        return;
    }
    if (e.value == touch::kUp)
    {
        static constexpr const char* owners[] = {"none", "frame", "strip", "body"};
        if (touchDrags_ > 0) // one trace line per dragged touch (no line per drag event)
            log("INPUT touch up %.0f,%.0f (%s, %d drags%s)", static_cast<double>(x), static_cast<double>(y),
                owners[static_cast<int>(touchOwner_)], touchDrags_, dragColumn_ >= 0 ? ", column" : "");
        touchDrags_ = 0;
        dragColumn_ = -1;
        touchOwner_ = TouchOwner::None;
        if (body_)
            body_->handle(e, ctx);
        return;
    }
    if (e.value != touch::kDown)
        return;
    touchOwner_ = TouchOwner::Frame;
    touchY_ = y;
    touchDrags_ = 0;
    dragColumn_ = -1;
    if (y < L::statusH && cautionCount_ > 0 && x >= L::notifyX0 && x < L::notifyX1)
    {
        showCautions();
        return;
    }
    if (y >= L::stripY && ! (ctx.palette.classic && current().id == "utility"))
    {
        const int column = strip_.columnAt(x, y);
        touchOwner_ = TouchOwner::Strip;
        dragColumn_ = column;
        dragY_ = y;
        return;
    }
    if (y >= L::contextY && y < L::bodyY)
    {
        contextAction(contextBar_.hit(x, y, ctx.palette.classic));
        return;
    }
    for (int i = 0; i < static_cast<int>(rail_.size()); ++i)
        if (railFocusable(i) && contains(railRect(i), x, y))
        {
            activateRail(i, x, y); // buttons, toggles and selectors act on the first tap (UID-155)
            return;
        }
    if (body_ && y >= L::bodyY)
    {
        if (focus_.focused >= 0 && ! rail_.empty())
            focus_.focused = -1;
        touchOwner_ = TouchOwner::Body;
        body_->handle(e, ctx);
    }
}

void Shell::dragColumn(int column0)
{
    if (column0 < 0 || column0 >= 6 || touchOwner_ != TouchOwner::Body)
        return;
    dragColumn_ = column0; // the rest of this touch turns the column (UID-163)
    dragY_ = touchY_;
}

bool Shell::jumpKey(io::Control c)
{
    std::string_view target = jumpTarget(c, state_.shift);
    if (c == io::Control::ExtBrowse)
    {
        // BROWSE of the selected track (UI-403); a track that has no BROWSE (THRU, MIDI) opens its TRACK MENU.
        const auto* mode = params_.find("track.mode");
        const bool sample = mode == nullptr || std::lround(values_->get(*mode, state_.trackScope())) == 0;
        target = sample ? std::string_view("track.browse") : std::string_view("track.menu");
    }
    if (target.empty())
        return false;
    const auto name = io::nameOf(c);
    log("INPUT key %s%.*s -> %.*s", state_.shift ? "shift+" : "", static_cast<int>(name.size()), name.data(),
        static_cast<int>(target.size()), target.data());
    if (router_.current().id != target)
        open(target);
    else
        dismissAll(); // the screen is already shown: the key still closes its popups (as HOME does)
    return true;
}

bool Shell::toggleRailFocus()
{
    // SHIFT + sel.push moves the one focus (UID-150) between a screen's body and its rail, so a
    // body that keeps the rotary for itself (PLAYBACK / SET SLICE zoom, AMP ENVELOPE handles, BROWSE rows) leaves the
    // rail reachable without touch: from the body to the rail's first item, from the rail back to the body. Screens
    // without a focusable rail (HOME: SHIFT + push = BROWSE, UI-403) leave SHIFT + push to their body.
    if (! body_)
        return false;
    int first = -1;
    for (int i = 0; i < static_cast<int>(rail_.size()) && first < 0; ++i)
        first = railFocusable(i) ? i : -1;
    if (first < 0)
        return false;
    focus_.focused = focus_.focused >= 0 ? -1 : first;
    log("INPUT rail focus %s", focus_.focused >= 0 ? "on" : "off");
    return true;
}

void Shell::dispatch(const io::ControlEvent& e)
{
    nowMs_ = std::max(nowMs_, e.timeMs);
    changedSinceFrame_ = true;
    interruptAnimations();
    eraseClosedPopups();
    auto ctx = context(theme::palette(state_.skin), nowMs_);

    // State every layer needs first.
    if (e.control == io::Control::Shift && (e.kind == Kind::Press || e.kind == Kind::Release))
    {
        state_.shift = e.kind == Kind::Press;
        return;
    }
    if (io::isStep(e.control) && (e.kind == Kind::Press || e.kind == Kind::Release))
    {
        const int n = io::indexOf(e.control);
        if (e.kind == Kind::Press)
            state_.stepHeld = n;
        else if (state_.stepHeld == n)
            state_.stepHeld = 0; // releasing the step returns to normal at once (UID-166)
        if (body_)
            body_->handle(e, ctx);
        return;
    }
    if (e.control == io::Control::Home && e.kind == Kind::Press)
    {
        if (state_.shift)
        {
            open(kUtility); // SHIFT + HOME = UTILITY (UI-120)
            return;
        }
        log("INPUT home");
        home();
        return;
    }
    // The extension jump keys (UI-102, UI-403) act like HOME on any screen — popups close, the target
    // opens; SHIFT + ext.tap opens the BPM/QUANTIZE popup (our choice: the reference reaches it only by touch, UI-408).
    if (e.kind == Kind::Press && jumpKey(e.control))
        return;
    if (e.kind == Kind::Press && e.control == io::Control::ExtTap && state_.shift)
    {
        if (topPopup() == nullptr)
        {
            log("INPUT key shift+ext.tap -> %.*s", static_cast<int>(kBpmQuantize.size()), kBpmQuantize.data());
            open(kBpmQuantize);
        }
        return; // SHIFT + TAP never taps (the root keeps it from the transport too)
    }

    // The top popup first (UID-913: what it does not consume still reaches the screen).
    if (toast_ && touch::isTouch(e) && toast_->handle(e, ctx))
        return;
    if (auto* top = topPopup())
    {
        const bool encoder = io::isKnob(e.control) && e.kind == Kind::Turn;
        if (encoder && ! top->bindings().empty())
        {
            if (top->swallowsEncoders() && top->handle(e, ctx))
                return;
            const int n0 = io::indexOf(e.control) - 1;
            resolveStrip();
            updateStripModel(nowMs_);
            turnEncoder(n0, e.value, e.timeMs);
            return;
        }
        if (! encoder && ! io::isPad(e.control) && e.control != io::Control::Play && e.control != io::Control::Stop &&
            e.control != io::Control::Rec)
        {
            if (e.control == io::Control::Back && e.kind == Kind::Press)
                log("INPUT back (popup)");
            else if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
                log("INPUT sel.push%s (popup)", state_.shift ? " shift" : "");
            else if (e.control == io::Control::Selector && e.kind == Kind::Turn)
                log("INPUT sel %+d (popup)", e.value);
            top->handle(e, ctx);
            eraseClosedPopups();
            return; // a modal popup takes every non-performance event
        }
    }

    switch (e.kind)
    {
    case Kind::Press:
        if (io::isPad(e.control))
        {
            const int n = io::indexOf(e.control);
            state_.tracks[static_cast<std::size_t>(n - 1)].lastHitMs = e.timeMs;
            log("INPUT pad%d down velocity=%d", n, e.value);
            if (! (body_ && body_->handle(e, ctx)))
                selectTrack(n); // default pad mode: pads play and select (home.md)
            return;
        }
        if (e.control == io::Control::Back && state_.shift && cautionCount_ > 0)
        {
            showCautions(); // SHIFT + BACK opens the CAUTIONS list (popups-and-feedback.md)
            return;
        }
        if (e.control == io::Control::Back)
        {
            log("INPUT back");
            back();
            return;
        }
        if (e.control == io::Control::SelectorPush)
        {
            const bool bodyShiftPush = focus_.focused < 0 && railOwner_ != nullptr && railOwner_->ownsShiftPush();
            if (state_.shift && ! bodyShiftPush && toggleRailFocus())
                return;
            if (focus_.focused >= 0 && focus_.focused < static_cast<int>(rail_.size()))
                activateRail(focus_.focused);
            else if (body_)
                body_->handle(e, ctx);
            return;
        }
        if (body_)
            body_->handle(e, ctx);
        return;
    case Kind::Turn:
        if (io::isKnob(e.control))
        {
            updateStripModel(nowMs_);
            turnEncoder(io::indexOf(e.control) - 1, e.value, e.timeMs);
            return;
        }
        if (e.control == io::Control::Selector)
        {
            // The rotary moves the screen's one focus (UID-150): within the rail while it holds the focus, else
            // the body; a body that does not use the rotary hands it to the rail.
            if (focus_.focused >= 0 || ! (body_ && body_->handle(e, ctx)))
                moveRailFocus(e.value);
            return;
        }
        if (body_)
            body_->handle(e, ctx);
        return;
    case Kind::Touch:
        if (touch::isTouch(e))
            handleTouch(e, ctx);
        return;
    default:
        if (body_)
            body_->handle(e, ctx);
        return;
    }
}

bool Shell::typeText(std::string_view utf8)
{
    if (auto* top = topPopup())
        return top->text(utf8);
    return false;
}

bool Shell::textKey(TextKey k)
{
    if (auto* top = topPopup())
    {
        const bool used = top->textKey(k);
        eraseClosedPopups();
        return used;
    }
    return false;
}

// --- popups ---------------------------------------------------------------------------------------------------------
int Shell::popupDepth() const noexcept
{
    int n = 0;
    for (const auto& p : popups_)
        n += p->closed ? 0 : 1;
    return n;
}

PopupWidget* Shell::topPopup() noexcept
{
    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
        if (! (*it)->closed)
            return it->get();
    return nullptr;
}

bool Shell::popupOpen() const
{
    return popupDepth() > 0;
}

void Shell::dismissTop()
{
    if (auto* top = topPopup())
        top->cancel();
    eraseClosedPopups();
}

void Shell::dismissAll()
{
    for (auto it = popups_.rbegin(); it != popups_.rend(); ++it)
        if (! (*it)->closed && ! (*it)->blocksHome())
            (*it)->cancel();
    for (auto& p : popups_)
        if (! p->blocksHome())
            p->closed = true; // HOME closes without applying (UI-005)
    eraseClosedPopups();
}

PopupWidget* Shell::pushPopup(std::unique_ptr<PopupWidget> p)
{
    if (p == nullptr)
        return nullptr;
    if (popupDepth() >= kMaxPopupDepth && p->kind() != PopupSpec::Kind::Message)
    {
        log("POPUP refused: nesting depth %d", kMaxPopupDepth);
        return nullptr;
    }
    p->openedMs = nowMs_;
    p->state = &state_;
    p->shell = this;
    auto* raw = p.get();
    popups_.push_back(std::move(p));
    resolveStrip();
    changedSinceFrame_ = true;
    return raw;
}

PopupWidget* Shell::showPopup(std::unique_ptr<PopupWidget> p)
{
    return pushPopup(std::move(p));
}

void Shell::showList(ListRequest r)
{
    pushPopup(std::make_unique<ListPopup>(std::move(r)));
}
void Shell::showSetting(SettingRequest r)
{
    pushPopup(std::make_unique<SettingPopup>(std::move(r)));
}
void Shell::showGrid(GridRequest r)
{
    pushPopup(std::make_unique<GridPopup>(std::move(r)));
}
void Shell::showMessage(MessageRequest r)
{
    pushPopup(std::make_unique<Message>(std::move(r)));
}
void Shell::showDialog(DialogRequest r)
{
    pushPopup(std::make_unique<Dialog>(std::move(r)));
}

Progress* Shell::showProgress(ProgressRequest r)
{
    return static_cast<Progress*>(pushPopup(std::make_unique<Progress>(std::move(r))));
}

Toast* Shell::showToast(ToastRequest r)
{
    log("TOAST %s", r.text.c_str()); // one trace line per toast (the live set's evidence of a result)
    toast_ = std::make_unique<Toast>(std::move(r)); // a new toast replaces the current one
    toast_->openedMs = nowMs_;
    toast_->state = &state_;
    changedSinceFrame_ = true;
    return toast_.get();
}

void Shell::setCaution(std::string_view id, std::string_view text)
{
    clearCaution(id);
    if (cautionCount_ == cautions_.size())
    {
        std::move(cautions_.begin() + 1, cautions_.end(), cautions_.begin());
        --cautionCount_;
    }
    cautions_[cautionCount_++] = {id, text};
    changedSinceFrame_ = true;
}

void Shell::clearCaution(std::string_view id)
{
    for (std::size_t i = 0; i < cautionCount_; ++i)
        if (cautions_[i].id == id)
        {
            std::move(cautions_.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                      cautions_.begin() + static_cast<std::ptrdiff_t>(cautionCount_),
                      cautions_.begin() + static_cast<std::ptrdiff_t>(i));
            --cautionCount_;
            changedSinceFrame_ = true;
            return;
        }
}

void Shell::showCautions()
{
    // CAUTIONS list: one row per cause, newest focused (UID-909); the actions per cause belong to the owners.
    ListRequest r;
    r.title = "CAUTIONS";
    for (std::size_t i = 0; i < cautionCount_; ++i)
        r.items.push_back({cautions_[i].text, {}, {}, static_cast<int>(i)});
    r.current = static_cast<int>(cautionCount_) - 1;
    showList(std::move(r));
}

bool Shell::present(const PopupSpec& spec)
{
    // A package's own popup widget for this id (PopupFactory.h: SELECT FX's populated grid).
    if (const auto make = PopupFactoryRegistry::builtin().find(spec.id))
        if (auto popup = make(spec, *this))
            return pushPopup(std::move(popup)) != nullptr;
    const auto* bound = spec.list != nullptr && ! spec.list->binds.empty() ? params_.find(spec.list->binds) : nullptr;
    auto fill = [&](std::vector<ListItem>& items)
    {
        if (spec.list == nullptr)
            return;
        if (const auto source = ListSourceRegistry::builtin().find(spec.list->source))
            source(spec.list->argument, items);
    };
    switch (spec.kind)
    {
    case PopupSpec::Kind::List:
    {
        ListRequest r;
        r.title = spec.title;
        fill(r.items);
        r.grouped = spec.list != nullptr && spec.list->grouped;
        if (bound != nullptr)
        {
            const auto scope = scopeOf(*bound);
            const int v = static_cast<int>(std::lround(values_->get(*bound, scope)));
            for (std::size_t i = 0; i < r.items.size(); ++i)
                if (r.items[i].value == v)
                    r.current = static_cast<int>(i);
            r.onChoose = [this, bound, scope](int, const ListItem& it)
            { values_->set(*bound, scope, static_cast<float>(it.value)); };
        }
        showList(std::move(r));
        return true;
    }
    case PopupSpec::Kind::Grid:
    {
        GridRequest r;
        r.title = spec.title;
        std::vector<ListItem> items;
        fill(items);
        for (const auto& it : items)
            r.items.push_back({it.label, it.icon, it.enabled});
        if (bound != nullptr)
        {
            const auto scope = scopeOf(*bound);
            const int v = static_cast<int>(std::lround(values_->get(*bound, scope)));
            for (std::size_t i = 0; i < items.size(); ++i)
                if (items[i].value == v)
                    r.current = static_cast<int>(i);
            r.onChoose = [this, bound, scope, items](int i)
            {
                if (i >= 0 && i < static_cast<int>(items.size()))
                    values_->set(*bound, scope, static_cast<float>(items[static_cast<std::size_t>(i)].value));
            };
        }
        showGrid(std::move(r));
        return true;
    }
    case PopupSpec::Kind::Setting:
    {
        SettingRequest r;
        r.title = spec.title;
        std::vector<const core::ParamSpec*> specs;
        if (spec.settings != nullptr)
            for (const auto& section : SettingsRegistry::builtin().sections(spec.settings->id))
                for (const auto& row : SettingsRegistry::builtin().rows(spec.settings->id, section.id))
                {
                    if (row.kind != SettingsRowSpec::Kind::Value || ! flags_.enabled(row.featureFlag))
                        continue;
                    const auto* p = params_.find(row.binds);
                    if (p == nullptr)
                        continue;
                    SettingRow sr;
                    sr.label = row.label;
                    const float v = values_->get(*p, scopeOf(*p));
                    if (p->type == core::ParamType::Enum)
                    {
                        sr.options.assign(p->enumLabels.begin(), p->enumLabels.end());
                        sr.current = static_cast<int>(std::lround(v - p->minValue));
                    }
                    else
                    {
                        sr.spec = p;
                        sr.value = v;
                    }
                    r.rows.push_back(std::move(sr));
                    specs.push_back(p);
                }
        r.onCommit = [this, specs](int row, int option, float value)
        {
            const auto* p = specs[static_cast<std::size_t>(row)];
            values_->set(*p, scopeOf(*p),
                         p->type == core::ParamType::Enum ? p->minValue + static_cast<float>(option) : value);
        };
        showSetting(std::move(r));
        return true;
    }
    case PopupSpec::Kind::Modal:
    {
        std::unique_ptr<Widget> body;
        if (const auto make = WidgetRegistry::builtin().find(spec.bodyComponent))
            body = make(router_.current(), WidgetArgs{L::modalPanel, spec.id, &params_, this, &state_});
        pushPopup(std::make_unique<Modal>(spec.title, std::move(body), spec.bindings));
        return true;
    }
    case PopupSpec::Kind::Dialog:
    {
        DialogRequest r;
        r.title.assign(spec.title);
        showDialog(std::move(r));
        return true;
    }
    case PopupSpec::Kind::Message:
    {
        MessageRequest r;
        r.title.assign(spec.title);
        showMessage(std::move(r));
        return true;
    }
    case PopupSpec::Kind::Toast:
    {
        ToastRequest r;
        r.text.assign(spec.title);
        showToast(std::move(r));
        return true;
    }
    }
    return false;
}
} // namespace ui
