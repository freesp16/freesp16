// SEQUENCE (07-ui-design/screens/sequence.md; UI-245…249, UI-345, UID-750…761). Template grid, context bar
// trackContext, no rail: StepGrid + StepRuler of the selected track's shown bar, then TRIGGER TYPE (seq.triggerType),
// BAR (follows the bar keys; bars past the pattern length disabled), CLEAR TRIGGERS and QUANTIZE; knob strip
// OFF SET · RETRIGGER CNT · RETRIGGER SPD | SHIFT · SHIFT AREA · TRANSPOSE (separator after column 3).
//   - Step keys toggle target steps (UI-246; cleared on leaving: the targets live in the body). Holding a step key and
//     turning a lockable column still locks (UID-759, the shell's hold + turn); a release after such a turn does not
//     toggle the target. A tap on a cell toggles its target; a long-press (≥ 500 ms) holds the step.
//   - Columns 1–3 act on the targets (UID-755, SEQ-038): while targets exist the columns show the targets' value
//     (`—` when they differ, UID-760) and a turn writes the targets' locks through core::perf::setStepsParam (offsets
//     relative to each target — one setStepsParam per resulting value, grouped into one undo step — counts and
//     speeds absolute); with no target the columns show and edit the track values (model-backed seq.*, the same
//     "scene[s].track[t].seq.*" fields setStepsParam writes with an empty mask), which apply to every unlocked step.
//     The body takes the turns through the shell's StripOwner hook.
//   - SHIFT (column 4, relative) rotates the track's triggers inside SHIFT AREA (BAR = the shown bar, ALL = the
//     pattern) one step per detent (SEQ-036) with core::perf::shiftSteps (locks travel with the cells,
//     consecutive detents one undo step).
//   - CLEAR TRIGGERS asks (destructive Message) and is undoable; QUANTIZE zeroes the targets' offsets (all steps of the
//     bar when none) without asking, undoable, with a Toast (UID-758).
// Edits go through core::commands::sequencer on the ui::m2 session's bus (M2Session.h); the shown bar is
// ui::input::steps::shownBar() (the step keys' controller locks and toggles on the same bar).
#include "core/commands/CommandGroup.h"
#include "core/commands/perf/ShiftSteps.h"
#include "core/commands/sequencer/SequencerCommands.h"
#include "ui/input/steps/StepController.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/screens/sequence/M2Commands.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/screens/sequence/SequenceAccess.h"
#include "ui/shell/Feeds.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/Segmented.h"
#include "ui/widgets/StepGrid.h"
#include "ui/widgets/StepRuler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;

constexpr RectF kTriggerType{16, 512, 472, 88}, kBar{504, 512, 488, 88}, kClear{1008, 512, 120, 88},
    kQuantize{1144, 512, 120, 88};
constexpr std::string_view kTypes[] = {"FULL", "HALF", "PARAM"};
constexpr std::string_view kBars[] = {"BAR1", "BAR2", "BAR3", "BAR4"};
constexpr const char* kColumnIds[3] = {"seq.offset", "seq.retrigCount", "seq.retrigSpeed"};
constexpr core::model::LockSlot kColumnSlots[3] = {m2::kSlotOffset, m2::kSlotRetrigCount, m2::kSlotRetrigSpeed};
constexpr double kLongPressMs = 500;

void toast(ShellApi& shell, std::string_view text)
{
    ToastRequest t;
    t.text.assign(text);
    shell.showToast(std::move(t));
}

int segmentAt(RectF r, int count, float x, float y) noexcept
{
    if (! contains(r, x, y) || count <= 0)
        return -1;
    return std::clamp(static_cast<int>((x - r.x) / (r.w / static_cast<float>(count))), 0, count - 1);
}

