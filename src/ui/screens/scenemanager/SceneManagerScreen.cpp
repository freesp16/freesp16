// SCENE MANAGER (07-ui-design/screens/scene-manager.md; UI-280, UI-284…288, UID-600…612). Template rail+content,
// context bar menu with the SCENE | PATTERN item lit (the shell does that for the id `sceneManager`; a tap on it
// closes the screen, UID-612). One screen whose rail items PATTERN · SEQ. · TRACK are the shell's (RailOwner: the
// body draws them as RailTabs and switches the tab; slot 4 stays free); the selected tab, the clipboard and INCLUDE
// SEQUENCE are remembered for the session.
//   PATTERN: 16 SceneKeys of the scenes over 16 of the focused scene's patterns; two-level selection (turn over scenes,
//     push into the patterns, turn and push to commit; tap = focus, tap again = enter / commit, UID-603); the commit
//     posts `scene.queue` (scene × 16 + pattern) — queued to the switch boundary while playing (pending key blinking
//     at 2 Hz from the PlayheadFeed until the engine switches), immediate when stopped. Closing before a commit
//     changes nothing. COPY / PASTE / DELETE act on the focused scene or pattern (PASTE disabled for an empty clipboard
//     or the other kind, UI-286; the copied key carries the copy glyph); deleting a scene or the playing pattern asks
//     (UI-287); results are toasts (UID-611). SCENE BPM (its own popup through ShellApi::showPopup: set, OFF, USE
//     CURRENT) and FILE (EXPORT / IMPORT SCENE, P2: disabled).
//     Strip `SCENE nn`: NAME (turning opens the name dialog) and COLOR of the focused scene (UID-609).
//   SEQ.: a read-only four-bar overview of one pattern for all 16 tracks (cached geometry, rebuilt on change), a
//     selection rectangle (track row or ALL × BAR LENGTH from the start bar; a tap on a ruler bar or a drag over the
//     rows moves the start) that COPY / PASTE / DELETE use; strip SCENE No., PATTERN No., TRACK, TRACK No., BAR LENGTH
//     (UI state).
//   TRACK: 2 × 8 scene keys and a 4 × 4 track map in pad order; a tap (or a pad) chooses the track; COPY / PASTE /
//     DELETE disabled until one is chosen; INCLUDE SEQUENCE toggle; strip TRACK (CURRENT / ALL).
// Scene and pattern edits are core::commands::scene (replaceScene / replacePattern / clearScene /
// clearPattern / setSceneBpm / setSceneName / setSceneColour); the commit posts the real scene.queue (scene.cpp).
// COPY / PASTE / DELETE (and SHIFT + PASTE = swap) go through core::edit::SceneEditor (the session's
// one clipboard, async pastes, one undo step each); the local Clipboard keeps only what the buttons and marks show.
// Interim adapters: BACK always closes the
// screen (the shell handles BACK before the body, so "BACK from the pattern row returns to the scene row" needs a
// shell hook); the scene NAME / COLOR columns mirror the focused scene through ParamAccess (scope = the current
// scene) until a model-backed ParamAccess lands.
#include "core/commands/CommandBusAccess.h"
#include "core/commands/scene/SceneCommands.h"
#include "core/edit/SharedEditor.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/scenemanager/SceneManagerAccess.h"
#include "ui/screens/sequence/M2Commands.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Feeds.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/theme/Draw.h"
#include "ui/theme/Glyphs.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/SceneKey.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace scmd = core::commands::scene;
using Kind = io::ControlEvent::Kind;
using core::model::Pattern;
using core::model::Scene;
using core::model::Track;

// --- descriptors ------------------------------------------------------------------------------------------------
constexpr std::string_view kSceneTabs[16] = {"SCENE 01", "SCENE 02", "SCENE 03", "SCENE 04", "SCENE 05", "SCENE 06",
                                             "SCENE 07", "SCENE 08", "SCENE 09", "SCENE 10", "SCENE 11", "SCENE 12",
                                             "SCENE 13", "SCENE 14", "SCENE 15", "SCENE 16"};
constexpr StripSpec patternStrip(int i) noexcept
{
    return {kSceneTabs[i], {"", "", "", "", "scene.name", "scene.color"}, 0};
}
constexpr StripSpec kPatternStrips[16] = {patternStrip(0),  patternStrip(1),  patternStrip(2),  patternStrip(3),
                                          patternStrip(4),  patternStrip(5),  patternStrip(6),  patternStrip(7),
                                          patternStrip(8),  patternStrip(9),  patternStrip(10), patternStrip(11),
                                          patternStrip(12), patternStrip(13), patternStrip(14), patternStrip(15)};
constexpr StripSpec kSeqStrip{"SEQ.",
                              {"sceneManager.seq.scene", "sceneManager.seq.pattern", "sceneManager.seq.trackMode",
                               "sceneManager.seq.track", "sceneManager.seq.barLength", ""},
                              0};
constexpr StripSpec kTrackStrip{"TRACK", {"sceneManager.track.mode", "", "", "", "", ""}, 0};

// --- geometry (scene-manager.md "Layout") --------------------------------------------------------------------------
constexpr RectF kTab(int i) noexcept
{
    return {16, 152 + 118 * static_cast<float>(i), 192, 102};
}
constexpr RectF kCopy{224, 528, 192, 80}, kPaste{432, 528, 192, 80}, kDelete{640, 528, 192, 80},
    kSlot4{864, 528, 192, 80}, kSlot5{1072, 528, 192, 80};
constexpr RectF patternKey(int row, int n) noexcept // row 0 scenes, 1 patterns; n 1–16
{
    return {236 + 64 * static_cast<float>(n - 1), row == 0 ? 184.0f : 320.0f, 56, 88};
}
constexpr RectF trackTabScene(int n) noexcept
{
    return {224 + 64 * static_cast<float>((n - 1) % 8), 328 + 88 * static_cast<float>((n - 1) / 8), 56, 80};
}
constexpr RectF trackCell(int t) noexcept // pad order: 13–16 on top
{
    return {744 + 132 * static_cast<float>((t - 1) % 4), 184 + 80 * static_cast<float>(3 - (t - 1) / 4), 124, 72};
}
constexpr float kGx = 288, kStepW = 15, kRowY0 = 212, kRowH = 18;
constexpr const char* kNumbers[] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                    "9", "10", "11", "12", "13", "14", "15", "16"};
constexpr const char* kBeatLabels[16] = {"1.1", "1.2", "1.3", "1.4", "2.1", "2.2", "2.3", "2.4",
                                         "3.1", "3.2", "3.3", "3.4", "4.1", "4.2", "4.3", "4.4"};
