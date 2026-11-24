// ARRANGER (07-ui-design/screens/arranger.md, UID-575…583; 03-ui/screens.md ARRANGER UI-300…306; ARR-001…
// 012). Template rail+content+meter, context bar menu (ARRANGER lit): rail ARRANGEMENT (toggle, `play` LED) · COPY ·
// PASTE · DELETE, the arrangement DataTable (No., POSITION, LENGTH, SCENE swatch, PATTERN dot, REPEAT, BPM; END OF
// ARRANGEMENT always last), the MASTER meter. Every edit is one core::commands::arrangement command on the document's
// bus (one undo step; knob turns of one field coalesce, EDT-U2).
//   - Rotary: the focus row (no wrap); turning up from row 1 hands the focus to the rail (its last item), turning down
//     past the rail's last item returns to row 1 — so every rail action is reachable without touch (NFR-040).
//     SHIFT + turn moves the focused row (ARR-003; our choice).
//   - Push: insert the current scene/pattern before the focused row (UI-303); SHIFT + push with ARRANGEMENT on: start
//     the song from the focused row (UI-305, `arrangement.start`; the shell's SHIFT + push rail toggle yields to it
//     then, RailOwner::ownsShiftPush).
//   - Knobs 1–4 (arrangement.row.*) show and edit the focused row (UI-301, StripOwner); on END OF ARRANGEMENT they show
//     `–` and the first turn appends a row with the current scene/pattern, REPEAT 1, BPM `--` and the turned value
//     (UI-302). Knobs 5–6 HEADPHONES / MASTER never rebind (UID-578). The strip tab reads `ROW nnn` / `END`.
//   - COPY (SHIFT: duplicate after itself), PASTE (always before the focused row), DELETE (a Toast `Row n deleted`,
//     undoable; SHIFT + DELETE asks before clearing every row, UID-582). The keyboard's ext.copy / ext.paste /
//     ext.clear (⌘C, ⌘V, Delete) do the same on this screen, ext.undo / ext.redo (⌘Z / ⇧⌘Z) undo and redo (our choice).
//   - Playing (core::arrangement::liveSongStatus(), the engine's song position): ▶ + 3 lpx play bar on the playing row,
//     its REPEAT cell `remaining / repeats` in `play` (UID-579); the view follows the playing row unless the focus
//     moved in the last 4 s (UID-583). While the song plays the SCENE | PATTERN context item is greyed (ARR-007).
//   - While a step is held, columns 1–4 are disabled (row values are not lockable).
#include "core/commands/arrangement/ArrangementCommands.h"
#include "core/util/arrangement/ArrangementPlan.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/shell/StripOwner.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/DataTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace ar = ::core::commands::arrangement;
using Kind = io::ControlEvent::Kind;

constexpr DataColumn kColumns[] = {{0, 80, "NO.", {}},          {80, 152, "POSITION", "BAR.STEP"},
                                   {232, 152, "LENGTH", "BAR.STEP"}, {384, 136, "SCENE", {}},
                                   {520, 136, "PATTERN", {}},   {656, 136, "REPEAT", {}},
                                   {792, 144, "BPM", {}}};
constexpr double kFollowAfterMs = 4000; // UID-583
constexpr int kRailPlay = 0, kRailCopy = 1, kRailPaste = 2, kRailDelete = 3;

// The screen's row clipboard (ARR-012: one per document; it survives project changes; our choice).
ar::RowClipboard& clipboard()
{
    static ar::RowClipboard c;
    return c;
}
int& rememberedFocus()
{
    static int f = 0;
    return f;
}

// Two strip specs whose tabs point at their own buffers: rebinding alternates between them, so the shell (which keys
// its binding on the spec's address) takes the new tab text — `ROW 004`, `END`.
struct RowStrip
{
    std::array<char, 20> text {};
    StripSpec spec;
};
std::array<RowStrip, 2>& rowStrips()
{
    static std::array<RowStrip, 2> s {};
    return s;
}

class ArrangerBody final : public Widget, public RailOwner, public StripOwner
{
public:
    explicit ArrangerBody(const WidgetArgs& a) : shell_(a.shell), state_(a.state)
    {
        focus_ = rememberedFocus();
    }
    ~ArrangerBody() override { rememberedFocus() = focus_; }
    RectF bounds() const noexcept override { return {224, L::contentTop, 936, L::contentH}; }
    int focusRow() const noexcept { return focus_; }
    int firstRow() const noexcept { return first_; }