class SequenceBody final : public Widget, public StripOwner
{
public:
    explicit SequenceBody(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state)
    {
        for (int k = 0; k < 3; ++k)
            specs_[static_cast<std::size_t>(k)] = a.params != nullptr ? a.params->find(kColumnIds[k]) : nullptr;
        shiftSpec_ = a.params != nullptr ? a.params->find("seq.shift") : nullptr;
        areaSpec_ = a.params != nullptr ? a.params->find("seq.shiftArea") : nullptr;
        typeSpec_ = a.params != nullptr ? a.params->find("seq.triggerType") : nullptr;
        input::steps::setShownBar(0); // the body opens on BAR1
    }

    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    // The grid model now (also read by the tests).
    const StepGridModel& model(double nowMs) const
    {
        auto& m = model_;
        if (shell_ == nullptr || state_ == nullptr)
            return m;
        auto& s = m2::Session::instance();
        const int scene = state_->scene - 1, pattern = state_->pattern - 1, t = state_->selectedTrack;
        m.track = t;
        m.length = s.patternLength(scene, pattern);
        const int bars = std::max(1, (m.length + 15) / 16);
        if (bar_ > bars)
            bar_ = bars;
        m.bar = bar_;
        m.colour = theme::trackColour(m1::padColour(*shell_, t));
        m.muted = state_->tracks[static_cast<std::size_t>((t - 1) & 15)].muted;
        for (int n = 1; n <= 16; ++n)
            m.cells[static_cast<std::size_t>(n - 1)] = s.cell(scene, pattern, t - 1, firstStep() + n - 1);
        m.targets = static_cast<std::uint16_t>(targets_ >> firstStep());
        const bool phase = std::fmod(nowMs, 500.0) < 250.0; // target marks flash at 2 Hz (UID-755)
        m.flashing = phase ? m.targets : 0;
        m.heldStep = state_->stepHeld;
        m.playStep = 0;
        m.playFraction = 0;
        const auto& feeds = shell_->feeds();
        const auto& ph = feeds.playhead();
        if (state_->playing && feeds.live() && ph.scene == scene && ph.pattern == pattern && ph.step / 16 == bar_ - 1)
        {
            m.playStep = ph.step % 16 + 1;
            const double steps = ph.ppq * 4.0;
            m.playFraction = static_cast<float>(steps - std::floor(steps));
        }
        else if (state_->playing && playStepOverride_ > 0)
            m.playStep = playStepOverride_;
        return m;
    }
    int bar() const noexcept { return bar_; }
    std::uint64_t targets() const noexcept { return targets_; }
    int focus() const noexcept { return focus_; }
    void setPlayStepForTests(int n) noexcept { playStepOverride_ = n; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const_cast<SequenceBody*>(this)->sync(ctx.nowMs);
        const auto& m = model(ctx.nowMs);
        StepGrid::draw(dl, ctx, m);
        StepRuler::draw(dl, ctx, m);

        SegmentedModel type{kTypes, triggerType(), 0, "TRIGGER TYPE", 0, focus_ == 0};
        Segmented::draw(dl, ctx, kTriggerType, type);
        const int bars = std::max(1, (m.length + 15) / 16);
        SegmentedModel bar{kBars, bar_ - 1, static_cast<std::uint32_t>(0xF & ~((1u << bars) - 1)),
                           "BAR", 0,        focus_ == 1};
        Segmented::draw(dl, ctx, kBar, bar);
        Button::draw(dl, ctx, kClear, {"CLEAR", ButtonStyle::Plain, false, pressed_ == 2, focus_ == 2, "TRIGGERS"});
        Button::draw(dl, ctx, kQuantize, {"QUANTIZE", ButtonStyle::Plain, false, pressed_ == 3, focus_ == 3});
    }

    bool dirty(double) const override { return targets_ != 0 || (state_ != nullptr && state_->playing); }

    bool handle(const io::ControlEvent& e, UiContext& ctx) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (io::isStep(e.control) && (e.kind == Kind::Press || e.kind == Kind::Release))
        {
            stepKey(io::indexOf(e.control), e.kind == Kind::Press);
            return true;
        }
        if (e.kind == Kind::Press && e.control >= io::Control::Bar1 && e.control <= io::Control::Bar4)
        {
            selectBar(static_cast<int>(e.control) - static_cast<int>(io::Control::Bar1) + 1);
            return true;
        }
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            focus_ = std::clamp(focus_ + e.value, 0, 3); // TRIGGER TYPE → BAR → CLEAR → QUANTIZE
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            activate(focus_, -1);
            return true;
        }
        if (touch::isTouch(e))
            return touchAt(e, ctx);
        return false;
    }