constexpr RailItemSpec kTabs[] = {{RailItemSpec::Kind::Button, "PATTERN"},
                                  {RailItemSpec::Kind::Button, "SEQ."},
                                  {RailItemSpec::Kind::Button, "TRACK"}};

// --- session state (remembered while the app runs, UID-600 "remembered per project session") ----------------------
struct Clipboard
{
    enum class Kind : std::uint8_t
    {
        None,
        Scene,
        Pattern,
        Sequence,
        Track
    } kind = Kind::None;
    int scene = 0, pattern = 0, track = 0; // 0-based source
    std::uint16_t trackMask = 0;           // Sequence: the tracks copied
    int firstStep = 0, steps = 0;          // Sequence: the range copied
    bool withSequence = false;             // Track
};
struct Memory
{
    int tab = 0;
    bool includeSequence = true;
    Clipboard clip;
    int forcedQueue = -1;
};
Memory& memory()
{
    static Memory m;
    return m;
}

void toast(ShellApi& shell, std::string_view text)
{
    ToastRequest t;
    t.text.assign(text);
    shell.showToast(std::move(t));
}

std::uint32_t sceneColour(int scene0)
{
    return theme::trackColour(m2::Session::instance().project().scenes[static_cast<std::size_t>(scene0 & 15)].colour +
                              1);
}

// SCENE BPM popup (scene-manager.md "Popups"): a Message-sized panel 640 × 320, centred: the value in `display`
// (`124.0`, or `---` for no scene BPM) with the stepper's + / − marks, the rotary ±1 BPM (SHIFT ±0.1, SYN-002), a
// vertical drag on the value turns it (8 lpx per detent); OFF clears the field, USE CURRENT takes the project tempo,
// OK (primary, also push) commits, CANCEL / BACK close without a change.
class SceneBpmPopup final : public PopupWidget
{
public:
    static constexpr RectF kPanel{320, 224, 640, 320};
    using Commit = std::function<void(std::optional<double>)>;
    SceneBpmPopup(std::optional<double> bpm, double current, Commit commit)
        : PopupWidget(PopupSpec::Kind::Modal), value_(bpm), current_(current), commit_(std::move(commit))
    {
    }
    RectF bounds() const noexcept override { return place(kPanel); }
    std::optional<double> value() const noexcept { return value_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        const auto b = bounds();
        draw::panel(dl, p, b);
        draw::titleBand(dl, p, b, 48, "SCENE BPM", true, true, true);
        const RectF field = valueRect();
        dl.fill(field, p.surface2, tk::radiusM);
        if (value_)
            text_.format("%.1f", *value_);
        else
            text_.set("---");
        dl.text(field.x + field.w / 2, field.y + field.h / 2, Align::Centre, TextStyleId::Display,
                value_ ? p.text1 : p.text2, text_.view());
        dl.text(field.x + field.w - 32, field.y + 24, Align::Centre, TextStyleId::Title, p.text2, "+");
        dl.text(field.x + field.w - 32, field.y + field.h - 24, Align::Centre, TextStyleId::Title, p.text2,
                "\xE2\x88\x92");
        draw::focusRing(dl, p, field, tk::radiusM); // the value has the focus (default focus)
        static constexpr std::string_view labels[4] = {"OFF", "USE CURRENT", "CANCEL", "OK"};
        for (int i = 0; i < 4; ++i)
            Button::draw(dl, ctx, button(i),
                         {labels[i], i == 3 ? ButtonStyle::Primary : ButtonStyle::Plain, false, pressed_ == i});
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        const bool fine = state != nullptr && state->shift;
        if (e.control == io::Control::Selector && e.kind == io::ControlEvent::Kind::Turn)
        {
            step(e.value, fine);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == io::ControlEvent::Kind::Press)
        {
            finish(3);
            return true;
        }
        if (e.control == io::Control::Back && e.kind == io::ControlEvent::Kind::Press)
        {
            cancel();
            return true;
        }
        if (! touch::isTouch(e))
            return false;
        const float x = touch::x(e), y = touch::y(e);
        if (e.value == touch::kDrag && dragging_) // a vertical drag on the value turns it
        {
            const int detents = static_cast<int>((dragY_ - y) / (8 * touch::kToCanvas));
            if (detents != 0)
            {
                step(detents, fine);
                dragY_ -= static_cast<float>(detents) * 8 * touch::kToCanvas;
            }
            return true;
        }
        if (e.value == touch::kUp)
        {
            dragging_ = false;
            pressed_ = -1;
            return true;
        }
        if (e.value != touch::kDown)
            return true;
        const auto b = bounds();
        if (y < b.y + 48 && x >= b.x + b.w - 56)
        {
            cancel(); // ✕
            return true;
        }
        const RectF field = valueRect();
        if (contains(field, x, y))
        {
            if (x >= field.x + field.w - 64) // the stepper's + / − halves
                step(y < field.y + field.h / 2 ? 1 : -1, fine);
            dragging_ = true;
            dragY_ = y;
            return true;
        }
        for (int i = 0; i < 4; ++i)
            if (contains(button(i), x, y))
            {
                pressed_ = i;
                finish(i);
            }
        return true; // a tap outside: ignored (modal)
    }

private:
    RectF valueRect() const noexcept
    {
        const auto b = bounds();
        return {b.x + 24, b.y + 72, b.w - 48, 144};
    }
    RectF button(int i) const noexcept
    {
        const auto b = bounds();
        const float w = (b.w - 48 - 3 * 8) / 4;
        return {b.x + 24 + (w + 8) * static_cast<float>(i), b.y + b.h - 72, w, 48};
    }
    void step(int detents, bool fine)
    {
        const double base = value_ ? *value_ : current_;
        const double v = std::clamp(base + detents * (fine ? 0.1 : 1.0), 20.0, 300.0);
        value_ = std::round(v * 10.0) / 10.0;
    }
    void finish(int i)
    {
        switch (i)
        {
        case 0:
            value_.reset(); // OFF
            return;
        case 1:
            value_ = std::round(current_ * 10.0) / 10.0; // USE CURRENT
            return;
        case 2:
            cancel();
            return;
        default:
            closed = true;
            if (commit_)
                commit_(value_);
            return;
        }
    }

    std::optional<double> value_;
    double current_;
    Commit commit_;
    int pressed_ = -1;
    bool dragging_ = false;
    float dragY_ = 0;
    mutable FixedText<12> text_;
};