    DataTableModel model(double nowMs) const
    {
        auto& s = m2::Session::instance();
        const auto& p = s.project();
        DataTableModel m;
        m.columns = kColumns;
        m.rows = ar::rowCount(p);
        m.terminal = true;
        m.terminalText = "END OF ARRANGEMENT";
        focus_ = std::clamp(focus_, 0, m.rows);
        m.focused = focus_;
        m.playing = playingRow();
        // UID-583: the view follows the playing row unless the focus moved in the last 4 s.
        if (m.playing >= 0 && nowMs - focusMovedMs_ > kFollowAfterMs)
            first_ = DataTable::scrollFor(m.playing, first_, DataTable::count(m), m.visible);
        else
            first_ = DataTable::scrollFor(focus_, first_, DataTable::count(m), m.visible);
        m.first = first_;
        return m;
    }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& pal = ctx.palette;
        auto& s = m2::Session::instance();
        const auto& p = s.project();
        const auto m = model(ctx.nowMs);
        rebindTab(m.rows);
        if (state_ != nullptr)
            state_->sceneLocked = m.playing >= 0; // ARR-007: manual scene/pattern switching is blocked while it plays
        DataTable::drawFrame(dl, ctx, m);
        const auto& sections = p.arrangement.sections;
        const auto& song = core::arrangement::liveSongStatus();
        std::int64_t start16 = 0;
        for (int r = 0; r < m.first && r < m.rows; ++r)
            start16 += length16(sections[static_cast<std::size_t>(r)]);
        const int last = std::min(DataTable::count(m), m.first + m.visible);
        for (int r = m.first; r < last; ++r)
        {
            const auto rr = DataTable::drawRow(dl, ctx, m, r);
            if (r == m.rows)
            {
                DataTable::drawTerminal(dl, ctx, m, rr);
                continue;
            }
            const auto& sec = sections[static_cast<std::size_t>(r)];
            const bool strong = r == m.focused;
            const auto len = length16(sec);
            auto& t = cells_;
            t[0].format("%03d", r + 1);
            DataTable::cell(dl, m, pal, rr, 0, t[0].view(), 0, strong, 6);
            t[1].format("%03d.%02d", static_cast<int>(start16 / 16) + 1, static_cast<int>(start16 % 16) + 1);
            DataTable::cell(dl, m, pal, rr, 1, t[1].view(), 0, strong);
            t[2].format("%03d.%02d", static_cast<int>(len / 16), static_cast<int>(len % 16));
            DataTable::cell(dl, m, pal, rr, 2, t[2].view(), 0, strong);
            const auto colour = theme::sceneColour((sec.scene & 15) + 1);
            dl.fill({rr.x + 418, rr.y + 12, 20, 20}, colour, tk::radiusXs);
            t[3].format("%02d", (sec.scene & 15) + 1);
            DataTable::cell(dl, m, pal, rr, 3, t[3].view(), 0, strong, 76, Align::Left);
            dl.fill({rr.x + 558, rr.y + 19, 6, 6}, colour, 3);
            t[4].format("%02d", (sec.pattern & 15) + 1);
            DataTable::cell(dl, m, pal, rr, 4, t[4].view(), 0, strong, 4);
            const bool playing = r == m.playing;
            if (playing)
                t[5].format("%d / %d", std::clamp(song.remaining.load(std::memory_order_relaxed), 1, sec.repeats),
                            sec.repeats);
            else
                t[5].format("%d", sec.repeats);
            DataTable::cell(dl, m, pal, rr, 5, t[5].view(), playing ? pal.play : 0, strong);
            if (sec.fixedBpm && *sec.fixedBpm >= 20.0)
                t[6].format("%.1f", *sec.fixedBpm);
            else
                t[6].set("--");
            DataTable::cell(dl, m, pal, rr, 6, t[6].view(), sec.fixedBpm ? 0 : pal.text2, strong);
            start16 += len;
        }
    }

    bool dirty(double) const override { return playingRow() >= 0; }

    bool handle(const io::ControlEvent& e, UiContext& ctx) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        auto& s = m2::Session::instance();
        const int rows = ar::rowCount(s.project());
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (state_->shift && focus_ < rows) // SHIFT + turn: move the focused row (ARR-003; our choice)
            {
                const int to = std::clamp(focus_ + e.value, 0, rows - 1);
                if (to != focus_)
                    if (auto c = ar::moveRow(s.project(), focus_, to))
                    {
                        s.execute(std::move(c));
                        focus_ = to;
                    }
                moved(ctx.nowMs);
                return true;
            }
            const int f = focus_ + e.value;
            if (f < 0)
                return false; // up from row 1: the rail takes the focus (its last item)
            focus_ = std::min(f, rows);
            moved(ctx.nowMs);
            return true;
        }
        if (e.kind != Kind::Press)
            return touch::isTouch(e) && touchAt(e, ctx);
        switch (e.control)
        {
        case io::Control::SelectorPush:
            if (state_->shift)
                startFromFocus();
            else if (auto c = ar::insertCurrentBefore(s.project(), focus_))
            {
                s.execute(std::move(c)); // UI-303: the new row takes the focused index; the focus stays on the row it
                ++focus_;                // was on (END: repeated pushes build the song in order; our choice)
            }
            return true;
        case io::Control::ExtCopy:
            copy();
            return true;
        case io::Control::ExtPaste:
            paste();
            return true;
        case io::Control::ExtClear:
            erase();
            return true;
        case io::Control::ExtUndo:
            s.undo();
            return true;
        case io::Control::ExtRedo:
            s.redo();
            return true;
        default:
            return false;
        }
    }

    // --- RailOwner ---------------------------------------------------------------------------------------------------
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (shell_ == nullptr)
            return true;
        auto& s = m2::Session::instance();
        switch (index)
        {
        case kRailPlay:
            if (auto c = ar::setPlayArrangement(s.project(), ! s.project().settings.playArrangement))
                s.execute(std::move(c)); // UI-300
            break;
        case kRailCopy:
            copy();
            break;
        case kRailPaste:
            paste();
            break;
        case kRailDelete:
            erase();
            break;
        default:
            break;
        }
        return true;
    }
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        auto& s = m2::Session::instance();
        const int rows = ar::rowCount(s.project());
        switch (index)
        {
        case kRailPlay:
            st.on = s.project().settings.playArrangement;
            st.onColour = theme::palette(state_ == nullptr || state_->dark).play; // UID-581
            break;
        case kRailCopy:
        case kRailDelete:
            st.disabled = focus_ >= rows && ! (index == kRailDelete && state_ != nullptr && state_->shift && rows > 0);
            break;
        case kRailPaste:
            st.disabled = ! clipboard().hasRow();
            break;
        default:
            break;
        }
    }
    // SHIFT + push = play from the focused row while ARRANGEMENT is on (UI-305; control-surface.md SHIFT table, M p.26);
    // with it off the shell's SHIFT + push rail toggle applies (our choice).
    bool ownsShiftPush() const override { return m2::Session::instance().project().settings.playArrangement; }
        bool railFocusEnter(int detents) override
    {
        focus_ = std::max(0, detents - 1); // past DELETE: row 1, then on
        focusMovedMs_ = lastNowMs_;
        return true;
    }

    // --- StripOwner --------------------------------------------------------------------------------------------------
    void stripColumn(int column0, ParamColumnModel& c) const override
    {
        if (column0 > 3 || c.spec == nullptr)
            return;
        auto& s = m2::Session::instance();
        const auto& p = s.project();
        const int rows = ar::rowCount(p);
        if (state_ != nullptr && state_->stepHeld > 0)
            c.disabled = true; // row values are not lockable
        c.stepped = column0 < 2;
        if (focus_ >= rows)
        {
            c.valueText = "\xE2\x80\x93"; // – : END OF ARRANGEMENT shows the captions only (UI-302)
            c.inactive = true;
            c.value = c.spec->minValue;
            return;
        }
        const auto field = static_cast<ar::RowField>(column0);
        const float v = ar::rowValue(p, focus_, field);
        c.value = v;
        auto& t = strip_[static_cast<std::size_t>(column0)];
        switch (field)
        {
        case ar::RowField::Scene:
        case ar::RowField::Pattern:
            t.format("%02d", static_cast<int>(std::lround(v)) + 1);
            break;
        case ar::RowField::Repeat:
            t.format("%d", static_cast<int>(std::lround(v)));
            break;
        case ar::RowField::Bpm:
            if (v >= 20.0f)
                t.format("%.1f", static_cast<double>(v));
            else
                t.set("--");
            break;
        }
        c.valueText = t.view();
    }
    long long stripSteps(int column0, const core::ParamSpec& spec, bool fine) const override
    {
        if (column0 > 3)
            return -1;
        // BPM only: SCENE, PATTERN and REPEAT walk one by one.
        return static_cast<ar::RowField>(column0) == ar::RowField::Bpm ? EncoderAccel::stepsOf(spec, fine) : 0;
    }
    bool stripTurn(int column0, const core::ParamSpec& spec, int detents, bool fine) override
    {
        if (column0 > 3 || shell_ == nullptr)
            return false;
        if (state_ != nullptr && state_->stepHeld > 0)
            return true;
        auto& s = m2::Session::instance();
        const auto& p = s.project();
        const int rows = ar::rowCount(p);
        const auto field = static_cast<ar::RowField>(column0);
        const bool end = focus_ >= rows;
        const double now = end ? defaultOf(field, p) : static_cast<double>(ar::rowValue(p, focus_, field));
        const double v = stepped(field, now, detents, fine, p, spec);
        if (end)
        {
            auto row = ar::currentRow(p); // UI-302: the current scene/pattern, REPEAT 1, BPM "--", then the turned value
            apply(row, field, v);
            if (auto c = ar::appendRow(p, row))
            {
                s.execute(std::move(c));
                ++focus_; // the focus stays on END OF ARRANGEMENT
            }
        }
        else if (auto c = ar::editRowField(p, focus_, field, v))
            s.execute(std::move(c));
        return true;
    }