private:
    int firstStep() const noexcept { return (bar_ - 1) * 16; }
    int triggerType() const noexcept
    {
        return typeSpec_ != nullptr ? static_cast<int>(std::lround(shell_->values().get(*typeSpec_, 0))) : 0;
    }
    core::model::TriggerType entryType() const noexcept
    {
        const int t = triggerType();
        return t == 1   ? core::model::TriggerType::Half
               : t == 2 ? core::model::TriggerType::LockOnly
                        : core::model::TriggerType::Full;
    }

    // --- step keys / cells ---
    void stepKey(int n, bool down)
    {
        if (down)
        {
            pressStep_ = n;
            for (int k = 0; k < 3; ++k)
                pressLocked_[static_cast<std::size_t>(k)] = lockNow(k, n, pressLock_[static_cast<std::size_t>(k)]);
            return;
        }
        if (n != pressStep_)
            return;
        pressStep_ = 0;
        for (int k = 0; k < 3; ++k) // hold + turn made a lock: no target toggle (UID-759)
        {
            float v = 0;
            const bool locked = lockNow(k, n, v);
            if (locked != pressLocked_[static_cast<std::size_t>(k)] ||
                (locked && v != pressLock_[static_cast<std::size_t>(k)]))
                return;
        }
        toggleTarget(n);
    }
    bool lockNow(int k, int n, float& v) const
    {
        const auto* spec = specs_[static_cast<std::size_t>(k)];
        return spec != nullptr && shell_->values().lockOf(*spec, state_->trackScope(), n, v);
    }
    void toggleTarget(int n) { targets_ ^= std::uint64_t{1} << (firstStep() + n - 1); }
    void selectBar(int b)
    {
        const int bars =
            std::max(1, (m2::Session::instance().patternLength(state_->scene - 1, state_->pattern - 1) + 15) / 16);
        if (b >= 1 && b <= bars)
        {
            bar_ = b; // bars past the length stay disabled (UID-754)
            input::steps::setShownBar(b - 1);
        }
    }

    // --- bottom row ---
    void activate(int control, int segment)
    {
        switch (control)
        {
        case 0:
            if (typeSpec_ != nullptr)
                shell_->values().set(*typeSpec_, 0,
                                     static_cast<float>(segment >= 0 ? segment : (triggerType() + 1) % 3));
            break;
        case 1:
        {
            const int bars =
                std::max(1, (m2::Session::instance().patternLength(state_->scene - 1, state_->pattern - 1) + 15) / 16);
            selectBar(segment >= 0 ? segment + 1 : bar_ % bars + 1);
            break;
        }
        case 2:
            askClear();
            break;
        case 3:
            quantize();
            break;
        default:
            break;
        }
        focus_ = control;
    }

    // The steps an action works on: the targets of the shown bar, else every step of the bar.
    int barSteps(std::array<int, 16>& out, bool targetsOnly) const
    {
        int count = 0;
        for (int n = 0; n < 16; ++n)
        {
            const int s = firstStep() + n;
            if (! targetsOnly || (targets_ >> s & 1u) != 0)
                out[static_cast<std::size_t>(count++)] = s;
        }
        return count;
    }

    void askClear()
    {
        auto& s = m2::Session::instance();
        const int scene = state_->scene - 1, pattern = state_->pattern - 1, track = state_->selectedTrack - 1;
        std::array<int, 16> steps{};
        const bool onlyTargets = (targets_ >> firstStep() & 0xFFFFu) != 0;
        const int count = barSteps(steps, onlyTargets);
        int triggers = 0;
        std::vector<int> list;
        for (int i = 0; i < count; ++i)
            if (s.cell(scene, pattern, track, steps[static_cast<std::size_t>(i)]).kind != StepKind::Empty)
            {
                ++triggers;
                list.push_back(steps[static_cast<std::size_t>(i)]);
            }
        if (triggers == 0)
        {
            toast(*shell_, "No triggers to clear");
            return;
        }
        char line[96];
        std::snprintf(line, sizeof line, "Clear %d trigger%s on track %d, bar %d?", triggers, triggers == 1 ? "" : "s",
                      track + 1, bar_);
        MessageRequest r;
        r.icon = glyph::Kind::Caution;
        r.title = "CLEAR TRIGGERS";
        r.lines = {line};
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {"CLEAR", ButtonStyle::Destructive}};
        r.defaultButton = 0;
        r.onResult = [scene, pattern, track, list](int button)
        {
            if (button != 1)
                return;
            auto& session = m2::Session::instance();
            session.execute(core::commands::sequencer::clearTriggers(scene, pattern, track, list));
        };
        shell_->showMessage(std::move(r));
    }

    void quantize()
    {
        auto& s = m2::Session::instance();
        const int scene = state_->scene - 1, pattern = state_->pattern - 1, track = state_->selectedTrack - 1;
        std::array<int, 16> steps{};
        const bool onlyTargets = (targets_ >> firstStep() & 0xFFFFu) != 0;
        const int count = barSteps(steps, onlyTargets);
        int moved = 0;
        for (int i = 0; i < count; ++i)
        {
            const auto c = s.cell(scene, pattern, track, steps[static_cast<std::size_t>(i)]);
            moved += c.kind != StepKind::Empty && c.offset24 != 0 ? 1 : 0;
        }
        s.execute(core::commands::sequencer::quantizeOffsets(scene, pattern, track,
                                           std::span<const int>(steps.data(), static_cast<std::size_t>(count)), 1.0f));
        char text[48];
        std::snprintf(text, sizeof text, "Quantized %d trigger%s", moved, moved == 1 ? "" : "s");
        toast(*shell_, text);
    }

    bool touchAt(const io::ControlEvent& e, UiContext&)
    {
        const float x = touch::x(e), y = touch::y(e);
        if (e.value == touch::kUp)
        {
            if (touchStep_ > 0)
            {
                if (heldByTouch_)
                {
                    if (state_->stepHeld == touchStep_)
                        state_->stepHeld = 0;
                }
                else
                    toggleTarget(touchStep_);
            }
            touchStep_ = 0;
            heldByTouch_ = false;
            pressed_ = -1;
            return true;
        }
        if (e.value != touch::kDown)
            return false;
        if (const int n = StepGrid::stepAt(x, y); n > 0)
        {
            touchStep_ = n;
            touchAtMs_ = e.timeMs;
            heldByTouch_ = false;
            return true;
        }
        if (const int seg = segmentAt(kTriggerType, 3, x, y); seg >= 0)
            activate(0, seg);
        else if (const int b = segmentAt(kBar, 4, x, y); b >= 0)
            activate(1, b);
        else if (contains(kClear, x, y))
        {
            pressed_ = 2;
            activate(2, -1);
        }
        else if (contains(kQuantize, x, y))
        {
            pressed_ = 3;
            activate(3, -1);
        }
        else
            return false;
        return true;
    }

    // --- per frame: long-press hold ---
    void sync(double nowMs)
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        if (touchStep_ > 0 && ! heldByTouch_ && nowMs - touchAtMs_ >= kLongPressMs)
        {
            heldByTouch_ = true;
            state_->stepHeld = touchStep_; // a long-press holds the step (sequence.md "Interactions")
        }
    }

    // --- the knob strip (StripOwner): columns 1–3 on the targets, SHIFT ---