class SceneManagerBody final : public Widget, public RailOwner
{
public:
    explicit SceneManagerBody(const WidgetArgs& a) : shell_(a.shell), state_(a.state)
    {
        if (state_ != nullptr)
        {
            focusScene_ = state_->scene;
            focusPattern_ = state_->pattern;
        }
        hits_.reserve(1024);
    }
    ~SceneManagerBody() override
    {
        if (shell_ != nullptr)
            shell_->bindStrip(nullptr);
    }
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const_cast<SceneManagerBody*>(this)->sync();
        switch (memory().tab)
        {
        case 0:
            drawPatternTab(dl, ctx);
            break;
        case 1:
            drawSeqTab(dl, ctx);
            break;
        default:
            drawTrackTab(dl, ctx);
            break;
        }
    }
    bool dirty(double) const override { return pending().first > 0; }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        auto& mem = memory();
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (state_->shift) // SHIFT + rotary changes the tab (our choice)
            {
                setTab(std::clamp(mem.tab + (e.value > 0 ? 1 : -1), 0, 2));
                return true;
            }
            if (mem.tab == 0)
            {
                if (level_ == 0)
                    focusScene_ = std::clamp(focusScene_ + e.value, 1, 16);
                else
                    focusPattern_ = std::clamp(focusPattern_ + e.value, 1, 16);
                return true;
            }
            if (mem.tab == 2)
            {
                focusScene_ = std::clamp(focusScene_ + e.value, 1, 16);
                return true;
            }
            if (mem.tab == 1) // the rotary moves the selection's start bar (a tap on the ruler too)
            {
                seqStartBar_ = std::clamp(seqSelection().first + e.value, 0, 3);
                return true;
            }
            return false;
        }
        // COPY / PASTE / DELETE from the keyboard (UI-102: ext.copy, ext.paste, ext.clear = Cmd+C, Cmd+V, Delete), as
        // the buttons: disabled buttons do nothing; SHIFT + PASTE swaps.
        if (e.kind == Kind::Press
            && (e.control == io::Control::ExtCopy || e.control == io::Control::ExtPaste || e.control == io::Control::ExtClear))
        {
            const auto b = buttons();
            if (e.control == io::Control::ExtCopy && b[0])
                copy();
            else if (e.control == io::Control::ExtPaste && b[1])
                paste();
            else if (e.control == io::Control::ExtClear && b[2])
                remove();
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            if (mem.tab == 0)
                enter();
            return true;
        }
        if (io::isPad(e.control) && e.kind == Kind::Press && mem.tab == 2)
        {
            chosenTrack_ = io::indexOf(e.control); // a pad chooses the track (the shell still selects it)
            return false;
        }
        if (touch::isTouch(e))
        {
            if (e.value == touch::kDown)
                return tap(touch::x(e), touch::y(e));
            if (e.value == touch::kDrag && draggingSelection_)
            {
                seqStartBar_ = std::clamp(static_cast<int>((touch::x(e) - kGx) / 240), 0, 3);
                return true;
            }
            draggingSelection_ = false; // up
            return true;
        }
        return false;
    }

    // --- test access ---
    SceneManagerView view() const
    {
        SceneManagerView v;
        v.tab = memory().tab;
        v.level = level_;
        v.focusScene = focusScene_;
        v.focusPattern = focusPattern_;
        v.chosenTrack = chosenTrack_;
        const auto b = buttons();
        v.copyEnabled = b[0];
        v.pasteEnabled = b[1];
        v.deleteEnabled = b[2];
        const auto p = pending();
        v.pendingScene = p.first;
        v.pendingPattern = p.second;
        v.seqStartBar = seqSelection().first;
        return v;
    }
    SceneKeyModel key(bool patternRow, int n, double nowMs) const
    {
        return patternRow ? patternKeyModel(n, nowMs) : sceneKeyModel(n, nowMs, false);
    }