private:
    static std::int64_t length16(const core::model::Section& sec)
    {
        return static_cast<std::int64_t>(std::max(1, sec.repeats)) *
               m2::Session::instance().patternLength(sec.scene & 15, sec.pattern & 15);
    }
    static int playingRow() noexcept
    {
        const int r = core::arrangement::liveSongStatus().row.load(std::memory_order_relaxed);
        return r >= 0 && r < ar::rowCount(m2::Session::instance().project()) ? r : -1;
    }
    static double defaultOf(ar::RowField f, const core::model::Project& p)
    {
        const auto row = ar::currentRow(p);
        switch (f)
        {
        case ar::RowField::Scene:
            return row.scene;
        case ar::RowField::Pattern:
            return row.pattern;
        case ar::RowField::Repeat:
            return 1;
        case ar::RowField::Bpm:
            return 0;
        }
        return 0;
    }
    static double stepped(ar::RowField f, double now, int d, bool fine, const core::model::Project& p,
                          const core::ParamSpec& spec)
    {
        switch (f)
        {
        case ar::RowField::Scene:
        case ar::RowField::Pattern:
            return std::clamp(now + d, 0.0, 15.0);
        case ar::RowField::Repeat:
            return std::clamp(now + d, 1.0, static_cast<double>(ar::kMaxRepeats));
        case ar::RowField::Bpm:
        {
            // "--" (below 20) turned up starts at the tempo in force; turned below 20 it becomes "--" (ARR-002).
            if (now < 20.0)
                return d > 0 ? std::clamp(std::round(p.settings.tempo * 10.0) / 10.0, 20.0, 300.0) : 0.0;
            const double step = fine ? spec.fineStep : spec.coarseStep;
            const double v = std::round((now + d * step) * 10.0) / 10.0;
            return v < 20.0 ? 0.0 : std::min(v, 300.0);
        }
        }
        return now;
    }
    static void apply(core::model::Section& row, ar::RowField f, double v)
    {
        switch (f)
        {
        case ar::RowField::Scene:
            row.scene = static_cast<std::uint8_t>(std::lround(v) & 15);
            break;
        case ar::RowField::Pattern:
            row.pattern = static_cast<std::uint8_t>(std::lround(v) & 15);
            break;
        case ar::RowField::Repeat:
            row.repeats = static_cast<int>(std::lround(v));
            break;
        case ar::RowField::Bpm:
            if (v >= 20.0)
                row.fixedBpm = v;
            else
                row.fixedBpm.reset();
            break;
        }
    }
    void rebindTab(int rows) const
    {
        const int shown = focus_ >= rows ? 0 : focus_ + 1;
        if (shell_ == nullptr || shown == tabShown_)
            return;
        tabShown_ = shown;
        auto& strips = rowStrips();
        auto& next = strips[static_cast<std::size_t>(tabSlot_ ^= 1)];
        if (shown == 0)
            std::snprintf(next.text.data(), next.text.size(), "END");
        else
            std::snprintf(next.text.data(), next.text.size(), "ROW %03d", shown);
        next.spec = shell_->current().strip;
        next.spec.tab = next.text.data();
        shell_->bindStrip(&next.spec);
    }
    void moved(double nowMs)
    {
        focusMovedMs_ = nowMs;
        lastNowMs_ = nowMs;
    }
    void copy()
    {
        auto& s = m2::Session::instance();
        if (state_ != nullptr && state_->shift) // SHIFT + COPY: duplicate the focused row directly after itself
        {
            if (auto c = ar::duplicateRow(s.project(), focus_))
                s.execute(std::move(c));
            return;
        }
        if (clipboard().copy(s.project(), focus_))
            toast("Row copied");
    }
    void paste()
    {
        auto& s = m2::Session::instance();
        if (auto c = clipboard().pasteBefore(s.project(), focus_))
        {
            s.execute(std::move(c)); // UI-304: always inserts before the focused row (the focus stays on that row)
            ++focus_;
        }
    }
    void erase()
    {
        auto& s = m2::Session::instance();
        const int rows = ar::rowCount(s.project());
        if (state_ != nullptr && state_->shift)
        {
            if (rows == 0)
                return;
            MessageRequest req; // UID-582: clearing the whole arrangement asks first
            req.icon = glyph::Kind::Caution;
            req.title = "Clear arrangement";
            req.lines = {"Delete all " + std::to_string(rows) + (rows == 1 ? " row" : " rows") +
                         " of the arrangement?"};
            req.buttons = {{"CANCEL", ButtonStyle::Plain}, {"DELETE ALL", ButtonStyle::Destructive}};
            req.defaultButton = 0;
            req.onResult = [](int b)
            {
                auto& session = m2::Session::instance();
                if (b == 1)
                    if (auto c = ar::clearArrangement(session.project()))
                        session.execute(std::move(c));
            };
            shell_->showMessage(std::move(req));
            return;
        }
        if (focus_ >= rows)
            return;
        if (auto c = ar::deleteRow(s.project(), focus_))
        {
            const int n = focus_ + 1;
            s.execute(std::move(c)); // undoable (EDT-007): the Toast says so, ⌘Z undoes it
            toast("Row " + std::to_string(n) + " deleted");
        }
    }
    void startFromFocus()
    {
        auto& s = m2::Session::instance();
        if (! s.project().settings.playArrangement || focus_ >= ar::rowCount(s.project()))
            return;
        if (const auto* spec = shell_->params().find("arrangement.start"))
            shell_->values().set(*spec, 0, static_cast<float>(focus_)); // UI-305: the engine cues the row
        core::arrangement::liveSongStatus().cued.store(focus_, std::memory_order_relaxed);
    }
    void toast(std::string text)
    {
        ToastRequest t;
        t.text = std::move(text);
        shell_->showToast(std::move(t));
    }
    bool touchAt(const io::ControlEvent& e, UiContext& ctx)
    {
        if (e.value != touch::kDown)
            return true;
        const auto m = model(ctx.nowMs);
        const int r = DataTable::rowAt(m, touch::x(e), touch::y(e));
        if (r < 0)
            return false;
        focus_ = r; // a tap focuses the row (one level only)
        moved(ctx.nowMs);
        return true;
    }

    ShellApi* shell_;
    UiState* state_;
    mutable int focus_ = 0, first_ = 0;
    double focusMovedMs_ = -1e12, lastNowMs_ = 0;
    mutable std::array<FixedText<16>, 7> cells_ {};
    mutable std::array<FixedText<12>, 4> strip_ {};
    mutable int tabShown_ = -1, tabSlot_ = 0;
};