public:
    void stripColumn(int column, ParamColumnModel& c) const override
    {
        if (column > 2 || targets_ == 0 || shell_ == nullptr || state_ == nullptr || state_->stepHeld > 0)
            return;
        float first = 0;
        bool mixed = false;
        int n = 0;
        for (int i = 0; i < 64; ++i)
            if ((targets_ >> i & 1u) != 0)
            {
                const float v = targetValue(column, i);
                if (n++ == 0)
                    first = v;
                else if (v != first)
                    mixed = true;
            }
        c.value = first;
        if (mixed)
            c.valueText = "\xE2\x80\x94"; // — (UID-760)
        else if (c.spec != nullptr)
        {
            const int len = c.spec->formatValue(first, columnText_[column], sizeof columnText_[column]);
            c.valueText = std::string_view(columnText_[column], static_cast<std::size_t>(std::max(0, len)));
        }
    }
    long long stripSteps(int column, const core::ParamSpec& spec, bool fine) const override
    {
        if (column <= 2)
            return EncoderAccel::stepsOf(spec, fine);
        return column == 3 && shiftSpec_ != nullptr ? 0 : -1; // SHIFT moves the steps one place per detent
    }
    bool stripTurn(int column, const core::ParamSpec& spec, int detents, bool fine) override
    {
        if (shell_ == nullptr || state_ == nullptr || state_->stepHeld > 0)
            return false; // hold + turn: the shell writes the held step's lock
        const int scene = state_->scene - 1, pattern = state_->pattern - 1, track = state_->selectedTrack - 1;
        auto& s = m2::Session::instance();
        if (column == 3 && shiftSpec_ != nullptr) // SHIFT (SEQ-036)
        {
            const bool all =
                areaSpec_ == nullptr || std::lround(shell_->values().get(*areaSpec_, state_->trackScope())) != 0;
            s.execute(core::perf::shiftSteps(scene, pattern, track,
                                             all ? core::perf::ShiftArea::All : core::perf::ShiftArea::Bar, bar_ - 1,
                                             detents));
            return true;
        }
        if (column > 2 || targets_ == 0)
            return false; // no target: the shell edits the track value
        const auto id = shell_->params().idOf(spec.id);
        if (! id)
            return true;
        // The targets' new values: OFF SET relative to each target's own, counts and speeds stepped from the first.
        std::array<float, 64> next{};
        float first = 0;
        bool seen = false;
        for (int i = 0; i < 64; ++i)
            if ((targets_ >> i & 1u) != 0)
            {
                const float own = targetValue(column, i);
                if (! seen)
                    first = own, seen = true;
                next[static_cast<std::size_t>(i)] = spec.step(column == 0 ? own : first, detents, fine);
            }
        std::vector<std::unique_ptr<core::Command>> cmds;
        std::uint64_t done = 0;
        for (int i = 0; i < 64; ++i)
        {
            if ((targets_ >> i & 1u) == 0 || (done >> i & 1u) != 0)
                continue;
            std::uint64_t mask = 0; // every target that gets the same value: one setStepsParam
            for (int j = i; j < 64; ++j)
                if ((targets_ >> j & 1u) != 0 && next[static_cast<std::size_t>(j)] == next[static_cast<std::size_t>(i)])
                    mask |= std::uint64_t{1} << j;
            done |= mask;
            if (auto cmd = core::perf::setStepsParam(scene, pattern, track, mask, *id, next[static_cast<std::size_t>(i)],
                                                     shell_->params()))
                cmds.push_back(std::move(cmd));
        }
        if (cmds.size() == 1)
            s.execute(std::move(cmds.front()));
        else if (! cmds.empty())
        {
            auto group = std::make_unique<core::CommandGroup>(std::string(spec.label));
            for (auto& c : cmds)
                group->add(std::move(c));
            s.execute(std::move(group));
        }
        return true;
    }