private:
    // ---- state ----
    std::pair<int, int> pending() const // 1-based pending scene / pattern, {0, 0} = none
    {
        const auto& feeds = shell_->feeds();
        if (feeds.live() && feeds.playhead().pendingPattern >= 0)
            return {(feeds.playhead().pendingScene >= 0 ? feeds.playhead().pendingScene : state_->scene - 1) + 1,
                    feeds.playhead().pendingPattern + 1};
        return pending_;
    }
    bool queued() const
    {
        if (! state_->playing)
            return false;
        const int forced = memory().forcedQueue;
        if (forced >= 0)
            return forced != 0;
        const auto* q = shell_->params().find("scene.queue");
        return q != nullptr && q->featureFlag != m2::kParamStubFlag;
    }
    std::array<bool, 3> buttons() const // COPY, PASTE, DELETE enabled
    {
        const auto kind = memory().clip.kind;
        switch (memory().tab)
        {
        case 0:
            return {true, kind == (level_ == 0 ? Clipboard::Kind::Scene : Clipboard::Kind::Pattern), true};
        case 1:
            return {true, kind == Clipboard::Kind::Sequence, true};
        default:
            return {chosenTrack_ > 0, chosenTrack_ > 0 && kind == Clipboard::Kind::Track, chosenTrack_ > 0};
        }
    }
    // The session's SceneEditor (one clipboard, EDT-010) on the ui::m2 session's bus; settle() makes an edit visible to
    // the cached views at once (the session's own bus has no notification scheduler).
    static core::edit::SceneEditor& editor() { return core::edit::sharedEditor(m2::Session::instance().bus()); }
    static void settle()
    {
        core::commands::flushNotifications(m2::Session::instance().bus());
        m2::Session::instance().touch();
    }
    // Pastes onto `dst`, toasting `text` when it landed (async, EDT-004: the shell outlives this body, the body may
    // not).
    void pasteTo(const core::edit::EditRef& dst, std::string text)
    {
        editor().paste(dst, [shell = shell_, text = std::move(text)](core::edit::Result r) {
            settle();
            if (shell != nullptr)
                toast(*shell, r.ok() ? text : r.error().message);
        });
    }
    float param(std::string_view id) const
    {
        const auto* s = shell_->params().find(id);
        return s != nullptr ? shell_->values().get(*s, 0) : 0.0f;
    }
    void setParam(std::string_view id, float v)
    {
        if (const auto* s = shell_->params().find(id))
            shell_->values().set(*s, 0, v);
    }
    int seqScene() const { return static_cast<int>(std::lround(param("sceneManager.seq.scene"))); }     // 0-based
    int seqPattern() const { return static_cast<int>(std::lround(param("sceneManager.seq.pattern"))); } // 0-based
    bool seqAll() const { return std::lround(param("sceneManager.seq.trackMode")) != 0; }
    int seqTrack() const { return static_cast<int>(std::lround(param("sceneManager.seq.track"))); } // 0-based
    int seqBars() const
    {
        return std::max(1, (m2::Session::instance().patternLength(seqScene(), seqPattern()) + 15) / 16);
    }
    std::pair<int, int> seqSelection() const // first bar 0-based, bar count
    {
        const int bars = seqBars();
        const int start = std::clamp(seqStartBar_, 0, bars - 1);
        const int len =
            std::clamp(static_cast<int>(std::lround(param("sceneManager.seq.barLength"))) + 1, 1, bars - start);
        return {start, len};
    }

    // Per frame: strip binding per tab, the NAME / COLOR mirror, pending resolution, TRACK No. following the selection.
    void sync()
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        auto& s = m2::Session::instance();
        const int tab = memory().tab;
        const StripSpec* want = tab == 0   ? &kPatternStrips[(focusScene_ - 1) & 15]
                                : tab == 1 ? &kSeqStrip
                                           : &kTrackStrip;
        if (want != boundStrip_)
        {
            shell_->bindStrip(nullptr); // the shell re-reads a strip only when its pointer changes
            shell_->bindStrip(want);
            boundStrip_ = want;
        }
        // Resolve a local pending switch once the engine reports it done.
        const auto& feeds = shell_->feeds();
        if (pending_.first > 0 && feeds.live() && feeds.playhead().pendingPattern < 0 &&
            feeds.playhead().scene + 1 == pending_.first && feeds.playhead().pattern + 1 == pending_.second)
        {
            state_->scene = pending_.first;
            state_->pattern = pending_.second;
            pending_ = {0, 0};
        }
        // NAME / COLOR of the focused scene (scope = the current scene's slot, see the header).
        const auto& scene = s.project().scenes[static_cast<std::size_t>(focusScene_ - 1)];
        scmd::setFocusedSceneName(scene.name);
        if (const auto* c = shell_->params().find("scene.color"))
        {
            const auto scope = static_cast<std::uint16_t>(state_->scene - 1);
            const float now = shell_->values().get(*c, scope);
            if (mirrorScene_ == focusScene_ && now != colourWritten_)
                s.execute(scmd::setSceneColour(focusScene_ - 1, static_cast<int>(std::lround(now))));
            const float shown =
                static_cast<float>(s.project().scenes[static_cast<std::size_t>(focusScene_ - 1)].colour);
            if (shown != now)
                shell_->values().set(*c, scope, shown);
            colourWritten_ = shown;
            mirrorScene_ = focusScene_;
        }
        if (const auto* n = shell_->params().find("scene.name"))
        {
            const auto scope = static_cast<std::uint16_t>(state_->scene - 1);
            const float now = shell_->values().get(*n, scope);
            if (nameSeen_ && now != nameValue_)
                askName();
            nameValue_ = now;
            nameSeen_ = true;
        }
        if (tab == 1 && ! seqAll())
            if (const int t = state_->selectedTrack - 1; t != seqTrack())
                setParam("sceneManager.seq.track", static_cast<float>(t)); // TRACK = CURRENT follows the selection
        if (tab == 2 && overviewRevision_ != s.revision() + 1000u * static_cast<unsigned>(focusScene_))
            trackContent();
    }

    void setTab(int t)
    {
        memory().tab = t;
        level_ = 0;
    }

    // ---- PATTERN tab actions ----
    void enter()
    {
        if (level_ == 0)
        {
            level_ = 1;
            focusPattern_ = focusScene_ == state_->scene ? state_->pattern : 1;
            return;
        }
        commit(focusScene_, focusPattern_);
    }
    void commit(int scene, int pattern)
    {
        if (const auto* q = shell_->params().find("scene.queue"))
            shell_->values().set(*q, 0, static_cast<float>((scene - 1) * 16 + (pattern - 1)));
        if (queued())
        {
            if (scene == state_->scene && pattern == state_->pattern)
                pending_ = {0, 0}; // selecting the current pattern again cancels (SCN-021)
            else
                pending_ = {scene, pattern};
            return;
        }
        state_->scene = scene;
        state_->pattern = pattern;
        pending_ = {0, 0};
    }

    void copy()
    {
        auto& clip = memory().clip;
        clip = Clipboard{};
        char text[64];
        switch (memory().tab)
        {
        case 0:
            if (level_ == 0)
            {
                clip.kind = Clipboard::Kind::Scene;
                clip.scene = focusScene_ - 1;
                editor().copy(core::edit::sceneRef(clip.scene));
                std::snprintf(text, sizeof text, "Scene %d copied", focusScene_);
            }
            else
            {
                clip.kind = Clipboard::Kind::Pattern;
                clip.scene = focusScene_ - 1;
                clip.pattern = focusPattern_ - 1;
                editor().copy(core::edit::patternRef(clip.scene, clip.pattern));
                std::snprintf(text, sizeof text, "Pattern %d copied", focusPattern_);
            }
            break;
        case 1:
        {
            const auto [start, len] = seqSelection();
            clip.kind = Clipboard::Kind::Sequence;
            clip.scene = seqScene();
            clip.pattern = seqPattern();
            clip.track = seqTrack();
            clip.trackMask = seqAll() ? 0xFFFF : static_cast<std::uint16_t>(1u << seqTrack());
            clip.firstStep = start * 16;
            clip.steps = len * 16;
            editor().copy(core::edit::stepsRef(clip.scene, clip.pattern, seqAll() ? core::edit::kAllTracks : clip.track,
                                               core::edit::stepRange(clip.firstStep, clip.steps)));
            std::snprintf(text, sizeof text, "%d bar%s copied", len, len == 1 ? "" : "s");
            break;
        }
        default:
            clip.kind = Clipboard::Kind::Track;
            clip.scene = focusScene_ - 1;
            clip.track = chosenTrack_ - 1;
            clip.withSequence = memory().includeSequence;
            editor().copy(core::edit::trackRef(clip.scene, clip.track, clip.withSequence));
            std::snprintf(text, sizeof text, "Track %d copied%s", chosenTrack_,
                          clip.withSequence ? " (with sequence)" : "");
            break;
        }
        toast(*shell_, text);
    }

    void paste()
    {
        namespace ed = core::edit;
        auto& clip = memory().clip;
        char text[64];
        switch (clip.kind)
        {
        case Clipboard::Kind::Scene:
            if (state_->shift && clip.scene != focusScene_ - 1) // SHIFT + PASTE swaps (EDT-002; our choice)
            {
                editor().swap(ed::sceneRef(clip.scene), ed::sceneRef(focusScene_ - 1));
                settle();
                std::snprintf(text, sizeof text, "Scene %d swapped with scene %d", clip.scene + 1, focusScene_);
                break;
            }
            std::snprintf(text, sizeof text, "Scene %d pasted to scene %d", clip.scene + 1, focusScene_);
            pasteTo(ed::sceneRef(focusScene_ - 1), text);
            return;
        case Clipboard::Kind::Pattern:
            if (state_->shift)
            {
                editor().swap(ed::patternRef(clip.scene, clip.pattern),
                              ed::patternRef(focusScene_ - 1, focusPattern_ - 1));
                settle();
                std::snprintf(text, sizeof text, "Pattern %d swapped", focusPattern_);
                break;
            }
            std::snprintf(text, sizeof text, "Pattern %d pasted to scene %d", focusPattern_, focusScene_);
            pasteTo(ed::patternRef(focusScene_ - 1, focusPattern_ - 1), text);
            return;
        case Clipboard::Kind::Sequence:
        {
            const auto [start, len] = seqSelection();
            (void)len;
            // One track onto another (the SEQ. track), all tracks onto all; wraps at the pattern length (EDT-005).
            std::snprintf(text, sizeof text, "Sequence pasted to scene %d, pattern %d", seqScene() + 1,
                          seqPattern() + 1);
            pasteTo(ed::stepsRef(seqScene(), seqPattern(), seqAll() ? ed::kAllTracks : seqTrack(),
                                 std::uint64_t{1} << (start * 16)),
                    text);
            return;
        }
        case Clipboard::Kind::Track:
        {
            const int scene = focusScene_ - 1, track = chosenTrack_ - 1;
            auto run = [this, scene, track]
            {
                char t[64];
                std::snprintf(t, sizeof t, "Track %d pasted to scene %d", track + 1, scene + 1);
                pasteTo(core::edit::trackRef(scene, track, memory().clip.withSequence), t);
            };
            trackContent(); // the focused scene may have changed since the last frame
            if (trackHasContent(track))
            {
                char line[64];
                std::snprintf(line, sizeof line, "Replace track %d of scene %d?", track + 1, scene + 1);
                confirm("PASTE TRACK", line, "REPLACE", run);
            }
            else
                run();
            return;
        }
        case Clipboard::Kind::None:
            return;
        }
        toast(*shell_, text);
    }

    void remove()
    {
        namespace ed = core::edit; // EDT-012: delete = reset the slot to its default, undoable
        switch (memory().tab)
        {
        case 0:
            if (level_ == 0)
            {
                const int scene = focusScene_ - 1;
                char line[64];
                std::snprintf(line, sizeof line, "Delete scene %d and its 16 patterns?", focusScene_);
                confirm("DELETE SCENE", line, "DELETE",
                        [this, scene]
                        {
                            editor().reset(core::edit::sceneRef(scene));
                            settle();
                            char t[48];
                            std::snprintf(t, sizeof t, "Scene %d deleted", scene + 1);
                            toast(*shell_, t);
                        });
                return;
            }
            {
                const int scene = focusScene_ - 1, pattern = focusPattern_ - 1;
                auto run = [this, scene, pattern]
                {
                    editor().reset(core::edit::patternRef(scene, pattern));
                    settle();
                    char t[48];
                    std::snprintf(t, sizeof t, "Pattern %d deleted", pattern + 1);
                    toast(*shell_, t);
                };
                if (state_->playing && focusScene_ == state_->scene && focusPattern_ == state_->pattern)
                {
                    char line[64];
                    std::snprintf(line, sizeof line, "Delete pattern %d? It is playing.", focusPattern_);
                    confirm("DELETE PATTERN", line, "DELETE", run); // UI-287
                }
                else
                    run(); // undoable at once (UID-605)
            }
            return;
        case 1:
        {
            const auto [start, len] = seqSelection();
            editor().reset(ed::stepsRef(seqScene(), seqPattern(), seqAll() ? ed::kAllTracks : seqTrack(),
                                        ed::stepRange(start * 16, len * 16)));
            settle();
            toast(*shell_, "Sequence deleted");
            return;
        }
        default:
        {
            const int scene = focusScene_ - 1, track = chosenTrack_ - 1;
            const bool withSequence = memory().includeSequence;
            char line[64];
            std::snprintf(line, sizeof line, "Reset track %d of scene %d to defaults?", track + 1, scene + 1);
            confirm("DELETE TRACK", line, "RESET",
                    [this, scene, track, withSequence]
                    {
                        editor().reset(core::edit::trackRef(scene, track, withSequence));
                        settle();
                        toast(*shell_, "Track reset");
                    });
            return;
        }
        }
    }

    void confirm(std::string_view title, std::string_view line, std::string_view action, std::function<void()> run)
    {
        MessageRequest r;
        r.icon = glyph::Kind::Caution;
        r.title.assign(title);
        r.lines = {std::string(line)};
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {std::string(action), ButtonStyle::Destructive}};
        r.defaultButton = 0; // destructive: CANCEL first (popups-and-feedback.md)
        r.onResult = [run = std::move(run)](int button)
        {
            if (button == 1)
                run();
        };
        shell_->showMessage(std::move(r));
    }

    void sceneBpm()
    {
        const int scene = focusScene_ - 1;
        const auto bpm = m2::Session::instance().project().scenes[static_cast<std::size_t>(scene)].bpm;
        const auto* tempo = shell_->params().find("transport.tempo");
        const double current = tempo != nullptr ? static_cast<double>(shell_->values().get(*tempo, 0)) : 120.0;
        shell_->showPopup(
            std::make_unique<SceneBpmPopup>(bpm, current, [scene](std::optional<double> b)
                                            { m2::Session::instance().execute(scmd::setSceneBpm(scene, b)); }));
    }

    // --- RailOwner: the three tabs are the shell's rail items, drawn as RailTabs (scene-manager.md) ---
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        setTab(std::clamp(index, 0, 2)); // tabs act on the first tap
        return true;
    }
    bool railDraw(int index, const RailItemSpec& item, const RailItemState& st, DrawList& dl,
                  const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        const auto r = kTab(index);
        const bool sel = index == memory().tab;
        dl.fill(r, sel ? p.accentWeak : p.surface2, tk::radiusM);
        if (sel)
            dl.fill({r.x, r.y + 16, 3, 70}, p.accent, 1);
        dl.text(r.x + r.w / 2, r.y + 52, Align::Centre, TextStyleId::Label, sel ? p.text1 : p.text2, item.label);
        if (st.focused)
            draw::focusRing(dl, p, r, tk::radiusM);
        return true;
    }
    bool railFocusEnter(int) override { return true; } // past the last tab: the keys get the rotary again

    void file()
    {
        ListRequest r;
        r.title = "FILE";
        r.items = {{"EXPORT SCENE\xE2\x80\xA6", {}, {}, 0, false},
                   {"IMPORT SCENE\xE2\x80\xA6", {}, {}, 1, false}}; // P2
        shell_->showList(std::move(r));
    }

    void askName()
    {
        const int scene = focusScene_ - 1;
        DialogRequest r;
        r.title = "SCENE NAME";
        char prompt[32];
        std::snprintf(prompt, sizeof prompt, "Name of scene %02d:", focusScene_);
        r.prompt = prompt;
        r.text = m2::Session::instance().project().scenes[static_cast<std::size_t>(scene)].name;
        r.hint = "1\xE2\x80\x93"
                 "32 characters";
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {"OK", ButtonStyle::Primary}};
        r.validate = [](std::string_view t) -> std::string
        { return t.size() > 32 ? "At most 32 characters" : std::string(); };
        r.onResult = [scene](int button, std::string_view text)
        {
            if (button == 1)
                m2::Session::instance().execute(scmd::setSceneName(scene, std::string(text)));
        };
        shell_->showDialog(std::move(r));
    }

    bool tap(float x, float y)
    {
        auto& mem = memory();
        const auto b = buttons();
        if (contains(kCopy, x, y))
        {
            if (b[0])
                copy();
            return true;
        }
        if (contains(kPaste, x, y))
        {
            if (b[1])
                paste();
            return true;
        }
        if (contains(kDelete, x, y))
        {
            if (b[2])
                remove();
            return true;
        }
        if (mem.tab == 0)
        {
            if (contains(kSlot4, x, y))
                sceneBpm();
            else if (contains(kSlot5, x, y))
                file();
            for (int row = 0; row < 2; ++row)
                for (int n = 1; n <= 16; ++n)
                    if (contains(patternKey(row, n), x, y))
                    {
                        const bool focusedNow = level_ == row && (row == 0 ? focusScene_ : focusPattern_) == n;
                        level_ = row;
                        if (row == 0)
                            focusScene_ = n;
                        else
                            focusPattern_ = n;
                        if (focusedNow) // a tap on the focused key enters / commits (UI-284)
                            enter();
                        return true;
                    }
            return true;
        }
        if (mem.tab == 1)
        {
            if (y >= 180 && y < 208 && x >= kGx && x < kGx + 960)
                seqStartBar_ = static_cast<int>((x - kGx) / 240); // a tap on a ruler bar moves the start (our choice)
            else if (y >= kRowY0 - 16 && y < kRowY0 + 16 * kRowH && x >= kGx && x < kGx + 960)
                draggingSelection_ = true; // a drag on the overview moves the selection's start bar (‹ › handle)
            else if (x >= 232 && x < 264 && y >= kRowY0 && y < kRowY0 + 16 * kRowH)
                setParam("sceneManager.seq.track", std::floor((y - kRowY0) / kRowH));
            return true;
        }
        if (contains(kSlot5, x, y))
        {
            mem.includeSequence = ! mem.includeSequence;
            return true;
        }
        for (int n = 1; n <= 16; ++n)
            if (contains(trackTabScene(n), x, y))
            {
                focusScene_ = n;
                return true;
            }
        for (int t = 1; t <= 16; ++t)
            if (contains(trackCell(t), x, y))
            {
                chosenTrack_ = chosenTrack_ == t ? 0 : t;
                return true;
            }
        return true;
    }

    // ---- drawing ----
    void caption(DrawList& dl, const UiContext& ctx, float x, float y, float w, std::string_view text) const
    {
        const float cw = ctx.measure ? ctx.measure(TextStyleId::Caption, text) : 8.0f * static_cast<float>(text.size());
        draw::captionLine(dl, ctx.palette, x, y, w, text, cw, ctx.palette.bg);
    }

    void drawButtons(DrawList& dl, const UiContext& ctx) const
    {
        const auto b = buttons();
        Button::draw(dl, ctx, kCopy, {"COPY", ButtonStyle::Plain, ! b[0]});
        Button::draw(dl, ctx, kPaste, {"PASTE", ButtonStyle::Plain, ! b[1]});
        Button::draw(dl, ctx, kDelete, {"DELETE", ButtonStyle::Plain, ! b[2]});
    }

    bool blinkOn(double nowMs) const { return std::fmod(nowMs, 500.0) < 250.0; } // 2 Hz (UID-602)

    SceneKeyModel sceneKeyModel(int n, double nowMs, bool trackTab) const
    {
        auto& s = m2::Session::instance();
        const auto pend = pending();
        SceneKeyModel k;
        k.index = n;
        k.colour = sceneColour(n - 1);
        k.current = n == state_->scene;
        k.hasContent = s.sceneHasContent(n - 1);
        k.pending = pend.first == n && pend.first != state_->scene && blinkOn(nowMs);
        k.copied = memory().clip.kind == Clipboard::Kind::Scene && memory().clip.scene == n - 1;
        k.focused = trackTab ? n == focusScene_ : level_ == 0 && n == focusScene_;
        k.trail = ! trackTab && level_ == 1 && n == focusScene_;
        return k;
    }
    SceneKeyModel patternKeyModel(int n, double nowMs) const
    {
        auto& s = m2::Session::instance();
        const auto pend = pending();
        SceneKeyModel k;
        k.index = n;
        k.colour = sceneColour(focusScene_ - 1);
        k.current = focusScene_ == state_->scene && n == state_->pattern;
        k.hasContent = s.patternHasContent(focusScene_ - 1, n - 1);
        k.pending = pend.first == focusScene_ && pend.second == n && blinkOn(nowMs);
        k.copied = memory().clip.kind == Clipboard::Kind::Pattern && memory().clip.scene == focusScene_ - 1 &&
                   memory().clip.pattern == n - 1;
        k.focused = level_ == 1 && n == focusPattern_;
        return k;
    }

    void drawPatternTab(DrawList& dl, const UiContext& ctx) const
    {
        const auto& p = ctx.palette;
        auto& s = m2::Session::instance();
        caption(dl, ctx, 224, 168, 1040, "SCENE");
        for (int n = 1; n <= 16; ++n)
            SceneKey::draw(dl, ctx, patternKey(0, n), sceneKeyModel(n, ctx.nowMs, false));
        line_.format("PATTERN \xC2\xB7 SCENE %02d", focusScene_);
        caption(dl, ctx, 224, 304, 1040, line_.view());
        for (int n = 1; n <= 16; ++n)
            SceneKey::draw(dl, ctx, patternKey(1, n), patternKeyModel(n, ctx.nowMs));
        // Focus info (UID-608).
        const auto& scene = s.project().scenes[static_cast<std::size_t>(focusScene_ - 1)];
        info1_.format("SCENE %02d  %s", focusScene_, scene.name.c_str());
        dl.text(236, 448, Align::Left, TextStyleId::BodyStrong, p.text1, info1_.view());
        const int len = s.patternLength(focusScene_ - 1, focusPattern_ - 1);
        const int bars = (len + 15) / 16;
        const auto pend = pending();
        const bool pendingHere = pend.first == focusScene_ && pend.second == focusPattern_;
        const char* when = "";
        if (pendingHere)
        {
            const int unit = static_cast<int>(std::lround(param("project.patternQuantize")));
            when = unit == 0   ? "  \xC2\xB7  switching at the next step"
                   : unit == 1 ? "  \xC2\xB7  switching at the next bar"
                               : "  \xC2\xB7  switching at the end of the pattern";
        }
        info2_.format("PATTERN %02d  \xC2\xB7  %d bar%s \xC2\xB7 %d steps%s", focusPattern_, bars, bars == 1 ? "" : "s",
                      len, when);
        dl.text(236, 476, Align::Left, TextStyleId::Body, p.text2, info2_.view());

        drawButtons(dl, ctx);
        // SCENE BPM selector and FILE.
        dl.fill(kSlot4, p.surface2, tk::radiusM);
        dl.text(kSlot4.x + 96, kSlot4.y + 18, Align::Centre, TextStyleId::Caption, p.text2, "SCENE BPM");
        dl.fill({kSlot4.x + 12, kSlot4.y + 32, 168, 36}, p.surface1, tk::radiusS);
        if (scene.bpm)
            bpm_.format("%.1f", *scene.bpm);
        else
            bpm_.set("---");
        dl.text(kSlot4.x + 96, kSlot4.y + 50, Align::Centre, TextStyleId::BodyStrong, p.text2, bpm_.view());
        Button::draw(dl, ctx, kSlot5, {"FILE"});
    }

    // The overview's trigger bars, rebuilt when the pattern or the model changes (16 × 64 cells, plan "Performance").
    struct Hit
    {
        RectF r;
        std::uint8_t track;
    };
    void rebuildOverview() const
    {
        auto& s = m2::Session::instance();
        const int scene = seqScene(), pattern = seqPattern();
        const auto key = s.revision() * 256u + static_cast<unsigned>(scene * 16 + pattern);
        if (key == overviewKey_)
            return;
        overviewKey_ = key;
        hits_.clear();
        const auto& pat =
            s.project().scenes[static_cast<std::size_t>(scene)].patterns[static_cast<std::size_t>(pattern)];
        const int len = std::min(pat.length, 64);
        for (int t = 0; t < 16; ++t)
        {
            const bool loop = state_->tracks[static_cast<std::size_t>(t)].loopGlyph == "\xE2\x9F\xB2";
            const float ry = kRowY0 + static_cast<float>(t) * kRowH;
            for (int st = 0; st < len; ++st)
            {
                if (pat.steps[static_cast<std::size_t>(st)][static_cast<std::size_t>(t)].trigger ==
                    core::model::Step::kEmpty)
                    continue;
                float w = kStepW - 5;
                if (loop) // loops and long notes span to the next trigger (or the pattern end)
                {
                    int next = st + 1;
                    while (next < len &&
                           pat.steps[static_cast<std::size_t>(next)][static_cast<std::size_t>(t)].trigger ==
                               core::model::Step::kEmpty)
                        ++next;
                    w = static_cast<float>(next - st) * kStepW - 4;
                }
                hits_.push_back(
                    {{kGx + static_cast<float>(st) * kStepW + 2, ry + 4, w, 10}, static_cast<std::uint8_t>(t)});
            }
        }
    }

    void drawSeqTab(DrawList& dl, const UiContext& ctx) const
    {
        const auto& p = ctx.palette;
        const int scene = seqScene(), pattern = seqPattern();
        // Source label: a strip-tab-shaped trapezoid 216 × 24 (sloped right edge drawn in 2 lpx columns).
        dl.fill({224, 152, 200, 24}, p.surface2);
        for (int i = 0; i < 8; ++i)
            dl.fill({424 + 2 * static_cast<float>(i), 152 + 3 * static_cast<float>(i) + 1.5f, 2,
                     24 - 3 * static_cast<float>(i) - 1.5f},
                    p.surface2);
        line_.format("SCENE %02d \xC2\xB7 PATTERN %02d", scene + 1, pattern + 1);
        dl.text(236, 164, Align::Left, TextStyleId::Caption, p.text2, line_.view());
        dl.fill({224, 176, 1040, 336}, p.surface1, tk::radiusM);
        const int bars = seqBars();
        for (int bar = 0; bar < 4; ++bar)
        {
            const float bx = kGx + 240 * static_cast<float>(bar);
            const bool lit = bar < bars;
            dl.fill({bx, 180, 240, 28}, lit ? p.surface3 : p.surface2);
            for (int beat = 0; beat < 4; ++beat)
            {
                dl.text(bx + 60 * static_cast<float>(beat) + 6, 194, Align::Left, TextStyleId::Caption,
                        lit ? (beat == 0 ? p.text1 : p.text2) : p.text3, kBeatLabels[bar * 4 + beat]);
                const float lx = bx + 60 * static_cast<float>(beat);
                if (beat == 0)
                    dl.fill({lx - 1, 212, 2, 288}, p.borderStrong);
                else
                    dl.fill({lx - 0.5f, 212, 1, 288}, p.border);
            }
        }
        dl.fill({kGx + 960 - 1, 212, 2, 288}, p.borderStrong);
        for (int t = 1; t <= 16; ++t)
        {
            const float ry = kRowY0 + static_cast<float>(t - 1) * kRowH;
            const bool has = state_->tracks[static_cast<std::size_t>(t - 1)].hasContent;
            dl.fill({232, ry + 2, 32, 14}, has ? theme::trackColour(t) : theme::trackColour(t, true), 3);
            dl.text(248, ry + 9, Align::Centre, TextStyleId::Caption, has ? p.onAccent : p.text1, kNumbers[t - 1]);
            if (t % 2 == 0)
                dl.fill({kGx, ry, 960, kRowH}, theme::withOpacity(p.surface2, 0.35f));
        }
        rebuildOverview();
        for (const auto& h : hits_)
            dl.fill(h.r, theme::trackColour(h.track + 1), 2);
        for (int bar = bars; bar < 4; ++bar) // beyond the pattern length: 50 % bg over the rows
            dl.fill({kGx + 240 * static_cast<float>(bar), 212, 240, 288}, theme::withOpacity(p.bg, 0.5f));
        // Selection.
        const auto [start, len] = seqSelection();
        const float sx = kGx + 240 * static_cast<float>(start), sw = 240 * static_cast<float>(len);
        const float sy = seqAll() ? kRowY0 : kRowY0 + static_cast<float>(seqTrack()) * kRowH;
        const float sh = seqAll() ? 16 * kRowH : kRowH;
        dl.stroke({sx - 1, sy - 1, sw + 2, sh + 2}, p.accent, 2, 3);
        dl.fill({sx + sw - 30, sy - 14, 30, 12}, p.accent, 3);
        dl.text(sx + sw - 15, sy - 8, Align::Centre, TextStyleId::Caption, p.onAccent, "\xE2\x80\xB9 \xE2\x80\xBA");
        drawButtons(dl, ctx);
    }

    void trackContent()
    {
        auto& s = m2::Session::instance();
        trackMask_ = 0;
        for (int t = 0; t < 16; ++t)
        {
            bool has = focusScene_ == state_->scene && state_->tracks[static_cast<std::size_t>(t)].hasContent;
            for (int pt = 0; pt < 16 && ! has; ++pt)
                has = s.trackHasTriggers(focusScene_ - 1, pt, t);
            if (has)
                trackMask_ = static_cast<std::uint16_t>(trackMask_ | 1u << t);
        }
        overviewRevision_ = s.revision() + 1000u * static_cast<unsigned>(focusScene_);
    }
    bool trackHasContent(int track0) const { return (trackMask_ >> track0 & 1u) != 0; }

    void drawTrackTab(DrawList& dl, const UiContext& ctx) const
    {
        const auto& p = ctx.palette;
        caption(dl, ctx, 224, 312, 504, "SCENE");
        for (int n = 1; n <= 16; ++n)
            SceneKey::draw(dl, ctx, trackTabScene(n), sceneKeyModel(n, ctx.nowMs, true));
        caption(dl, ctx, 744, 168, 520, "TRACK");
        for (int t = 1; t <= 16; ++t)
        {
            const auto r = trackCell(t);
            const bool has = trackHasContent(t - 1);
            dl.fill(r, has ? theme::trackColour(m1::padColour(*shell_, t)) : p.surface2, tk::radiusS);
            dl.text(r.x + 12, r.y + 16, Align::Left, TextStyleId::BodyStrong, has ? p.onAccent : p.text2,
                    kNumbers[t - 1]);
            if (t == chosenTrack_)
            {
                draw::focusRing(dl, p, r, tk::radiusS);
                dl.fill({r.x + 92, r.y + 44, 22, 20}, p.onAccent, tk::radiusXs);
                glyph::line(dl, r.x + 97, r.y + 54, r.x + 101, r.y + 58, 2, p.text1);
                glyph::line(dl, r.x + 101, r.y + 58, r.x + 109, r.y + 50, 2, p.text1);
            }
        }
        drawButtons(dl, ctx);
        // INCLUDE SEQUENCE: an LED toggle with a two-line label (kit led_toggle).
        const bool on = memory().includeSequence;
        dl.fill(kSlot5, p.surface2, tk::radiusS);
        dl.fill({kSlot5.x + 12, kSlot5.y + 7, kSlot5.w - 24, 4}, on ? p.accent : p.borderStrong, 2);
        dl.text(kSlot5.x + kSlot5.w / 2, kSlot5.y + kSlot5.h / 2 - 4, Align::Centre, TextStyleId::Label, p.text1,
                "INCLUDE");
        dl.text(kSlot5.x + kSlot5.w / 2, kSlot5.y + kSlot5.h / 2 + 12, Align::Centre, TextStyleId::Label, p.text1,
                "SEQUENCE");
    }

    ShellApi* shell_;
    UiState* state_;
    int level_ = 0;
    int focusScene_ = 1, focusPattern_ = 1;
    int chosenTrack_ = 0;
    int seqStartBar_ = 0;
    bool draggingSelection_ = false;
    std::pair<int, int> pending_{0, 0};
    const StripSpec* boundStrip_ = nullptr;
    float colourWritten_ = -1, nameValue_ = 0;
    bool nameSeen_ = false;
    int mirrorScene_ = 0;
    std::uint16_t trackMask_ = 0;
    unsigned overviewRevision_ = 0;
    mutable unsigned overviewKey_ = ~0u;
    mutable std::vector<Hit> hits_;
    mutable FixedText<96> info1_, info2_, line_;
    mutable FixedText<12> bpm_;
};