std::unique_ptr<Widget> makeArranger(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<ArrangerBody>(a);
}

constexpr RailItemSpec kRail[] = {{RailItemSpec::Kind::Toggle, "ARRANGEMENT"},
                                  {RailItemSpec::Kind::Button, "COPY"},
                                  {RailItemSpec::Kind::Button, "PASTE"},
                                  {RailItemSpec::Kind::Button, "DELETE"}};
} // namespace

// Tests: the ARRANGER body's focus row and first visible row (−1 = not the ARRANGER).
int arrangerFocus(const Widget* body) noexcept
{
    const auto* b = dynamic_cast<const ArrangerBody*>(body);
    return b != nullptr ? b->focusRow() : -1;
}
int arrangerFirstRow(const Widget* body) noexcept
{
    const auto* b = dynamic_cast<const ArrangerBody*>(body);
    return b != nullptr ? b->firstRow() : -1;
}
void resetArranger() noexcept
{
    rememberedFocus() = 0;
    clipboard().clear();
}
} // namespace ui

APP_REGISTER_WIDGET("DataTable", &ui::makeArranger)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "arranger",
                                    .title = "ARRANGER",
                                    .parent = "home",
                                    .body = ui::BodyTemplate::RailContentMeter,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = ui::kRail,
                                    .bodyComponent = "DataTable",
                                    .strip = {"ROW 001",
                                              {"arrangement.row.scene", "arrangement.row.pattern",
                                               "arrangement.row.repeat", "arrangement.row.bpm",
                                               "mixer.headphoneVolume", "mixer.masterVolume"},
                                              1u << 4},
                                    .meter = "master"}))