private:
    // Step i's value of column k: its lock, else the track value of the model (SEQ-038; as M2Session::cell).
    float targetValue(int k, int step) const
    {
        auto& s = m2::Session::instance();
        const int scene = state_->scene - 1, pattern = state_->pattern - 1, track = state_->selectedTrack - 1;
        const auto& pat = s.project().scenes[static_cast<std::size_t>(scene)].patterns[static_cast<std::size_t>(pattern)];
        const auto& cell = pat.steps[static_cast<std::size_t>(step)][static_cast<std::size_t>(track)];
        const auto& seq = s.project().scenes[static_cast<std::size_t>(scene)].tracks[static_cast<std::size_t>(track)].seq;
        const int base = k == 0 ? seq.offset : (k == 1 ? seq.retriggerCount : seq.retriggerSpeed);
        return m2::lockValue(pat, cell, kColumnSlots[k], static_cast<float>(base));
    }

    ShellApi* shell_;
    UiState* state_;
    std::array<const core::ParamSpec*, 3> specs_{};
    const core::ParamSpec *shiftSpec_ = nullptr, *areaSpec_ = nullptr, *typeSpec_ = nullptr;
    mutable StepGridModel model_{};
    mutable int bar_ = 1;
    std::uint64_t targets_ = 0;
    int focus_ = 0; // TRIGGER TYPE has the focus on entry (mockup)
    int pressed_ = -1;
    int pressStep_ = 0;
    std::array<bool, 3> pressLocked_{};
    std::array<float, 3> pressLock_{};
    int touchStep_ = 0;
    double touchAtMs_ = 0;
    bool heldByTouch_ = false;
    mutable char columnText_[3][24]{};
    int playStepOverride_ = 0;
};

std::unique_ptr<Widget> makeSequenceBody(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<SequenceBody>(a);
}
} // namespace

// Test access (tests/snapshot/m2): the SEQUENCE body's grid model and targets.
const StepGridModel* sequenceModel(Widget* body, double nowMs)
{
    auto* b = dynamic_cast<SequenceBody*>(body);
    return b != nullptr ? &b->model(nowMs) : nullptr;
}
std::uint64_t sequenceTargets(Widget* body)
{
    auto* b = dynamic_cast<SequenceBody*>(body);
    return b != nullptr ? b->targets() : 0;
}
void sequencePlayStep(Widget* body, int n)
{
    if (auto* b = dynamic_cast<SequenceBody*>(body))
        b->setPlayStepForTests(n);
}
} // namespace ui

APP_REGISTER_WIDGET("StepGrid", &ui::makeSequenceBody);
APP_REGISTER_SCREEN((ui::ScreenSpec{
    .id = "track.sequence",
    .title = "SEQUENCE",
    .parent = "track.menu",
    .body = ui::BodyTemplate::Grid,
    .contextBar = ui::ContextBarKind::TrackContext,
    .rail = {},
    .bodyComponent = "StepGrid",
    .strip = {"SEQUENCE",
              {"seq.offset", "seq.retrigCount", "seq.retrigSpeed", "seq.shift", "seq.shiftArea", "seq.transpose"},
              1u << 3}})); // separator before column 4 (KnobStrip groupMask)