std::unique_ptr<Widget> makeSceneManager(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<SceneManagerBody>(a);
}
} // namespace

bool sceneManagerView(Widget* body, SceneManagerView& out)
{
    auto* b = dynamic_cast<SceneManagerBody*>(body);
    if (b == nullptr)
        return false;
    out = b->view();
    return true;
}
SceneKeyModel sceneManagerKey(Widget* body, bool patternRow, int n, double nowMs)
{
    auto* b = dynamic_cast<SceneManagerBody*>(body);
    return b != nullptr ? b->key(patternRow, n, nowMs) : SceneKeyModel{};
}
void setSceneQueueHandledByEngine(int forced)
{
    memory().forcedQueue = forced;
}
void resetSceneManager()
{
    const int forced = memory().forcedQueue;
    core::edit::resetSharedEditor(); // the session clipboard
    memory() = Memory{};
    memory().forcedQueue = forced;
}
} // namespace ui

APP_REGISTER_WIDGET("SceneManager", &ui::makeSceneManager);
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "sceneManager",
                                    .title = "SCENE MANAGER",
                                    .parent = "home",
                                    .body = ui::BodyTemplate::RailContent,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = ui::kTabs,
                                    .bodyComponent = "SceneManager",
                                    .strip = ui::kPatternStrips[0]}));
